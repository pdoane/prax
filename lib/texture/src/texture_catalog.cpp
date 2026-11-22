// The texture catalog. It maps the names of resources to their sources, without holding a reference to the sources.

#include "pr/texture_catalog.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/thread.h"

#include "texture/src/texture_source.h"

namespace pr_impl {

namespace {

// The entry for one name. It has the source of the resource, the pending open that creates the source, or the status of
// a failed open.
struct CatalogEntry {
    uint32_t hash;
    char* name;
    size_t nameSize;
    PrTextureCatalogState state;
    PrStatus status;                    // the status of a failed open
    bool asked;                         // true if an ask found the entry pending
    bool forgotten;                     // true if the entry is pending and removed from the index; its open frees it
    uint64_t lastAsked;                 // the value of `trims` at the last ask for the name

    // The source of a Ready entry. The name does not hold a reference to it. `held` is true while the entry holds the
    // one reference from the open. The entry holds it until the first ask takes the source, or until a trim finds that
    // no ask wants the source.
    const PrTextureSource* PR_NULLABLE source;
    bool held;
};

// An open in flight: the entry of the name that it opens, and the bytes that it reads. `bytes` is null if the open
// reads the file at the name.
struct CatalogJob {
    struct TextureCatalogNames* names;
    CatalogEntry* entry;
    const PrBlobSource* PR_NULLABLE bytes;
};

// The names of a catalog. The catalog and each source that it names hold a reference to the names, so that a source can
// outlive its catalog.
struct TextureCatalogNames {
    Atomic<uint32_t> references;
    PrTextureCatalogDesc desc;
    PrMutex lock;
    PrCondition settled;                // signaled when an open leaves flight
    RawArray<CatalogEntry*> entries;
    HashIndex index;                    // index of `entries` by name
    uint32_t inFlight;
    uint64_t trims;
};

uint32_t hashOf(PrStringView name) {
    return uint32_t(prHashBytes(name.data, name.size, 0));
}

PrStringView nameOf(const CatalogEntry& entry) {
    return PrStringView{entry.name, entry.nameSize};
}

// Returns the position of the entry named `name`, or kHashMissing. Called with the lock held.
uint32_t findName(const TextureCatalogNames& names, uint32_t hash, PrStringView name) {
    return names.index.find(hash, [&](uint32_t at) { return nameOf(*names.entries.data[at]) == name; });
}

void freeEntry(CatalogEntry* entry) {
    prMemoryFree(entry->name);
    prMemoryFree(entry);
}

// Removes the entry at `at` from the names and returns it. Called with the lock held.
CatalogEntry* removeAt(TextureCatalogNames& names, uint32_t at) {
    CatalogEntry* entry = names.entries.data[at];
    names.index.erase(entry->hash, at);
    uint32_t last = names.entries.count - 1;
    if (at != last) {
        CatalogEntry* moved = names.entries.data[last];
        names.index.relocate(moved->hash, last, at);
        names.entries.data[at] = moved;
    }
    names.entries.count = last;
    return entry;
}

void releaseNames(TextureCatalogNames* names) {
    if (names->references.fetchSub(1) != 1)
        return;
    PR_ASSERT(names->entries.count == 0);
    names->entries.reset();
    names->index.reset();
    prMutexReset(&names->lock);
    prConditionReset(&names->settled);
    prMemoryFree(names);
}

// Forgets the name of `source` after its last reference was released. Releases the source's reference to the names.
void sourceReleased(void* PR_NULLABLE context, const PrTextureSource* source) {
    TextureCatalogNames* names = static_cast<TextureCatalogNames*>(context);
    prMutexLock(&names->lock);
    // The name may be gone already. It was forgotten, or an ask found the source released and opened the name again.
    uint32_t at = names->index.find(source->watchHash,
        [&](uint32_t entry) { return names->entries.data[entry]->source == source; });
    if (at != kHashMissing)
        freeEntry(removeAt(*names, at));
    prMutexUnlock(&names->lock);
    releaseNames(names);
}

// Opens the source of the job and gives it to the entry of the job.
void run(void* PR_NULLABLE context) {
    CatalogJob* job = static_cast<CatalogJob*>(context);
    TextureCatalogNames& names = *job->names;
    CatalogEntry* entry = job->entry;
    const PrBlobSource* bytes = job->bytes;
    if (bytes == nullptr)
        bytes = prBlobSourceCreateFile(nameOf(*entry));
    const PrTextureSource* source = nullptr;
    PrStatus status = prTextureFormatSetRead(names.desc.formats, bytes, nameOf(*entry), &names.desc.limits, &source);
    prBlobSourceRelease(bytes);
    prMemoryFree(job);

    prMutexLock(&names.lock);
    bool asked = false;
    const PrTextureSource* unnamed = nullptr;
    if (entry->forgotten) {
        // The name was forgotten while it opened, so nothing names the source.
        unnamed = source;
        freeEntry(entry);
    } else {
        asked = entry->asked;
        entry->asked = false;
        if (status == PrStatus_Ok) {
            names.references.fetchAdd(1);
            textureSourceWatch(source, sourceReleased, &names, entry->hash);
            entry->source = source;
            entry->held = true;
            entry->state = PrTextureCatalogState_Ready;
        } else {
            entry->state = PrTextureCatalogState_Failed;
            entry->status = status;
        }
    }
    // The callback runs before the open leaves flight, so that it finishes before the catalog can be destroyed.
    if (asked && names.desc.opened != nullptr) {
        prMutexUnlock(&names.lock);
        names.desc.opened(names.desc.openedContext);
        prMutexLock(&names.lock);
    }
    --names.inFlight;
    prConditionBroadcast(&names.settled);
    prMutexUnlock(&names.lock);
    prTextureSourceRelease(unnamed);
}

// Returns the state of `name`, and stores its source at `*out` with a reference for the caller. If the names do not
// have `name`, it starts an open from `bytes`, or from the file at the name if `bytes` is null.
PrTextureCatalogState sourceOf(TextureCatalogNames& names, PrStringView name, const PrBlobSource* PR_NULLABLE bytes,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    uint32_t hash = hashOf(name);
    prMutexLock(&names.lock);
    uint32_t at = findName(names, hash, name);
    if (at != kHashMissing) {
        CatalogEntry* entry = names.entries.data[at];
        entry->lastAsked = names.trims;
        if (entry->state == PrTextureCatalogState_Pending) {
            entry->asked = true;
            prMutexUnlock(&names.lock);
            return PrTextureCatalogState_Pending;
        }
        if (entry->state == PrTextureCatalogState_Failed) {
            prMutexUnlock(&names.lock);
            return PrTextureCatalogState_Failed;
        }
        if (entry->held) {
            // The caller takes the reference that the entry held since the open.
            entry->held = false;
            *out = entry->source;
            prMutexUnlock(&names.lock);
            return PrTextureCatalogState_Ready;
        }
        if (textureSourceRetainLive(entry->source)) {
            *out = entry->source;
            prMutexUnlock(&names.lock);
            return PrTextureCatalogState_Ready;
        }
        // The last reference to the source was released, and the release has not reached the names yet. The name opens
        // again.
        freeEntry(removeAt(names, at));
    }
    CatalogEntry* entry = allocZeroed<CatalogEntry>();
    entry->hash = hash;
    entry->name = static_cast<char*>(prMemoryAlloc(name.size + 1));
    if (name.size != 0)
        prMemCopy(entry->name, name.data, name.size);
    entry->nameSize = name.size;
    entry->state = PrTextureCatalogState_Pending;
    entry->lastAsked = names.trims;
    names.index.insert(hash, names.entries.count);
    names.entries.push(entry);
    ++names.inFlight;
    CatalogJob* job = allocZeroed<CatalogJob>();
    job->names = &names;
    job->entry = entry;
    if (bytes != nullptr) {
        prBlobSourceRetain(bytes);
        job->bytes = bytes;
    }
    PrTaskPool* tasks = names.desc.tasks;
    prMutexUnlock(&names.lock);
    if (tasks != nullptr) {
        prTaskPoolPost(tasks, run, job);
        return PrTextureCatalogState_Pending;
    }
    run(job);
    return sourceOf(names, name, bytes, out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextureCatalog : TextureCatalogNames {};

PrTextureCatalogDesc prTextureCatalogDescDefault(const PrTextureFormatSet* formats) {
    return PrTextureCatalogDesc{.formats = formats, .limits = prTextureLimitsDefault()};
}

PrTextureCatalog* prTextureCatalogCreate(const PrTextureCatalogDesc* desc) {
    PrTextureCatalog* catalog = allocZeroed<PrTextureCatalog>();
    catalog->references.storeRelaxed(1);
    catalog->desc = *desc;
    prMutexInit(&catalog->lock);
    prConditionInit(&catalog->settled);
    return catalog;
}

void prTextureCatalogDestroy(PrTextureCatalog* PR_NULLABLE catalog) {
    if (catalog == nullptr)
        return;
    prMutexLock(&catalog->lock);
    while (catalog->inFlight != 0)
        prConditionWait(&catalog->settled, &catalog->lock);
    // The failed entries are freed with the catalog. A live source keeps its name and its reference to the names until
    // it is released.
    RawArray<const PrTextureSource*> held;
    for (uint32_t at = catalog->entries.count; at-- > 0;) {
        CatalogEntry* entry = catalog->entries.data[at];
        if (entry->state == PrTextureCatalogState_Failed) {
            freeEntry(removeAt(*catalog, at));
        } else if (entry->held) {
            entry->held = false;
            held.push(entry->source);
        }
    }
    prMutexUnlock(&catalog->lock);
    for (const PrTextureSource* source : held.span())
        prTextureSourceRelease(source);
    held.reset();
    releaseNames(catalog);
}

PrTextureCatalogState prTextureCatalogSource(PrTextureCatalog* catalog, PrStringView path,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    return sourceOf(*catalog, path, nullptr, out);
}

PrTextureCatalogState prTextureCatalogSourceFromBytes(PrTextureCatalog* catalog, PrStringView name,
        const PrBlobSource* bytes, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    return sourceOf(*catalog, name, bytes, out);
}

PrTextureCatalogState prTextureCatalogWait(PrTextureCatalog* catalog, PrStringView path,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    PrTextureCatalogState state = sourceOf(*catalog, path, nullptr, out);
    while (state == PrTextureCatalogState_Pending) {
        uint32_t hash = hashOf(path);
        prMutexLock(&catalog->lock);
        for (;;) {
            uint32_t at = findName(*catalog, hash, path);
            if (at == kHashMissing || catalog->entries.data[at]->state != PrTextureCatalogState_Pending)
                break;
            prConditionWait(&catalog->settled, &catalog->lock);
        }
        prMutexUnlock(&catalog->lock);
        state = sourceOf(*catalog, path, nullptr, out);
    }
    return state;
}

void prTextureCatalogWaitAll(PrTextureCatalog* catalog) {
    prMutexLock(&catalog->lock);
    while (catalog->inFlight != 0)
        prConditionWait(&catalog->settled, &catalog->lock);
    prMutexUnlock(&catalog->lock);
}

PrStatus prTextureCatalogStatus(PrTextureCatalog* catalog, PrStringView name) {
    prMutexLock(&catalog->lock);
    uint32_t at = findName(*catalog, hashOf(name), name);
    PrStatus status = PrStatus_Ok;
    if (at != kHashMissing && catalog->entries.data[at]->state == PrTextureCatalogState_Failed)
        status = catalog->entries.data[at]->status;
    prMutexUnlock(&catalog->lock);
    return status;
}

void prTextureCatalogForget(PrTextureCatalog* catalog, PrStringView name) {
    const PrTextureSource* held = nullptr;
    prMutexLock(&catalog->lock);
    uint32_t at = findName(*catalog, hashOf(name), name);
    if (at != kHashMissing) {
        CatalogEntry* entry = removeAt(*catalog, at);
        if (entry->state == PrTextureCatalogState_Pending) {
            entry->forgotten = true;
        } else {
            // A live source still tells the names when it is released. The names then find no entry for it.
            if (entry->held)
                held = entry->source;
            freeEntry(entry);
        }
    }
    prMutexUnlock(&catalog->lock);
    prTextureSourceRelease(held);
}

void prTextureCatalogTrim(PrTextureCatalog* catalog) {
    RawArray<const PrTextureSource*> idle;
    prMutexLock(&catalog->lock);
    for (CatalogEntry* entry : catalog->entries.span()) {
        if (entry->held && entry->lastAsked != catalog->trims) {
            entry->held = false;
            idle.push(entry->source);
        }
    }
    ++catalog->trims;
    prMutexUnlock(&catalog->lock);
    // A release calls into the names, which takes the lock, so the sources are released after the unlock.
    for (const PrTextureSource* source : idle.span())
        prTextureSourceRelease(source);
    idle.reset();
}

uint32_t prTextureCatalogNameCount(const PrTextureCatalog* catalog) {
    PrMutex* lock = const_cast<PrMutex*>(&catalog->lock);
    prMutexLock(lock);
    uint32_t count = catalog->entries.count;
    prMutexUnlock(lock);
    return count;
}

// dotLottie: animations and their images in a ZIP archive.
//
// dotlottie - https://dotlottie.io/spec/

#include "pr/lottie_archive.h"

#include "pr/alloc.h"
#include "pr/compress_zip.h"
#include "pr/json.h"
#include "pr/lottie.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"

namespace pr_impl {

namespace {

// An animation in the archive: its id, and the entry that holds it.
struct ArchiveAnimation {
    uint32_t id;                // offset of the id in the archive's text
    uint32_t idSize;
    uint32_t entry;
};

struct LottieArchive {
    PrCompressZip* zip;
    RawArray<char> text;
    RawArray<ArchiveAnimation> animations;
};

// Returns the entry that holds the animation `id`. The entry is in animations/, or in a/ from the second version of the
// format on.
uint32_t animationEntry(const PrCompressZip* zip, PrStringView id) {
    uint32_t count = prCompressZipEntryCount(zip);
    const char* const folders[] = {"animations/", "a/"};
    for (const char* folder : folders) {
        PrStringView folderView = folder;
        RawArray<char> name;
        name.append(folder, uint32_t(folderView.size));
        name.append(id.data, uint32_t(id.size));
        name.append(".json", 5);
        uint32_t entry = prCompressZipFind(zip, PrStringView{name.data, name.count});
        name.reset();
        if (entry < count)
            return entry;
    }
    return count;
}

void addAnimation(LottieArchive& a, PrStringView id, uint32_t entry) {
    ArchiveAnimation animation{a.text.count, uint32_t(id.size), entry};
    a.text.append(id.data, uint32_t(id.size));
    a.animations.push(animation);
}

// Reads the manifest's animations, or every JSON file in an animation folder if the manifest lists none.
void readAnimations(LottieArchive& a) {
    const PrCompressZip* zip = a.zip;
    uint32_t count = prCompressZipEntryCount(zip);
    uint32_t manifest = prCompressZipFind(zip, "manifest.json");
    if (manifest < count) {
        PrCompressZipEntry entry = prCompressZipEntry(zip, manifest);
        RawArray<char> bytes;
        bytes.reserve(uint32_t(entry.size) + 1);
        PrJson* doc = prJsonCreate();
        if (prCompressZipRead(zip, manifest, reinterpret_cast<uint8_t*>(bytes.data), size_t(entry.size)) ==
                PrStatus_Ok && prJsonParse(doc, PrStringView{bytes.data, size_t(entry.size)}, nullptr)) {
            uint32_t list = prJsonMember(doc, 0, "animations");
            for (uint32_t i = prJsonFirst(doc, list); i != kPrJsonMissing; i = prJsonAfter(doc, list, i)) {
                PrStringView id = prJsonString(doc, prJsonMember(doc, i, "id"));
                uint32_t found = id.data != nullptr ? animationEntry(zip, id) : count;
                if (found < count)
                    addAnimation(a, id, found);
            }
        }
        prJsonDestroy(doc);
        bytes.reset();
    }
    if (a.animations.count > 0)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        PrStringView name = prCompressZipEntry(zip, i).name;
        const PrStringView folders[] = {"animations/", "a/"};
        for (PrStringView folder : folders) {
            if (!prStringStartsWith(name, folder) || !prStringEndsWith(name, ".json"))
                continue;
            PrStringView inFolder = prStringFrom(name, folder.size);
            if (prStringFindByte(inFolder, '/') == inFolder.size)
                addAnimation(a, prStringSlice(inFolder, 0, inFolder.size - 5), i);
        }
    }
}

// Files that an animation in the archive names. They are read from the archive and kept until the animation has been
// read.
struct ArchiveFiles {
    const PrCompressZip* zip;
    RawArray<uint8_t*> read;
};

bool loadEntry(void* PR_NULLABLE context, PrStringView path, PrByteView* out) {
    ArchiveFiles* files = static_cast<ArchiveFiles*>(context);
    // Asset folders are named relative to the archive's root, as in "/images/".
    while (path.size > 0 && path.data[0] == '/')
        path = prStringFrom(path, 1);
    uint32_t index = prCompressZipFind(files->zip, path);
    if (index >= prCompressZipEntryCount(files->zip))
        return false;
    PrCompressZipEntry entry = prCompressZipEntry(files->zip, index);
    uint8_t* bytes = static_cast<uint8_t*>(prMemoryAlloc(size_t(entry.size) + 1));
    if (prCompressZipRead(files->zip, index, bytes, size_t(entry.size)) != PrStatus_Ok) {
        prMemoryFree(bytes);
        return false;
    }
    files->read.push(bytes);
    *out = PrByteView{bytes, size_t(entry.size)};
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrLottieArchive : LottieArchive {};

PrLottieArchive* prLottieArchiveCreate(PrCompressCodec* codec) {
    PrLottieArchive* a = allocZeroed<PrLottieArchive>();
    a->zip = prCompressZipCreate(codec);
    return a;
}

void prLottieArchiveDestroy(PrLottieArchive* PR_NULLABLE archive) {
    if (archive == nullptr)
        return;
    prCompressZipDestroy(archive->zip);
    archive->text.reset();
    archive->animations.reset();
    prMemoryFree(archive);
}

PrStatus prLottieArchiveOpen(PrLottieArchive* archive, PrByteView bytes) {
    prLottieArchiveClose(archive);
    PrStatus status = prCompressZipOpen(archive->zip, bytes);
    if (status == PrStatus_Ok)
        readAnimations(*archive);
    return status;
}

void prLottieArchiveClose(PrLottieArchive* archive) {
    prCompressZipClose(archive->zip);
    archive->text.count = 0;
    archive->animations.count = 0;
}

uint32_t prLottieArchiveAnimationCount(const PrLottieArchive* archive) {
    return archive->animations.count;
}

PrStringView prLottieArchiveAnimationId(const PrLottieArchive* archive, uint32_t index) {
    const ArchiveAnimation& animation = archive->animations.data[index];
    return PrStringView{animation.idSize == 0 ? "" : archive->text.data + animation.id, animation.idSize};
}

PrStatus prLottieAnimationCreateFromArchive(const PrLottieArchive* archive, uint32_t index,
        const PrLottieOptions* PR_NULLABLE options, PrLottieAnimation* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    uint32_t entry = archive->animations.data[index].entry;
    ArchiveFiles files{archive->zip, {}};
    PrByteView json{};
    PrStatus status = PrStatus_InvalidData;
    PrCompressZipEntry zipped = prCompressZipEntry(archive->zip, entry);
    if (loadEntry(&files, zipped.name, &json)) {
        PrLottieOptions own = options != nullptr ? *options : PrLottieOptions{};
        own.load = loadEntry;
        own.loadContext = &files;
        status = prLottieAnimationCreate(PrStringView{reinterpret_cast<const char*>(json.data), json.size}, &own, out);
    }
    for (uint32_t i = 0; i < files.read.count; ++i)
        prMemoryFree(files.read.data[i]);
    files.read.reset();
    return status;
}

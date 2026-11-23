// A texture catalog: the sources of named resources, such as the files at paths. A host makes one catalog, and every
// consumer that shows a resource asks it for the resource's source by name, so one resource has one source, and one
// texture in the texture cache (pr/texture_cache.h).
//
// A consumer asks for a name each time it shows the resource (prTextureCatalogSource). The first ask starts opening the
// source: the catalog reads the resource's bytes, and the first format of its set that reads them makes the source
// (prTextureFormatSetOpen). Later asks return the source once it has opened. A source of a file keeps the file's path
// and no bytes, so each of its reads reads the file again.
//
// Lifetime. The catalog names a source without holding it. The source lives while a consumer or a texture cache holds
// it, and the catalog forgets the name when the last of them releases it. A later ask opens the resource again. The
// catalog holds a source only from its open until the first ask takes it, or until a trim finds that no ask wants it
// (prTextureCatalogTrim). A source stays valid after its catalog is destroyed.
//
// Threads. Any thread may ask. The catalog opens sources on the task pool that the host gave it. Without a pool, an
// open starts in the ask that starts it, on the asking thread, and blocks it while bytes that are all there are read.
//
// Arriving bytes. A resource whose bytes are still arriving (prBlobSourceProgress), as from a feed, opens as soon as
// its first bytes describe the texture (prTextureFormatSetOpen), and its source then shows the picture of the bytes so
// far. Until then its name is Pending, and the open completes on the thread that delivers the bytes. The open stays in
// flight until the bytes describe a texture, end or fail.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/task.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The sources of named resources.
typedef struct PrTextureCatalog PrTextureCatalog;

// Where the source of a name is.
enum {
    PrTextureCatalogState_Ready,    // opened: the source is live
    PrTextureCatalogState_Pending,  // opening
    PrTextureCatalogState_Failed,   // its bytes do not read or open, and the catalog does not try again
};
typedef uint8_t PrTextureCatalogState;

// Called with the host's context when a source that an ask found pending opens or fails. It runs on the thread that
// opened the source, and must not call the catalog.
typedef void (*PrTextureCatalogOpenedFn)(void* PR_NULLABLE context);

// How a catalog opens sources.
typedef struct PrTextureCatalogDesc {
    const PrTextureFormatSet* formats;          // outlives the catalog's sources
    PrTaskPool* PR_NULLABLE tasks;              // runs the opens, and outlives the catalog
    PrTextureLimits limits;                     // of each texture that the catalog opens
    PrTextureCatalogOpenedFn PR_NULLABLE opened;
    void* PR_NULLABLE openedContext;
} PrTextureCatalogDesc;

// Returns a description of a catalog of `formats` without a pool, with the default limits.
PrTextureCatalogDesc prTextureCatalogDescDefault(const PrTextureFormatSet* formats);

// Creates a catalog as `desc` describes.
PrTextureCatalog* prTextureCatalogCreate(const PrTextureCatalogDesc* desc);

// Frees the catalog. Its sources stay valid. Opens that have not started are cancelled, and the call waits only for
// opens that are starting or calling the host. An open that waits for bytes that are still arriving completes when they
// do, without a name, and its source goes to no one.
void prTextureCatalogDestroy(PrTextureCatalog* PR_NULLABLE catalog);

// Returns the state of the source of the file at `path`, and starts opening it if the catalog does not name it. Writes
// the source of a Ready name to `*out`, with a reference that the caller releases, and null otherwise. The name is the
// path as it is given: two spellings of one file are two names.
PrTextureCatalogState prTextureCatalogSource(PrTextureCatalog* catalog, PrStringView path,
    const PrTextureSource* PR_NULLABLE* PR_NONNULL out);

// Returns the state of the source named `name` as prTextureCatalogSource does. The first ask of a name reads `bytes`,
// which the source retains, as for a resource of an archive or of the network. `name` identifies the resource, and
// helps to classify it where it ends in an extension (pr/mime_sniff.h).
PrTextureCatalogState prTextureCatalogSourceFromBytes(PrTextureCatalog* catalog, PrStringView name,
    const PrBlobSource* bytes, const PrTextureSource* PR_NULLABLE* PR_NONNULL out);

// Returns the number of opens in flight.
uint32_t prTextureCatalogOpening(const PrTextureCatalog* catalog);

// Returns why the source named `name` failed to open: the status of reading its bytes, NotSupported if no format opens
// them, Limit for a texture past the limits, and InvalidData for one that does not parse. Returns Ok for a name that
// has not failed or that the catalog does not have.
PrStatus prTextureCatalogStatus(PrTextureCatalog* catalog, PrStringView name);

// Forgets `name`, as after its resource changes: the next ask opens the resource again, as a new source. A source that
// the name had stays valid for those that hold it, and its reads read the resource as it is now, which fail if it no
// longer matches the source's description.
void prTextureCatalogForget(PrTextureCatalog* catalog, PrStringView name);

// Marks the end of a frame: releases each source that opened and that no ask since the last trim asked for, which the
// catalog held for the consumer that started its open.
void prTextureCatalogTrim(PrTextureCatalog* catalog);

// Returns the number of names that the catalog has: the live sources, the opens in flight and the failures.
uint32_t prTextureCatalogNameCount(const PrTextureCatalog* catalog);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

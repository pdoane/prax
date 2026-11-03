// Blobs: immutable bytes with a MIME type, and the URLs that name them.
//
// A blob is what a script's bindings share with each other and with the fetcher, which answers its URLs. Native code
// sends a PrByteView and reads a response with prFetchRead.
//
// FileAPI - https://w3c.github.io/FileAPI/

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/fetch.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Immutable bytes with a MIME type, and a name and a modification time if the blob is a file. A blob is reference
// counted. Any thread may read it and hold references to it.
typedef struct PrFetchBlob PrFetchBlob;

// Creates a blob that copies `bytes`. A `type` with a byte outside U+0020 to U+007E is the empty type, and any other is
// lowered (FileAPI/#constructorBlob).
const PrFetchBlob* prFetchBlobCreate(PrByteView bytes, PrStringView type);

// Creates a file, with its name and its modification time in milliseconds since the Unix epoch (FileAPI/#file-section).
const PrFetchBlob* prFetchBlobCreateFile(PrByteView bytes, PrStringView type, PrStringView name, double lastModified);

// Adds or releases a reference. The last release frees the blob.
void prFetchBlobRetain(const PrFetchBlob* blob);
void prFetchBlobRelease(const PrFetchBlob* PR_NULLABLE blob);

// The blob's bytes, valid while the blob lives.
PrByteView prFetchBlobBytes(const PrFetchBlob* blob);

// The blob's MIME type, valid while the blob lives.
PrStringView prFetchBlobType(const PrFetchBlob* blob);

// Returns true if the blob is a file.
bool prFetchBlobIsFile(const PrFetchBlob* blob);

// The name and the modification time of a file.
PrStringView prFetchBlobFileName(const PrFetchBlob* blob);
double prFetchBlobFileLastModified(const PrFetchBlob* blob);

// Creates a blob of the bytes from `start` to `end`, which is never a file. A negative position counts from the blob's
// end, and each is clamped to the blob (FileAPI/#dfn-slice).
const PrFetchBlob* prFetchBlobSlice(const PrFetchBlob* blob, int64_t start, int64_t end, PrStringView type);

// Returns `text` with each of its line endings as the platform writes them (FileAPI/#convert-line-endings-to-native).
PrString prFetchBlobConvertLineEndings(PrStringView text);

// Creates or frees a store of blob URLs (FileAPI/#BlobURLStore). A fetcher's backend holds one. A mutex guards it, so
// any thread may use it.
PrFetchBlobStore* prFetchBlobStoreCreate(void);
void prFetchBlobStoreDestroy(PrFetchBlobStore* PR_NULLABLE store);

// Returns a reference to the blob that `url` names in the store, or null. A URL's fragment is ignored.
const PrFetchBlob* PR_NULLABLE prFetchBlobStoreResolve(PrFetchBlobStore* store, PrStringView url);

// Returns true if `url` has the scheme of a blob URL.
bool prFetchBlobIsUrl(PrStringView url);

// Returns a new blob URL for `origin`, which names nothing until a store's entry gives it an object
// (FileAPI/#unicodeBlobURL).
PrString prFetchBlobUrlGenerate(PrStringView origin);

// Creates a blob URL of `blob` for `origin`, which `fetcher` answers until it is revoked (FileAPI/#url). Any thread may
// call this and prFetchBlobUrlRevoke.
PrString prFetchBlobUrlCreate(PrFetchNetwork* fetcher, const PrFetchBlob* blob, PrStringView origin);

// Stops `fetcher` answering `url`, if `origin` created it (FileAPI/#dfn-revokeObjectURL). The revocation is posted to
// the network's loop, so a fetch of the URL that was posted before it still takes its blob. The fetcher is destroyed by
// a task that is posted to the loop after this call.
void prFetchBlobUrlRevoke(PrFetchNetwork* fetcher, PrStringView url, PrStringView origin);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

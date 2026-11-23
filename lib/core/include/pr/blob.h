// Blobs: immutable bytes shared by reference, which whoever made them frees.
//
// A blob holds bytes and what frees them. Whatever makes bytes hands them out as a blob: a file that the platform maps
// (prFileOpen), a resource of the program (prResourceOpen), or a copy. Whatever keeps bytes retains the blob, rather
// than copying them or holding a mapping of its own, and releases it when it is done. The last release frees the bytes
// as their maker asked, so the holder never knows how they were made.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/notifier.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Bytes and what frees them, shared by reference.
typedef struct PrBlob PrBlob;

// Called with `context` and the bytes of a blob once its last reference is released, on the thread that releases it.
typedef void (*PrBlobReleaseFn)(void* PR_NULLABLE context, PrByteView bytes);

// Returns a blob of `bytes`, which it takes: they stay valid and unchanged until `release` is called with `context` and
// the bytes, once the blob's last reference is released. The caller holds the blob's one reference.
PrBlob* prBlobCreate(PrByteView bytes, PrBlobReleaseFn release, void* PR_NULLABLE context);

// Returns a blob of a copy of `bytes`. The caller holds the blob's one reference.
PrBlob* prBlobCopy(PrByteView bytes);

// Returns the blob's bytes, which stay valid while a reference to the blob is held.
PrByteView prBlobBytes(const PrBlob* blob);

// Takes a reference to the blob. May be called from any thread.
void prBlobRetain(PrBlob* blob);

// Releases a reference to the blob. The last one frees the blob and releases its bytes. May be called from any thread.
void prBlobRelease(PrBlob* PR_NULLABLE blob);

// Returns a blob of the `size` bytes of `blob` from `offset`, clamped to its bytes, which shares them and keeps `blob`
// alive. The caller holds the blob's one reference.
PrBlob* prBlobSlice(PrBlob* blob, uint64_t offset, uint64_t size);

// ---------------------------------------------------------------------------------------------------------------------
// Blob sources
//
// A blob source provides the bytes of one resource, and reads them again each time they are wanted. An object that
// holds a source therefore keeps the means to read the bytes, not the bytes. A source of a file reads the file again. A
// source of an archive entry or a network resource is made by the code that reads it, and that code decides whether to
// keep the bytes, use a cache, or fetch again.
//
// Sources compose. A source of an archive entry reads a range of the archive's source, which may itself be an entry of
// another archive. A source is immutable and reference counted. Any thread may read it.
//
// A read is a request that completes once. It names a range of the bytes, which is usually the whole resource. The
// bytes arrive in a blob, or in storage that the caller provides. The read completes on the thread of its provider.
// That may be the calling thread, during the call: a source of bytes in memory completes at once, and a source of the
// network completes later. The caller learns of the completion from a callback, by polling, or by waiting. Waiting
// blocks the thread. In a program that suspends (JSPI), it suspends the program instead.
//
// Arriving bytes. The bytes of a resource may still be arriving, as from the network or from a host that feeds them
// (PrBlobFeed). Such a source says how many bytes from the start have arrived and whether they are complete or failed
// (prBlobSourceProgress), and notifies its notifier as that changes. A read of it completes once its range has arrived,
// or the bytes end or fail, so a consumer that wants every byte reads it as it reads any source. A consumer that shows
// what it can while the bytes arrive reads the bytes that have arrived, which completes at once, and reads again as
// more arrive. The bytes that have arrived never change, and stay readable if the rest fail.

// A provider of a resource's bytes.
typedef struct PrBlobSource PrBlobSource;

// One read of a source's bytes. A read completes once.
typedef struct PrBlobRead PrBlobRead;

// A size that is not known. As the size of a range, it means every byte up to the end of the resource.
PR_CONSTANT uint64_t kPrBlobSizeUnknown = 0xffffffffffffffffull;

// A range of a resource's bytes: `size` bytes from `offset`. A range that extends past the end of the resource covers
// the bytes up to the end.
typedef struct PrBlobRange {
    uint64_t offset;
    uint64_t size;
} PrBlobRange;

// Returns the range of a whole resource.
PrBlobRange prBlobRangeWhole(void);

// Called once when a read completes, on the thread that completes it.
typedef void (*PrBlobReadDoneFn)(void* PR_NULLABLE context, PrBlobRead* read);

// Begins a read. It is called with the source's `context`. The provider reads the range that the read names
// (prBlobReadRange). It then completes the read (prBlobReadComplete), during the call or later, on any thread.
typedef void (*PrBlobSourceStartFn)(void* PR_NULLABLE context, PrBlobRead* read);

// Called with the source's `context` when the last reference to the source is released, on the releasing thread.
typedef void (*PrBlobSourceReleaseFn)(void* PR_NULLABLE context);

// What has arrived of a resource's bytes.
typedef struct PrBlobProgress {
    uint64_t available;                     // bytes from the start that a read returns without waiting
    PrStatus status;                        // Ok, or why the bytes stopped arriving
    bool complete;                          // no more bytes arrive: every byte is there, or the bytes failed
} PrBlobProgress;

// Returns what has arrived of the bytes. It is called with the source's `context`, on any thread.
typedef PrBlobProgress (*PrBlobSourceProgressFn)(void* PR_NULLABLE context);

// What a source is created with.
typedef struct PrBlobSourceDesc {
    PrBlobSourceStartFn start;
    PrBlobSourceReleaseFn PR_NULLABLE release;
    void* PR_NULLABLE context;              // valid until `release` is called with it
    uint64_t size;                          // of the resource in bytes, or kPrBlobSizeUnknown

    // For bytes that are still arriving: the function that says what has arrived, and the notifier that the provider
    // notifies after more bytes arrive and after they end or fail. The source retains the notifier. Both are null for a
    // resource whose bytes are all there.
    PrBlobSourceProgressFn PR_NULLABLE progress;
    PrNotifier* PR_NULLABLE notifier;
} PrBlobSourceDesc;

// Creates a source. The caller holds the one reference.
PrBlobSource* prBlobSourceCreate(const PrBlobSourceDesc* desc);

// Creates a source of the bytes of `blob`, which the source retains. The bytes stay in memory as long as the source
// lives. Use it for bytes that nothing can read again. Its reads complete during the call.
PrBlobSource* prBlobSourceCreateBlob(PrBlob* blob);

// Creates a source of the file at `path`. Each read opens the file again (prFileOpen) and completes during the call. A
// read fails with Io if the file cannot be opened.
PrBlobSource* prBlobSourceCreateFile(PrStringView path);

// Creates a source of `range` of the bytes of `source`, which the new source retains. A read of the new source reads
// the matching range of `source`.
PrBlobSource* prBlobSourceCreateRange(const PrBlobSource* source, PrBlobRange range);

// Add and release a reference. The last release calls the source's `release` function.
void prBlobSourceRetain(const PrBlobSource* source);
void prBlobSourceRelease(const PrBlobSource* PR_NULLABLE source);

// Returns the size of the resource in bytes, or kPrBlobSizeUnknown if the provider does not know it.
uint64_t prBlobSourceSize(const PrBlobSource* source);

// Returns what has arrived of the source's bytes. A source whose bytes are all there is complete, with its size
// available.
PrBlobProgress prBlobSourceProgress(const PrBlobSource* source);

// Returns the notifier of a source whose bytes are still arriving, which is notified as its progress changes, or null
// for a source whose bytes are all there. It is valid while the source is.
PrNotifier* PR_NULLABLE prBlobSourceNotifier(const PrBlobSource* source);

// What a read asks for. The bytes of `range` go to `storage`, or to a blob if `storage` is null. Storage holds as many
// bytes as the range's size, so a read into storage needs a range of known size. `done` is called when the read
// completes, if it is not null.
typedef struct PrBlobReadDesc {
    PrBlobRange range;
    uint8_t* PR_NULLABLE storage;
    PrBlobReadDoneFn PR_NULLABLE done;
    void* PR_NULLABLE context;              // passed to `done`
} PrBlobReadDesc;

// Returns the description of a read of the whole resource into a blob, with no callback.
PrBlobReadDesc prBlobReadDescDefault(void);

// Starts a read of the source. The caller destroys the read after it completes (prBlobReadDestroy), in `done` or later.
PrBlobRead* prBlobReadCreate(const PrBlobSource* source, const PrBlobReadDesc* desc);

// Returns true if the read has completed. Any thread may call it.
bool prBlobReadIsDone(const PrBlobRead* read);

// Blocks the calling thread until the read completes. In a program that suspends, it suspends the program instead.
void prBlobReadWait(PrBlobRead* read);

// Returns the status of a completed read: Ok, Io if the bytes cannot be read, or the provider's own failure.
PrStatus prBlobReadStatus(const PrBlobRead* read);

// Returns the bytes of a read without storage that succeeded. The blob is valid until the read is destroyed. The caller
// retains it to keep it longer.
PrBlob* PR_NULLABLE prBlobReadBlob(const PrBlobRead* read);

// Returns the number of bytes that the read produced. It is less than the range's size if the range extends past the
// end of the resource.
uint64_t prBlobReadByteCount(const PrBlobRead* read);

// Frees a completed read and releases its blob.
void prBlobReadDestroy(PrBlobRead* PR_NULLABLE read);

// Reads `range` of the source into a blob and blocks until the read completes. Writes the blob to `*out`. The caller
// releases it. Returns the read's status. On failure, `*out` is null.
PrStatus prBlobSourceRead(const PrBlobSource* source, PrBlobRange range, PrBlob* PR_NULLABLE* PR_NONNULL out);

// For a provider. Return the range that the read asks for, and the storage that receives the bytes, or null if the read
// wants a blob.
PrBlobRange prBlobReadRange(const PrBlobRead* read);
uint8_t* PR_NULLABLE prBlobReadStorage(const PrBlobRead* read);

// For a provider. Completes the read with `status`.
//
// On success, `bytes` holds the bytes of the range, and the read takes the caller's reference to it. A read with
// storage copies the bytes into its storage. A provider that wrote the bytes into the storage itself passes a null
// `bytes` and the number of bytes in `byteCount`.
//
// The read's `done` callback runs during the call. A read must be completed exactly once.
void prBlobReadComplete(PrBlobRead* read, PrStatus status, PrBlob* PR_NULLABLE bytes, uint64_t byteCount);

// ---------------------------------------------------------------------------------------------------------------------
// Feeds
//
// A feed is the provider of a resource whose bytes a host hands over as they arrive: it appends each piece, and then
// ends the bytes or fails them. One thread at a time writes to a feed, which asserts it; any thread may read its
// source. The feed's source holds every byte that was appended, for as long as it lives, and serves reads of them as a
// source of arriving bytes does. A host that fetches a resource appends the body as it is read (pr/fetch.h), ends it at
// the fetch's End and fails it at Fail.

// The provider of a resource whose bytes a host appends.
typedef struct PrBlobFeed PrBlobFeed;

// Creates a feed of a resource of `size` bytes, or of kPrBlobSizeUnknown.
PrBlobFeed* prBlobFeedCreate(uint64_t size);

// Stops feeding: a feed whose bytes have not ended fails with Lost. Its source stays valid, with the bytes that were
// appended.
void prBlobFeedDestroy(PrBlobFeed* PR_NULLABLE feed);

// Returns the source of the feed's bytes, which is valid while the feed is. A consumer retains it to keep it longer.
const PrBlobSource* prBlobFeedSource(const PrBlobFeed* feed);

// Appends `bytes` to the resource, completes the reads that they satisfy, and notifies the source's notifier. Bytes
// appended after the end or a failure are ignored.
void prBlobFeedAppend(PrBlobFeed* feed, PrByteView bytes);

// Ends the bytes: the resource is what was appended. Completes the reads that wait, with the bytes that their ranges
// cover, and notifies.
void prBlobFeedEnd(PrBlobFeed* feed);

// Fails the bytes with `status`, which is not Ok. Completes the reads that wait with `status`, and notifies. A later
// read of bytes that had arrived still reads them, and a read past them fails.
void prBlobFeedFail(PrBlobFeed* feed, PrStatus status);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

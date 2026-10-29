// ZIP archives: the files in an archive in memory, read through a compression codec.
//
// An archive may be read from any number of threads at once.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A ZIP archive in memory, read from its central directory, ZIP64 included.
typedef struct PrCompressZip PrCompressZip;

// How the bytes of an entry are stored.
enum {
    PrCompressZipMethod_Stored = 0,         // as they are
    PrCompressZipMethod_Deflated = 8,       // compressed with Deflate
};
typedef uint16_t PrCompressZipMethod;

// An entry of an archive, as the central directory describes it. `name` stays valid while the bytes are open.
typedef struct PrCompressZipEntry {
    PrStringView name;          // the name as the archive gives it, with '/' separators
    uint64_t size;              // uncompressed size
    uint64_t compressedSize;
    uint32_t crc32;
    PrCompressZipMethod method;
    bool encrypted;
} PrCompressZipEntry;

// Creates an archive that decompresses with `codec` and has nothing open. `codec` must outlive the archive. The archive
// keeps its storage from one open to the next.
PrCompressZip* prCompressZipCreate(PrCompressCodec* codec);

// Closes the open bytes, if any, and frees the archive.
void prCompressZipDestroy(PrCompressZip* PR_NULLABLE archive);

// Closes the open bytes, if any, and opens `bytes`. `bytes` must stay valid until they are closed. Returns InvalidData
// and leaves nothing open if `bytes` is not a ZIP archive.
PrStatus prCompressZipOpen(PrCompressZip* archive, PrByteView bytes);

// Closes the open bytes, leaving no entries.
void prCompressZipClose(PrCompressZip* archive);

// Returns the number of entries in the open bytes.
uint32_t prCompressZipEntryCount(const PrCompressZip* archive);

// Returns the entry at `index`. `index` must be less than the entry count.
PrCompressZipEntry prCompressZipEntry(const PrCompressZip* archive, uint32_t index);

// Returns the index of the entry named `name`, compared byte for byte, or the entry count if there is none.
uint32_t prCompressZipFind(const PrCompressZip* archive, PrStringView name);

// Reads every byte of entry `index` into `out`. `capacity` must be at least the entry's size. Returns NotSupported for
// an encrypted entry or a method other than Stored and Deflated, and InvalidData if the entry is corrupt or its
// checksum does not match.
PrStatus prCompressZipRead(const PrCompressZip* archive, uint32_t index, uint8_t* PR_NULLABLE out, size_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

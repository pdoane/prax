// ZIP archives: the files in an archive in memory, read through a set of compression codecs.
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
typedef struct PrZip PrZip;

// How the bytes of an entry are stored.
enum {
    PrZipMethod_Stored = 0,         // as they are
    PrZipMethod_Deflated = 8,       // compressed with Deflate
};
typedef uint16_t PrZipMethod;

// An entry of an archive, as the central directory describes it. `name` stays valid while the bytes are open.
typedef struct PrZipEntry {
    PrStringView name;          // the name as the archive gives it, with '/' separators
    uint64_t size;              // uncompressed size
    uint64_t compressedSize;
    uint32_t crc32;
    PrZipMethod method;
    bool encrypted;
} PrZipEntry;

// Creates an archive that decompresses with the codecs of `compress` and has nothing open. `compress` must outlive the
// archive. Without a set, the archive reads the entries that are stored as they are, and a deflated entry is
// NotSupported. The archive keeps its storage from one open to the next.
PrZip* prZipCreate(const PrCompressCodecSet* PR_NULLABLE compress);

// Closes the open bytes, if any, and frees the archive.
void prZipDestroy(PrZip* PR_NULLABLE archive);

// Closes the open bytes, if any, and opens `bytes`. `bytes` must stay valid until they are closed. Returns InvalidData
// and leaves nothing open if `bytes` is not a ZIP archive.
PrStatus prZipOpen(PrZip* archive, PrByteView bytes);

// Closes the open bytes, leaving no entries.
void prZipClose(PrZip* archive);

// Returns the number of entries in the open bytes.
uint32_t prZipEntryCount(const PrZip* archive);

// Returns the entry at `index`. `index` must be less than the entry count.
PrZipEntry prZipEntry(const PrZip* archive, uint32_t index);

// Returns the index of the entry named `name`, compared byte for byte, or the entry count if there is none.
uint32_t prZipFind(const PrZip* archive, PrStringView name);

// Reports the bytes of entry `index` as the archive holds them, in the open bytes: the entry's own bytes if its method
// is Stored, and otherwise its compressed bytes. A caller reads a stored entry in place this way, without a copy, and
// learns where in the archive the entry lies. The entry's checksum is not checked. Returns InvalidData, and the null
// view, if the entry's local header or its bytes lie outside the archive.
PrStatus prZipEntryData(const PrZip* archive, uint32_t index, PrByteView* out);

// Reads every byte of entry `index` into `out`. `capacity` must be at least the entry's size. Returns NotSupported for
// an encrypted entry, for a method other than Stored and Deflated, and for a deflated entry of an archive without a
// set, and InvalidData if the entry is corrupt or its checksum does not match.
PrStatus prZipRead(const PrZip* archive, uint32_t index, uint8_t* PR_NULLABLE out, size_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

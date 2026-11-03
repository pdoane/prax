// ZIP archives: the files in an archive in memory, read through a compression backend.

#include "pr/compress_zip.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/checksum.h"
#include "pr/string_ops.h"

namespace pr_impl {

namespace {

constexpr uint32_t kEndOfDirectory = 0x06054b50;
constexpr uint32_t kEndOfDirectory64 = 0x06064b50;
constexpr uint32_t kEndLocator64 = 0x07064b50;
constexpr uint32_t kDirectoryEntry = 0x02014b50;
constexpr uint32_t kLocalEntry = 0x04034b50;
constexpr size_t kEndSize = 22;
constexpr size_t kLocalSize = 30;
constexpr size_t kDirectorySize = 46;
constexpr uint16_t kZip64Extra = 0x0001;

// An entry of the central directory, and where its local header is.
struct ZipRecord {
    PrCompressZipEntry entry;
    uint64_t local;             // the local header's offset
};

struct ZipArchive {
    PrCompressCodec* codec;
    PrByteView bytes;
    Array<ZipRecord> records;
};

uint16_t read16(const uint8_t* p) {
    return uint16_t(p[0] | p[1] << 8);
}

uint32_t read32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint64_t read64(const uint8_t* p) {
    return uint64_t(read32(p)) | uint64_t(read32(p + 4)) << 32;
}

// Returns true if the `size` bytes starting at `at` lie inside the archive.
bool inside(const ZipArchive& z, uint64_t at, uint64_t size) {
    return at <= z.bytes.size && size <= z.bytes.size - at;
}

// Finds the directory's offset and entry count from the end record, and from its ZIP64 form if present.
bool findDirectory(const ZipArchive& z, uint64_t* offset, uint64_t* count) {
    const uint8_t* b = z.bytes.data;
    if (z.bytes.size < kEndSize)
        return false;
    // The end record is last, except for a comment of up to 65535 bytes.
    size_t lowest = z.bytes.size > kEndSize + 0xffff ? z.bytes.size - kEndSize - 0xffff : 0;
    for (size_t at = z.bytes.size - kEndSize + 1; at-- > lowest;) {
        if (read32(b + at) != kEndOfDirectory)
            continue;
        *count = read16(b + at + 10);
        *offset = read32(b + at + 16);
        // ZIP64: a locator just before the end record points to the larger record.
        if (at >= 20 && read32(b + at - 20) == kEndLocator64) {
            uint64_t record = read64(b + at - 20 + 8);
            if (inside(z, record, 56) && read32(b + record) == kEndOfDirectory64) {
                *count = read64(b + record + 32);
                *offset = read64(b + record + 48);
            }
        }
        return true;
    }
    return false;
}

bool readDirectory(ZipArchive& z) {
    uint64_t at = 0;
    uint64_t count = 0;
    if (!findDirectory(z, &at, &count))
        return false;
    const uint8_t* b = z.bytes.data;
    for (uint64_t i = 0; i < count; ++i) {
        if (!inside(z, at, kDirectorySize) || read32(b + at) != kDirectoryEntry)
            return false;
        const uint8_t* e = b + at;
        uint16_t nameSize = read16(e + 28);
        uint16_t extraSize = read16(e + 30);
        uint16_t commentSize = read16(e + 32);
        if (!inside(z, at + kDirectorySize, uint64_t(nameSize) + extraSize + commentSize))
            return false;
        ZipRecord record{};
        record.entry.name = PrStringView{reinterpret_cast<const char*>(e + kDirectorySize), nameSize};
        record.entry.method = read16(e + 10);
        record.entry.encrypted = (read16(e + 8) & 1) != 0;
        record.entry.crc32 = read32(e + 16);
        record.entry.compressedSize = read32(e + 20);
        record.entry.size = read32(e + 24);
        record.local = read32(e + 42);
        // The ZIP64 extra field holds the sizes and offset for each field that the record sets to all ones.
        const uint8_t* extra = e + kDirectorySize + nameSize;
        for (uint32_t x = 0; x + 4 <= extraSize;) {
            uint16_t id = read16(extra + x);
            uint16_t size = read16(extra + x + 2);
            if (x + 4 + size > extraSize)
                break;
            if (id == kZip64Extra) {
                const uint8_t* field = extra + x + 4;
                uint32_t used = 0;
                auto next = [&](uint64_t& value, uint32_t original) {
                    if (original == 0xffffffffu && used + 8 <= size) {
                        value = read64(field + used);
                        used += 8;
                    }
                };
                next(record.entry.size, read32(e + 24));
                next(record.entry.compressedSize, read32(e + 20));
                next(record.local, read32(e + 42));
            }
            x += 4 + size;
        }
        z.records.push(record);
        at += kDirectorySize + nameSize + extraSize + commentSize;
    }
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrCompressZip : ZipArchive {};

PrCompressZip* prCompressZipCreate(PrCompressCodec* codec) {
    PrCompressZip* z = allocZeroed<PrCompressZip>();
    z->codec = codec;
    return z;
}

void prCompressZipDestroy(PrCompressZip* PR_NULLABLE archive) {
    if (archive == nullptr)
        return;
    archive->records.reset();
    prMemoryFree(archive);
}

PrStatus prCompressZipOpen(PrCompressZip* archive, PrByteView bytes) {
    prCompressZipClose(archive);
    archive->bytes = bytes;
    if (bytes.data == nullptr || !readDirectory(*archive)) {
        prCompressZipClose(archive);
        return PrStatus_InvalidData;
    }
    return PrStatus_Ok;
}

void prCompressZipClose(PrCompressZip* archive) {
    archive->bytes = PrByteView{};
    archive->records.count = 0;
}

uint32_t prCompressZipEntryCount(const PrCompressZip* archive) {
    return archive->records.count;
}

PrCompressZipEntry prCompressZipEntry(const PrCompressZip* archive, uint32_t index) {
    return archive->records.data[index].entry;
}

uint32_t prCompressZipFind(const PrCompressZip* archive, PrStringView name) {
    for (uint32_t i = 0; i < archive->records.count; ++i) {
        if (archive->records.data[i].entry.name == name)
            return i;
    }
    return archive->records.count;
}

PrStatus prCompressZipRead(const PrCompressZip* archive, uint32_t index, uint8_t* PR_NULLABLE out, size_t capacity) {
    const ZipRecord& record = archive->records.data[index];
    PrCompressZipMethod method = record.entry.method;
    if (record.entry.encrypted || (method != PrCompressZipMethod_Stored && method != PrCompressZipMethod_Deflated))
        return PrStatus_NotSupported;
    PR_ASSERT(capacity >= record.entry.size);
    const uint8_t* b = archive->bytes.data;
    if (!inside(*archive, record.local, kLocalSize) || read32(b + record.local) != kLocalEntry)
        return PrStatus_InvalidData;
    uint64_t data = record.local + kLocalSize + read16(b + record.local + 26) + read16(b + record.local + 28);
    if (!inside(*archive, data, record.entry.compressedSize))
        return PrStatus_InvalidData;
    PrByteView input{b + data, size_t(record.entry.compressedSize)};
    if (record.entry.method == PrCompressZipMethod_Stored) {
        if (record.entry.compressedSize != record.entry.size)
            return PrStatus_InvalidData;
        if (record.entry.size > 0)
            prMemCopy(out, input.data, size_t(record.entry.size));
    } else {
        PrCompressRead read{};
        PrStatus status = prCompressCodecDecode(archive->codec, PrCompressFormat_Deflate, input, out, capacity, &read);
        if (status != PrStatus_Ok)
            return status;
        if (read.written != record.entry.size || read.progress != PrCompressProgress_Done)
            return PrStatus_InvalidData;
    }
    uint32_t crc = prCrc32Update(0, PrByteView{out, size_t(record.entry.size)});
    return crc == record.entry.crc32 ? PrStatus_Ok : PrStatus_InvalidData;
}

// The capture settings of a photograph, read from its Exif data.

#include "pr/image_exif.h"

#include "pr/mem.h"

namespace pr_impl {

namespace {

// The tags that a capture reads (Exif 2.32). The first tags are in the first image file directory. The rest are in the
// Exif directory that the first directory points to.
constexpr uint32_t kTagMake = 0x010F;
constexpr uint32_t kTagModel = 0x0110;
constexpr uint32_t kTagExifDirectory = 0x8769;
constexpr uint32_t kTagExposureTime = 0x829A;
constexpr uint32_t kTagFNumber = 0x829D;
constexpr uint32_t kTagIsoSpeed = 0x8827;
constexpr uint32_t kTagTaken = 0x9003;
constexpr uint32_t kTagFocalLength = 0x920A;
constexpr uint32_t kTagLensModel = 0xA434;

// The field types of those tags.
constexpr uint32_t kTypeAscii = 2;
constexpr uint32_t kTypeShort = 3;
constexpr uint32_t kTypeLong = 4;
constexpr uint32_t kTypeRational = 5;

// The maximum number of entries that are read from a directory.
constexpr uint32_t kMaxEntries = 512;

// A TIFF structure, read in its byte order.
struct Tiff {
    const uint8_t* bytes;
    size_t size;
    bool bigEndian;

    uint32_t readU16(size_t at) const { return bigEndian ? prMemReadBeU16(bytes + at) : prMemReadLeU16(bytes + at); }
    uint32_t readU32(size_t at) const { return bigEndian ? prMemReadBeU32(bytes + at) : prMemReadLeU32(bytes + at); }
};

// An entry of a directory: its tag, type and count, and the location of its value. The value is in the entry if it fits
// in four bytes. Otherwise it is at the offset that the entry holds.
struct Entry {
    uint32_t tag;
    uint32_t type;
    uint32_t count;
    size_t value;
};

// Returns the size in bytes of one value of `type`.
uint32_t sizeOfType(uint32_t type) {
    switch (type) {
        case kTypeShort: return 2;
        case kTypeLong: return 4;
        case kTypeRational: return 8;
        default: return 1;
    }
}

// Writes the entry at `at` to `*out`. Returns false if the value of the entry lies outside the structure.
bool entryAt(const Tiff& tiff, size_t at, Entry* out) {
    out->tag = tiff.readU16(at);
    out->type = tiff.readU16(at + 2);
    out->count = tiff.readU32(at + 4);
    uint64_t bytes = uint64_t(out->count) * sizeOfType(out->type);
    out->value = bytes <= 4 ? at + 8 : tiff.readU32(at + 8);
    return out->value <= tiff.size && bytes <= tiff.size - out->value;
}

// Copies an ASCII value into `out`, which holds `capacity` bytes. It copies the printable characters up to the first
// NUL, and trims them.
void textOf(const Tiff& tiff, const Entry& entry, char* out, size_t capacity) {
    if (entry.type != kTypeAscii)
        return;
    size_t size = 0;
    size_t kept = 0;
    for (uint32_t i = 0; i < entry.count && size + 1 < capacity; ++i) {
        uint8_t c = tiff.bytes[entry.value + i];
        if (c == 0)
            break;
        if (c < 0x20 || c == 0x7f || (c == ' ' && size == 0))
            continue;
        out[size++] = char(c);
        if (c != ' ')
            kept = size;
    }
    out[kept] = 0;
}

// Returns a rational value as a float, or zero if the entry has none.
float rationalOf(const Tiff& tiff, const Entry& entry) {
    if (entry.type != kTypeRational || entry.count == 0)
        return 0;
    uint32_t denominator = tiff.readU32(entry.value + 4);
    return denominator == 0 ? 0 : float(double(tiff.readU32(entry.value)) / double(denominator));
}

// Returns an integer value, which is a short or a long, or zero if the entry has none.
uint32_t wholeOf(const Tiff& tiff, const Entry& entry) {
    if (entry.count == 0)
        return 0;
    if (entry.type == kTypeShort)
        return tiff.readU16(entry.value);
    return entry.type == kTypeLong ? tiff.readU32(entry.value) : 0;
}

// Reads the directory at `at` into `*out`. Returns the offset of the Exif directory that it points to, or zero if it
// points to none.
size_t readDirectory(const Tiff& tiff, size_t at, PrImageCapture* out) {
    if (at < 8 || at > tiff.size - 2)
        return 0;
    uint32_t count = tiff.readU16(at);
    count = count < kMaxEntries ? count : kMaxEntries;
    size_t exif = 0;
    for (uint32_t i = 0; i < count; ++i) {
        size_t place = at + 2 + size_t(i) * 12;
        Entry entry{};
        if (place + 12 > tiff.size)
            break;
        if (!entryAt(tiff, place, &entry))
            continue;
        switch (entry.tag) {
            case kTagMake: textOf(tiff, entry, out->make, sizeof out->make); break;
            case kTagModel: textOf(tiff, entry, out->model, sizeof out->model); break;
            case kTagLensModel: textOf(tiff, entry, out->lens, sizeof out->lens); break;
            case kTagTaken: textOf(tiff, entry, out->taken, sizeof out->taken); break;
            case kTagExifDirectory: exif = wholeOf(tiff, entry); break;
            case kTagExposureTime:
                if (entry.type == kTypeRational && entry.count != 0) {
                    out->exposure[0] = tiff.readU32(entry.value);
                    out->exposure[1] = tiff.readU32(entry.value + 4);
                }
                break;
            case kTagFNumber: out->fNumber = rationalOf(tiff, entry); break;
            case kTagFocalLength: out->focalLength = rationalOf(tiff, entry); break;
            case kTagIsoSpeed: out->iso = wholeOf(tiff, entry); break;
            default: break;
        }
    }
    return exif;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prImageExifCapture(PrByteView exif, PrImageCapture* out) {
    *out = PrImageCapture{};
    if (exif.size < 8)
        return false;
    bool bigEndian = exif.data[0] == 'M' && exif.data[1] == 'M';
    if (!bigEndian && (exif.data[0] != 'I' || exif.data[1] != 'I'))
        return false;
    Tiff tiff{exif.data, exif.size, bigEndian};
    if (tiff.readU16(2) != 42)
        return false;
    size_t inner = readDirectory(tiff, tiff.readU32(4), out);
    if (inner != 0)
        readDirectory(tiff, inner, out);
    return out->make[0] != 0 || out->model[0] != 0 || out->lens[0] != 0 || out->taken[0] != 0 ||
        out->exposure[1] != 0 || out->fNumber != 0 || out->focalLength != 0 || out->iso != 0;
}

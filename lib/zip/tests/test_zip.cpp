#include "pr/array.h"
#include "pr/compress_deflate.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/zip.h"

#include "tools/harness/test.h"

extern "C" int prTestZipCApi(void);

namespace pr_impl {

namespace {

using namespace pr;

// An archive with a stored entry and a deflated one in a directory, as Python's zipfile writes it.
constexpr uint8_t kArchive[] = {
    0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0xcb, 0xaf,
    0x5c, 0x71, 0x0b, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x73, 0x74,
    0x6f, 0x72, 0x65, 0x64, 0x2e, 0x74, 0x78, 0x74, 0x70, 0x6c, 0x61, 0x69, 0x6e, 0x20, 0x62, 0x79,
    0x74, 0x65, 0x73, 0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x21,
    0x00, 0xb2, 0xdb, 0x41, 0x32, 0x0f, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00,
    0x00, 0x64, 0x69, 0x72, 0x2f, 0x64, 0x65, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x64, 0x2e, 0x6a, 0x73,
    0x6f, 0x6e, 0xab, 0x56, 0x4a, 0x54, 0xb2, 0x52, 0x88, 0x36, 0xd4, 0x51, 0xc0, 0x89, 0x62, 0x6b,
    0x01, 0x50, 0x4b, 0x01, 0x02, 0x14, 0x03, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21,
    0x00, 0xcb, 0xaf, 0x5c, 0x71, 0x0b, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x73,
    0x74, 0x6f, 0x72, 0x65, 0x64, 0x2e, 0x74, 0x78, 0x74, 0x50, 0x4b, 0x01, 0x02, 0x14, 0x03, 0x14,
    0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x21, 0x00, 0xb2, 0xdb, 0x41, 0x32, 0x0f, 0x00, 0x00,
    0x00, 0x25, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x01, 0x33, 0x00, 0x00, 0x00, 0x64, 0x69, 0x72, 0x2f, 0x64, 0x65, 0x66, 0x6c, 0x61,
    0x74, 0x65, 0x64, 0x2e, 0x6a, 0x73, 0x6f, 0x6e, 0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x02, 0x00, 0x77, 0x00, 0x00, 0x00, 0x71, 0x00, 0x00, 0x00, 0x00, 0x00,
};

String read(PrZip* archive, uint32_t index, PrStatus* status) {
    PrZipEntry entry = prZipEntry(archive, index);
    Array<uint8_t> bytes(entry.size);
    *status = prZipRead(archive, index, bytes.data(), bytes.count());
    return String(PrStringView{reinterpret_cast<const char*>(bytes.data()), bytes.count()});
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an archive's entries read stored and deflated") {
    PrCompressCodec* deflate = prCompressDeflateCreate();
    PrCompressCodecSet* compress = prCompressCodecSetCreate(&deflate, 1);
    PrZip* archive = prZipCreate(compress);
    REQUIRE(prZipOpen(archive, kArchive) == PrStatus_Ok);
    REQUIRE(prZipEntryCount(archive) == 2);
    PrZipEntry stored = prZipEntry(archive, 0);
    CHECK(stored.name == "stored.txt");
    CHECK(stored.method == PrZipMethod_Stored);
    uint32_t deflated = prZipFind(archive, "dir/deflated.json");
    REQUIRE(deflated == 1);
    CHECK(prZipEntry(archive, deflated).method == PrZipMethod_Deflated);
    CHECK(prZipFind(archive, "missing") == 2);
    // A stored entry's bytes are read where the archive holds them.
    PrByteView data{};
    REQUIRE(prZipEntryData(archive, 0, &data) == PrStatus_Ok);
    CHECK(data.data == kArchive + 40);
    CHECK(PrStringView{reinterpret_cast<const char*>(data.data), data.size} == "plain bytes");
    REQUIRE(prZipEntryData(archive, deflated, &data) == PrStatus_Ok);
    CHECK(data.size == prZipEntry(archive, deflated).compressedSize);
    PrStatus status = PrStatus_NotFound;
    CHECK(read(archive, 0, &status) == "plain bytes");
    CHECK(status == PrStatus_Ok);
    CHECK(read(archive, 1, &status) == "{\"a\": [1, 1, 1, 1, 1, 1, 1, 1, 1, 1]}");
    CHECK(status == PrStatus_Ok);
    prZipDestroy(archive);
    prCompressCodecSetDestroy(compress);
}

TEST_CASE("bytes that are not an archive, or an entry whose checksum is wrong, are refused") {
    PrCompressCodec* deflate = prCompressDeflateCreate();
    PrCompressCodecSet* compress = prCompressCodecSetCreate(&deflate, 1);
    PrZip* archive = prZipCreate(compress);
    REQUIRE(prZipOpen(archive, kArchive) == PrStatus_Ok);
    const uint8_t junk[] = {1, 2, 3, 4, 5};
    CHECK(prZipOpen(archive, junk) == PrStatus_InvalidData);
    CHECK(prZipEntryCount(archive) == 0);
    Array<uint8_t> damaged(kArchive);
    // The first byte of the stored entry.
    damaged[40] ^= 0xff;
    REQUIRE(prZipOpen(archive, damaged) == PrStatus_Ok);
    CHECK(prZipEntryCount(archive) == 2);
    PrStatus status = PrStatus_Ok;
    read(archive, 0, &status);
    CHECK(status == PrStatus_InvalidData);
    prZipDestroy(archive);
    prCompressCodecSetDestroy(compress);
}

TEST_CASE("an archive without a set reads its stored entries, and no deflated one") {
    PrZip* archive = prZipCreate(nullptr);
    REQUIRE(prZipOpen(archive, kArchive) == PrStatus_Ok);
    PrStatus status = PrStatus_NotFound;
    CHECK(read(archive, 0, &status) == "plain bytes");
    CHECK(status == PrStatus_Ok);
    read(archive, 1, &status);
    CHECK(status == PrStatus_NotSupported);
    prZipDestroy(archive);
}

TEST_CASE("the public header compiles as C") {
    CHECK(prTestZipCApi() == 0);
}

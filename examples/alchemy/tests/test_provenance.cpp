#include "pr/array.h"
#include "pr/checksum.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "examples/alchemy/provenance.h"

#include "tools/harness/test.h"

using namespace alchemy;
using namespace pr;

namespace {

ProvenanceOp generateOp() {
    ProvenanceOp op;
    op.mode = "generate";
    op.prompt = "a red fox in snow";
    op.negative = "blurry";
    op.checkpoint = "sd_xl_base_1.0.safetensors";
    op.sampler = "euler";
    op.scheduler = "normal";
    op.steps = 24;
    op.cfg = 6.5;
    op.width = 1024;
    op.height = 1024;
    op.batch = 1;
    op.seed = 18446744073709551615ull;
    return op;
}

ProvenanceOp upscaleOp() {
    ProvenanceOp op;
    op.mode = "upscale";
    op.upscaleModel = "RealESRGAN_x4.pth";
    op.factor = 2.0;
    op.source = "Alchemy_00001_.png";
    return op;
}

void appendU32(Array<uint8_t>* out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out->push(uint8_t(value >> shift));
}

void appendChunk(Array<uint8_t>* out, const char* type, PrStringView data) {
    appendU32(out, uint32_t(data.size));
    uint32_t start = out->count();
    out->append(reinterpret_cast<const uint8_t*>(type), 4);
    out->append(prStringBytes(data), uint32_t(data.size));
    appendU32(out, prCrc32Update(0, PrByteView{out->data() + start, out->count() - start}));
}

// Returns a PNG file with well-formed chunks, and `text` under `keyword` if it is not empty. Its pixels are not valid.
Array<uint8_t> png(const char* keyword = "", PrStringView text = "") {
    Array<uint8_t> bytes({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
    static const char kHeader[13] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    appendChunk(&bytes, "IHDR", PrStringView{kHeader, sizeof(kHeader)});
    if (keyword[0] != 0) {
        String chunk(keyword);
        chunk.append('\0');
        chunk.append(text);
        appendChunk(&bytes, "tEXt", chunk);
    }
    appendChunk(&bytes, "IDAT", "pixels");
    appendChunk(&bytes, "IEND", "");
    return bytes;
}

} // namespace

TEST_CASE("a generate operation reads back whole, with its seed's full 64 bits") {
    ProvenanceRecord written;
    written.operations.push(generateOp());
    ProvenanceRecord read;
    REQUIRE(provenanceParse(provenanceSerialize(written), &read));
    REQUIRE(read.operations.count() == 1);
    const ProvenanceOp& op = read.operations[0];
    CHECK(op.mode == "generate");
    CHECK(op.prompt == "a red fox in snow");
    CHECK(op.negative == "blurry");
    CHECK(op.checkpoint == "sd_xl_base_1.0.safetensors");
    CHECK(op.scheduler == "normal");
    CHECK(op.steps == 24);
    CHECK(op.cfg == 6.5);
    CHECK(op.width == 1024);
    CHECK(op.batch == 1);
    CHECK(op.seed == 18446744073709551615ull);
}

TEST_CASE("a chain reads back in order, with its adapters") {
    ProvenanceRecord written;
    written.operations.push(generateOp());
    written.operations.last().loras.push(ProvenanceLora{.name = "detail.safetensors", .weight = 0.8});
    written.operations.last().loras.push(ProvenanceLora{.name = "film.safetensors", .weight = 1.25});
    written.operations.push(upscaleOp());
    ProvenanceRecord read;
    REQUIRE(provenanceParse(provenanceSerialize(written), &read));
    CHECK(provenanceChain(read) == "generate \xE2\x86\x92 upscale");
    REQUIRE(read.operations[0].loras.count() == 2);
    CHECK(read.operations[0].loras[1].name == "film.safetensors");
    CHECK(read.operations[0].loras[1].weight == 1.25);
    const ProvenanceOp* newest = provenanceNewest(read);
    REQUIRE(newest != nullptr);
    CHECK(newest->upscaleModel == "RealESRGAN_x4.pth");
    CHECK(newest->factor == 2.0);
    CHECK(newest->source == "Alchemy_00001_.png");
}

TEST_CASE("a record holds only what its mode uses, and keeps a mode that it does not know") {
    ProvenanceRecord written;
    written.operations.push(upscaleOp());
    String json = provenanceSerialize(written);
    CHECK_FALSE(prStringContains(json, "prompt"));
    CHECK_FALSE(prStringContains(json, "seed"));
    CHECK(prStringContains(json, "upscaleModel"));

    ProvenanceRecord read;
    REQUIRE(provenanceParse("{\"version\":2,\"operations\":[{\"mode\":\"relight\",\"prompt\":\"noon\","
        "\"lightAngle\":45,\"loras\":[{\"name\":\"a\"},\"junk\"]}]}", &read));
    REQUIRE(read.operations.count() == 1);
    CHECK(read.operations[0].mode == "relight");
    REQUIRE(read.operations[0].loras.count() == 1);
    CHECK(read.operations[0].loras[0].weight == 1.0);
    const char* bad[] = {"", "{\"version\":1}", "not json", "{\"operations\":{}}"};
    for (const char* text : bad)
        CHECK_FALSE(provenanceParse(text, &read));
    REQUIRE(provenanceParse("{\"version\":1,\"operations\":[]}", &read));
    CHECK(provenanceNewest(read) == nullptr);
}

TEST_CASE("a record is written into a PNG file and read back from it") {
    ProvenanceRecord written;
    written.operations.push(generateOp());
    Array<uint8_t> plain = png();
    Array<uint8_t> withRecord = provenanceWrite(plain, written);
    REQUIRE(!withRecord.isEmpty());
    ProvenanceRecord read;
    REQUIRE(provenanceRead(withRecord, nullptr, &read));
    CHECK(read.operations[0].prompt == "a red fox in snow");

    Array<uint8_t> other = png("other", "{\"version\":1,\"operations\":[{\"mode\":\"generate\"}]}");
    CHECK_FALSE(provenanceRead(other, nullptr, &read));

    CHECK_FALSE(provenanceRead(plain, nullptr, &read));
    PrStringView notPng = "not an image";
    PrByteView notPngBytes{prStringBytes(notPng), notPng.size};
    CHECK_FALSE(provenanceRead(notPngBytes, nullptr, &read));
    CHECK(provenanceWrite(notPngBytes, written).isEmpty());
}

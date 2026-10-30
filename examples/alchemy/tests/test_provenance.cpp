#include "pr/checksum.h"

#include "examples/alchemy/provenance.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

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

void appendU32(std::vector<uint8_t>* out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out->push_back(uint8_t(value >> shift));
}

void appendChunk(std::vector<uint8_t>* out, const char* type, const std::string& data) {
    appendU32(out, uint32_t(data.size()));
    size_t start = out->size();
    out->insert(out->end(), type, type + 4);
    out->insert(out->end(), data.begin(), data.end());
    appendU32(out, crc32Update(0, PrByteView{out->data() + start, out->size() - start}));
}

// Returns a PNG file with well-formed chunks, and `text` under `keyword` if it is not empty. Its pixels are not valid.
std::vector<uint8_t> png(const char* keyword = "", const std::string& text = "") {
    std::vector<uint8_t> bytes{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    appendChunk(&bytes, "IHDR", std::string(13, '\1'));
    if (keyword[0] != 0)
        appendChunk(&bytes, "tEXt", std::string(keyword) + '\0' + text);
    appendChunk(&bytes, "IDAT", "pixels");
    appendChunk(&bytes, "IEND", "");
    return bytes;
}

} // namespace

TEST_CASE("a generate operation reads back whole, with its seed's full 64 bits") {
    ProvenanceRecord written;
    written.operations.push_back(generateOp());
    ProvenanceRecord read;
    REQUIRE(provenanceParse(provenanceSerialize(written), &read));
    REQUIRE(read.operations.size() == 1);
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
    written.operations.push_back(generateOp());
    written.operations.back().loras = {{"detail.safetensors", 0.8}, {"film.safetensors", 1.25}};
    written.operations.push_back(upscaleOp());
    ProvenanceRecord read;
    REQUIRE(provenanceParse(provenanceSerialize(written), &read));
    CHECK(provenanceChain(read) == "generate \xE2\x86\x92 upscale");
    REQUIRE(read.operations[0].loras.size() == 2);
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
    written.operations.push_back(upscaleOp());
    std::string json = provenanceSerialize(written);
    CHECK(json.find("prompt") == std::string::npos);
    CHECK(json.find("seed") == std::string::npos);
    CHECK(json.find("upscaleModel") != std::string::npos);

    ProvenanceRecord read;
    REQUIRE(provenanceParse("{\"version\":2,\"operations\":[{\"mode\":\"relight\",\"prompt\":\"noon\","
        "\"lightAngle\":45,\"loras\":[{\"name\":\"a\"},\"junk\"]}]}", &read));
    REQUIRE(read.operations.size() == 1);
    CHECK(read.operations[0].mode == "relight");
    REQUIRE(read.operations[0].loras.size() == 1);
    CHECK(read.operations[0].loras[0].weight == 1.0);
    for (const char* bad : {"", "{\"version\":1}", "not json", "{\"operations\":{}}"})
        CHECK_FALSE(provenanceParse(bad, &read));
    REQUIRE(provenanceParse("{\"version\":1,\"operations\":[]}", &read));
    CHECK(provenanceNewest(read) == nullptr);
}

TEST_CASE("a record is written into a PNG file and read back from it") {
    ProvenanceRecord written;
    written.operations.push_back(generateOp());
    std::vector<uint8_t> plain = png();
    std::string withRecord = provenanceWrite(plain, written);
    REQUIRE(!withRecord.empty());
    ProvenanceRecord read;
    REQUIRE(provenanceRead(PrByteView{reinterpret_cast<const uint8_t*>(withRecord.data()), withRecord.size()}, nullptr, &read));
    CHECK(read.operations[0].prompt == "a red fox in snow");

    std::vector<uint8_t> other = png("other", "{\"version\":1,\"operations\":[{\"mode\":\"generate\"}]}");
    CHECK_FALSE(provenanceRead(other, nullptr, &read));

    CHECK_FALSE(provenanceRead(plain, nullptr, &read));
    std::string notPng = "not an image";
    CHECK_FALSE(provenanceRead(PrByteView{reinterpret_cast<const uint8_t*>(notPng.data()), notPng.size()}, nullptr, &read));
    CHECK(provenanceWrite(PrByteView{reinterpret_cast<const uint8_t*>(notPng.data()), notPng.size()}, written) .empty());
}

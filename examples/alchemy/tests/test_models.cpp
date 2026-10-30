#include "examples/alchemy/models.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

using namespace alchemy;

namespace {

// Returns a safetensors file's leading bytes: the little-endian length, which `declared` overrides to pose a truncated
// or impossible framing, and then the header.
std::vector<uint8_t> framed(const std::string& header, uint64_t declared) {
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 8; ++i)
        bytes.push_back(uint8_t(declared >> (8 * i)));
    bytes.insert(bytes.end(), header.begin(), header.end());
    return bytes;
}

// Returns a header of one tensor named `key`, of the shape [rows, `width`].
std::string tensorHeader(const std::string& key, uint32_t width, uint32_t rows = 640) {
    return "{\"" + key + "\":{\"dtype\":\"F16\",\"shape\":[" + std::to_string(rows) + "," + std::to_string(width) +
        "],\"data_offsets\":[0,16]}}";
}

std::string unetHeader(uint32_t context) {
    return tensorHeader("model.diffusion_model.input_blocks.4.1.transformer_blocks.0.attn2.to_k.weight", context);
}

std::string spec(const char* architecture) {
    return std::string("{\"__metadata__\":{\"modelspec.architecture\":\"") + architecture + "\"}}";
}

} // namespace

TEST_CASE("a model spec, then a Kohya base model, then the tensors name the architecture") {
    CHECK(architectureFromHeader(spec("stable-diffusion-xl-v1-base")) == ModelArchitecture_Sdxl);
    CHECK(architectureFromHeader(spec("stable-diffusion-xl-v1-refiner")) == ModelArchitecture_SdxlRefiner);
    CHECK(architectureFromHeader(spec("stable-diffusion-v1/lora")) == ModelArchitecture_Sd15);
    CHECK(architectureFromHeader(spec("Flux.1-dev")) == ModelArchitecture_Flux);
    CHECK(architectureFromHeader(spec("stable-diffusion-3-medium")) == ModelArchitecture_Sd3);
    CHECK(architectureFromHeader(spec("stable-cascade-v1-prior")) == ModelArchitecture_Cascade);
    CHECK(architectureFromHeader("{\"__metadata__\":{\"ss_base_model_version\":\"sdxl_base_v1-0\"}}") ==
        ModelArchitecture_Sdxl);
    CHECK(architectureFromHeader("{\"__metadata__\":{\"ss_base_model_version\":\"sd_v1-5\"}}") ==
        ModelArchitecture_Sd15);
    // Metadata that names nothing falls through to the tensors, which a stripped checkpoint still carries.
    CHECK(architectureFromHeader("{\"__metadata__\":{\"format\":\"pt\"}," + unetHeader(2048).substr(1)) ==
        ModelArchitecture_Sdxl);
}

TEST_CASE("a cross-attention width, a signature block or an adapter's down projection names the architecture") {
    CHECK(architectureFromHeader(unetHeader(768)) == ModelArchitecture_Sd15);
    CHECK(architectureFromHeader(unetHeader(2048)) == ModelArchitecture_Sdxl);
    CHECK(architectureFromHeader(unetHeader(1280)) == ModelArchitecture_SdxlRefiner);
    CHECK(architectureFromHeader(unetHeader(1024)) == ModelArchitecture_Unknown);
    CHECK(architectureFromHeader(tensorHeader("model.diffusion_model.double_blocks.0.img_attn.qkv.weight", 3072)) ==
        ModelArchitecture_Flux);
    CHECK(architectureFromHeader(tensorHeader("model.diffusion_model.joint_blocks.0.context_block.attn.qkv.weight", 1536)) ==
        ModelArchitecture_Sd3);
    std::string kohya = "lora_unet_input_blocks_4_1_transformer_blocks_0_attn2_to_k.lora_down.weight";
    CHECK(architectureFromHeader(tensorHeader(kohya, 768, 8)) == ModelArchitecture_Sd15);
    CHECK(architectureFromHeader(tensorHeader(kohya, 2048, 8)) == ModelArchitecture_Sdxl);
    CHECK(architectureFromHeader(
        tensorHeader("unet.down_blocks.1.attentions.0.transformer_blocks.0.attn2.to_k.lora_A.weight", 2048, 8)) ==
        ModelArchitecture_Sdxl);
    CHECK(architectureFromHeader("not json") == ModelArchitecture_Unknown);
    CHECK(architectureFromHeader("[]") == ModelArchitecture_Unknown);
}

TEST_CASE("a file's leading bytes frame its header, and a framing that does not fit names nothing") {
    std::string header = spec("stable-diffusion-xl-v1-base");
    CHECK(architectureFromPrefix(framed(header, header.size())) == ModelArchitecture_Sdxl);
    CHECK(architectureFromPrefix(framed(header, 4096)) == ModelArchitecture_Unknown);
    CHECK(architectureFromPrefix(framed("{}", 1ull << 40)) == ModelArchitecture_Unknown);
    CHECK(architectureFromPrefix(std::vector<uint8_t>{1, 2, 3}) == ModelArchitecture_Unknown);
    CHECK(readArchitecture("/nonexistent/model.safetensors") == ModelArchitecture_Unknown);
}

TEST_CASE("architectures conflict only if both are known and their bases differ") {
    CHECK(architecturesConflict(ModelArchitecture_Sd15, ModelArchitecture_Sdxl));
    CHECK(architecturesConflict(ModelArchitecture_Flux, ModelArchitecture_Sd3));
    CHECK_FALSE(architecturesConflict(ModelArchitecture_Sdxl, ModelArchitecture_Sdxl));
    CHECK_FALSE(architecturesConflict(ModelArchitecture_SdxlRefiner, ModelArchitecture_Sdxl));
    CHECK_FALSE(architecturesConflict(ModelArchitecture_Unknown, ModelArchitecture_Sdxl));
}

TEST_CASE("an essential model is found by its name or its file") {
    CHECK(findEssentialModel("sdxl")->kind == EssentialKind_Checkpoint);
    CHECK(findEssentialModel("RealESRGAN_x4plus.pth")->kind == EssentialKind_Upscale);
    CHECK(findEssentialModel("") == nullptr);
    CHECK(findEssentialModel("nothing") == nullptr);
    CHECK(essentialModel(EssentialKind_Upscale).type == "upscale");
}

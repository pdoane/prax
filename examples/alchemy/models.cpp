// Models: what a model file states about its architecture, and the essential models.

#include "examples/alchemy/models.h"

#include "pr/ascii.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/json.h"
#include "pr/string_ops.h"

#include "examples/alchemy/text.h"

namespace alchemy {

using namespace pr;

namespace {

// The safetensors format caps the JSON header at 100 MB, so a larger length is a file that is not one.
constexpr uint64_t kMaxHeaderBytes = 100ull * 1024 * 1024;
constexpr size_t kLengthBytes = 8;

// The cross-attention context widths that the UNet architectures were trained with.
constexpr double kSd15Context = 768.0;
constexpr double kSdxlContext = 2048.0;
constexpr double kSdxlRefinerContext = 1280.0;

// The `type` strings are the manager's keys for its folders, not the folders' names: "upscale" is what resolves to
// models/upscale_models.
constexpr EssentialModel kEssentials[] = {
    {
        .kind = EssentialKind_Checkpoint,
        .name = "SDXL Base 1.0",
        .type = "checkpoints",
        .base = "SDXL",
        .filename = "sd_xl_base_1.0.safetensors",
        .url = "https://huggingface.co/stabilityai/stable-diffusion-xl-base-1.0/resolve/main/"
            "sd_xl_base_1.0.safetensors",
        .folder = "models/checkpoints",
    },
    {
        .kind = EssentialKind_Upscale,
        .name = "RealESRGAN x4plus",
        .type = "upscale",
        .base = "upscale",
        .filename = "RealESRGAN_x4plus.pth",
        .url = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth",
        .folder = "models/upscale_models",
    },
};
static_assert(countOf(kEssentials) == EssentialKind_Count);

uint64_t readLittleEndian64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < kLengthBytes; ++i)
        value |= uint64_t(bytes[i]) << (8 * i);
    return value;
}

// Reads `modelspec.architecture` as the reference tooling writes it: an identifier whose first segment names the model.
ModelArchitecture fromModelSpec(PrStringView architecture) {
    if (architecture.size == 0)
        return ModelArchitecture_Unknown;
    if (prAsciiContainsFold(architecture, "stable-cascade"))
        return ModelArchitecture_Cascade;
    if (prAsciiContainsFold(architecture, "flux"))
        return ModelArchitecture_Flux;
    if (prAsciiContainsFold(architecture, "stable-diffusion-3") || prAsciiContainsFold(architecture, "sd3"))
        return ModelArchitecture_Sd3;
    if (prAsciiContainsFold(architecture, "stable-diffusion-xl"))
        return prAsciiContainsFold(architecture, "refiner") ? ModelArchitecture_SdxlRefiner : ModelArchitecture_Sdxl;
    if (prAsciiContainsFold(architecture, "stable-diffusion-v1"))
        return ModelArchitecture_Sd15;
    return ModelArchitecture_Unknown;
}

// Reads `ss_base_model_version` as a Kohya training run records it: the base that the weights were trained against,
// prefixed by its family.
ModelArchitecture fromKohyaBaseModel(PrStringView version) {
    if (prStringStartsWith(version, "sdxl_"))
        return ModelArchitecture_Sdxl;
    if (prStringStartsWith(version, "sd_v1"))
        return ModelArchitecture_Sd15;
    return ModelArchitecture_Unknown;
}

ModelArchitecture fromContextWidth(double width) {
    if (width == kSd15Context)
        return ModelArchitecture_Sd15;
    if (width == kSdxlContext)
        return ModelArchitecture_Sdxl;
    if (width == kSdxlRefinerContext)
        return ModelArchitecture_SdxlRefiner;
    return ModelArchitecture_Unknown;
}

// Returns the second dimension of a tensor's shape, which for a cross-attention key projection is the width of the
// conditioning that it takes.
double contextWidthOf(const PrJson* d, PrJsonValueIndex tensor) {
    PrJsonValueIndex shape = prJsonMember(d, tensor, "shape");
    return prJsonNumber(d, prJsonElement(d, shape, 1), 0.0);
}

// Returns true if `key` names the down projection of a cross-attention key, in the Kohya naming or in the diffusers
// naming of adapters.
bool isCrossAttentionKeyDown(PrStringView key) {
    if (!prStringContains(key, "attn2") || !prStringContains(key, "to_k"))
        return false;
    return prStringEndsWith(key, "lora_down.weight") || prStringEndsWith(key, "lora.down.weight") ||
        prStringEndsWith(key, "lora_A.weight");
}

ModelArchitecture fromTensors(const PrJson* d) {
    for (PrJsonValueIndex i = prJsonFirst(d, 0); i != kPrJsonMissing; i = prJsonAfter(d, 0, i)) {
        PrStringView key = prJsonString(d, i);
        if (prStringContains(key, "double_blocks."))
            return ModelArchitecture_Flux;
        if (prStringContains(key, "joint_blocks."))
            return ModelArchitecture_Sd3;
        bool adapted = isCrossAttentionKeyDown(key);
        bool unet = prStringStartsWith(key, "model.diffusion_model.") && prStringEndsWith(key, "attn2.to_k.weight");
        if (!adapted && !unet)
            continue;
        ModelArchitecture arch = fromContextWidth(contextWidthOf(d, i + 1));
        if (arch != ModelArchitecture_Unknown)
            return arch;
    }
    return ModelArchitecture_Unknown;
}

} // namespace

PrStringView architectureName(ModelArchitecture arch) {
    switch (arch) {
        case ModelArchitecture_Unknown: return "unknown";
        case ModelArchitecture_Sd15: return "sd15";
        case ModelArchitecture_Sdxl: return "sdxl";
        case ModelArchitecture_SdxlRefiner: return "sdxl-refiner";
        case ModelArchitecture_Sd3: return "sd3";
        case ModelArchitecture_Flux: return "flux";
        case ModelArchitecture_Cascade: return "cascade";
    }
    return "unknown";
}

bool architecturesConflict(ModelArchitecture a, ModelArchitecture b) {
    auto base = [](ModelArchitecture arch) {
        return arch == ModelArchitecture_SdxlRefiner ? ModelArchitecture_Sdxl : arch;
    };
    if (a == ModelArchitecture_Unknown || b == ModelArchitecture_Unknown)
        return false;
    return base(a) != base(b);
}

ModelArchitecture architectureFromHeader(PrStringView headerJson) {
    PrJson* d = prJsonCreate();
    ModelArchitecture arch = ModelArchitecture_Unknown;
    if (prJsonParse(d, headerJson, nullptr) && prJsonKind(d, 0) == PrJsonKind_Object) {
        PrJsonValueIndex meta = prJsonMember(d, 0, "__metadata__");
        arch = fromModelSpec(prJsonString(d, prJsonMember(d, meta, "modelspec.architecture")));
        if (arch == ModelArchitecture_Unknown)
            arch = fromKohyaBaseModel(prJsonString(d, prJsonMember(d, meta, "ss_base_model_version")));
        if (arch == ModelArchitecture_Unknown)
            arch = fromTensors(d);
    }
    prJsonDestroy(d);
    return arch;
}

ModelArchitecture architectureFromPrefix(PrByteView leading) {
    if (leading.size < kLengthBytes)
        return ModelArchitecture_Unknown;
    uint64_t length = readLittleEndian64(leading.data);
    if (length == 0 || length > kMaxHeaderBytes || length > leading.size - kLengthBytes)
        return ModelArchitecture_Unknown;
    return architectureFromHeader(
        PrStringView(reinterpret_cast<const char*>(leading.data) + kLengthBytes, size_t(length)));
}

ModelArchitecture readArchitecture(PrStringView path) {
    PrFileMapping mapping{};
    if (!prFileMap(path, &mapping))
        return ModelArchitecture_Unknown;
    ModelArchitecture arch = architectureFromPrefix(PrByteView{mapping.data, mapping.size});
    prFileUnmap(&mapping);
    return arch;
}

PrStringView modelFolderName(ModelFolder folder) {
    return folder == ModelFolder_Loras ? "loras" : "checkpoints";
}

const EssentialModel& essentialModel(EssentialKind kind) {
    return kEssentials[size_t(kind)];
}

const EssentialModel* PR_NULLABLE findEssentialModel(PrStringView name) {
    if (name.size == 0)
        return nullptr;
    for (const EssentialModel& model : kEssentials) {
        if (prAsciiContainsFold(model.name, name) || prAsciiContainsFold(model.filename, name))
            return &model;
    }
    return nullptr;
}

} // namespace alchemy

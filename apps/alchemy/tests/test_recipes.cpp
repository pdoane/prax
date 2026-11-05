#include "pr/array.h"
#include "pr/json.h"
#include "pr/string.h"

#include "apps/alchemy/recipes.h"

#include "tools/harness/test.h"

using namespace alchemy;

namespace {

// A graph read back from its text, which the checks below query by node id.
struct Graph {
    PrJson* d = prJsonCreate();

    explicit Graph(PrStringView text) {
        REQUIRE(prJsonParse(d, text, nullptr));
    }
    ~Graph() {
        prJsonDestroy(d);
    }

    bool has(const char* id) const {
        return prJsonMember(d, 0, id) != kPrJsonMissing;
    }

    // Returns the inputs of the node `id`, which must be of `classType`.
    PrJsonValueIndex inputs(const char* id, const char* classType) const {
        PrJsonValueIndex node = prJsonMember(d, 0, id);
        REQUIRE(node != kPrJsonMissing);
        CHECK(prJsonString(d, prJsonMember(d, node, "class_type")) == classType);
        return prJsonMember(d, node, "inputs");
    }

    PrStringView text(PrJsonValueIndex inputs, const char* key) const {
        return prJsonString(d, prJsonMember(d, inputs, key));
    }

    double number(PrJsonValueIndex inputs, const char* key) const {
        return prJsonNumber(d, prJsonMember(d, inputs, key), -1);
    }

    // Returns true if the input `key` links to output `slot` of the node `node`.
    bool linksTo(PrJsonValueIndex inputs, const char* key, const char* node, double slot) const {
        PrJsonValueIndex ref = prJsonMember(d, inputs, key);
        return prJsonCount(d, ref) == 2 && prJsonString(d, prJsonElement(d, ref, 0)) == node &&
            prJsonNumber(d, prJsonElement(d, ref, 1), -1) == slot;
    }
};

SamplingRecipe sampling(Array<LoraRef> loras = {}, PrStringView negative = "") {
    return SamplingRecipe{
        .checkpoint = "sdxl.safetensors",
        .loras = move(loras),
        .sampler = "euler",
        .scheduler = "normal",
        .prompt = "a fox",
        .negativePrompt = negative,
        .seed = 42,
        .cfg = 6.5,
    };
}

// Returns the adapter of `name` at `weight`.
LoraRef lora(const char* name, double weight) {
    return LoraRef{.name = name, .weight = weight};
}

Array<LoraRef> chain() {
    Array<LoraRef> loras;
    loras.push(lora("detail.safetensors", 0.8));
    loras.push(lora("film.safetensors", 1.25));
    return loras;
}

} // namespace

TEST_CASE("a text-to-image graph without adapters reads the checkpoint, and zeroes an empty negative prompt") {
    Graph g(txt2ImgGraph({.sampling = sampling(), .width = 832, .height = 1216, .batch = 2}));
    CHECK_FALSE(g.has("lora0"));
    PrJsonValueIndex sampler = g.inputs("sampler", "KSampler");
    CHECK(g.linksTo(sampler, "model", "checkpoint", 0));
    CHECK(g.number(sampler, "seed") == 42);
    CHECK(g.number(sampler, "cfg") == 6.5);
    CHECK(g.number(sampler, "denoise") == 1);
    CHECK(g.linksTo(g.inputs("positive", "CLIPTextEncode"), "clip", "checkpoint", 1));
    CHECK(g.linksTo(g.inputs("negative", "ConditioningZeroOut"), "conditioning", "positive", 0));
    PrJsonValueIndex latent = g.inputs("latent", "EmptyLatentImage");
    CHECK(g.number(latent, "width") == 832);
    CHECK(g.number(latent, "batch_size") == 2);
    CHECK(g.text(g.inputs("save", "SaveImage"), "filename_prefix") == kSavePrefix);
}

TEST_CASE("adapters chain between the checkpoint and everything that reads a model or a text encoder") {
    Graph g(txt2ImgGraph({.sampling = sampling(chain(), "blurry")}));
    PrJsonValueIndex first = g.inputs("lora0", "LoraLoader");
    CHECK(g.text(first, "lora_name") == "detail.safetensors");
    CHECK(g.number(first, "strength_model") == 0.8);
    CHECK(g.number(first, "strength_clip") == 0.8);
    CHECK(g.linksTo(first, "model", "checkpoint", 0));
    CHECK(g.linksTo(first, "clip", "checkpoint", 1));
    PrJsonValueIndex second = g.inputs("lora1", "LoraLoader");
    CHECK(g.number(second, "strength_model") == 1.25);
    CHECK(g.linksTo(second, "model", "lora0", 0));
    CHECK(g.linksTo(second, "clip", "lora0", 1));
    // The VAE is the checkpoint's, which no adapter changes.
    CHECK(g.linksTo(g.inputs("sampler", "KSampler"), "model", "lora1", 0));
    CHECK(g.linksTo(g.inputs("positive", "CLIPTextEncode"), "clip", "lora1", 1));
    CHECK(g.linksTo(g.inputs("negative", "CLIPTextEncode"), "clip", "lora1", 1));
    CHECK(g.linksTo(g.inputs("decode", "VAEDecode"), "vae", "checkpoint", 2));
}

TEST_CASE("an image-to-image graph encodes the source at the requested size and denoises part of it") {
    Graph g(txt2ImgGraph({.sampling = sampling(), .sourceImage = "fox.png", .denoise = 0.6, .width = 768}));
    CHECK(g.text(g.inputs("source", "LoadImage"), "image") == "fox.png");
    CHECK(g.number(g.inputs("resize", "ImageScale"), "width") == 768);
    CHECK(g.linksTo(g.inputs("latent", "VAEEncode"), "pixels", "resize", 0));
    CHECK(g.number(g.inputs("sampler", "KSampler"), "denoise") == 0.6);
}

TEST_CASE("an outpaint grows the canvas on one side and samples a crop anchored at the edge that grew") {
    OutpaintRecipe recipe{
        .sampling = sampling(chain()),
        .sourceImage = "fox.png",
        .direction = OutpaintDirection_Left,
        .sourceWidth = 1000,
        .sourceHeight = 1000,
        .amount = 256,
        .context = 512,
    };
    CHECK(outpaintWidth(recipe) == 1256);
    CHECK(outpaintHeight(recipe) == 1000);
    Graph g(outpaintGraph(recipe));
    PrJsonValueIndex pad = g.inputs("pad", "ImagePadForOutpaint");
    CHECK(g.number(pad, "left") == 256);
    CHECK(g.number(pad, "right") == 0);
    PrJsonValueIndex crop = g.inputs("crop", "ImageCrop");
    CHECK(g.number(crop, "x") == 0);
    CHECK(g.number(crop, "width") == 768);
    CHECK(g.number(crop, "height") == 1000);
    CHECK(g.linksTo(g.inputs("sampler", "KSampler"), "model", "lora1", 0));
    CHECK(g.linksTo(g.inputs("encode", "VAEEncode"), "vae", "checkpoint", 2));
    CHECK(g.linksTo(g.inputs("save", "SaveImage"), "images", "composite", 0));
}

TEST_CASE("a detail pass reads the chain's model and text encoder") {
    Graph g(detailGraph({
        .sampling = sampling(Array<LoraRef>({lora("detail.safetensors", 0.8)})),
        .sourceImage = "fox.png",
        .detector = "bbox/face_yolov8m.pt",
    }));
    PrJsonValueIndex detail = g.inputs("detail", "FaceDetailer");
    CHECK(g.linksTo(detail, "model", "lora0", 0));
    CHECK(g.linksTo(detail, "clip", "lora0", 1));
    CHECK(g.linksTo(detail, "vae", "checkpoint", 2));
    CHECK(g.text(g.inputs("detector", "UltralyticsDetectorProvider"), "model_name") == "bbox/face_yolov8m.pt");
}

TEST_CASE("an upscale rescales by what the model's name states, and runs the model alone otherwise") {
    Graph x4(upscaleGraph({.model = "RealESRGAN_x4plus.pth", .sourceImage = "fox.png", .factor = 2}));
    CHECK_FALSE(x4.has("checkpoint"));
    CHECK(x4.number(x4.inputs("rescale", "ImageScaleBy"), "scale_by") == 0.5);
    Graph exact(upscaleGraph({.model = "4x-UltraSharp.pth", .sourceImage = "fox.png", .factor = 4}));
    CHECK_FALSE(exact.has("rescale"));
    Graph unknown(upscaleGraph({.model = "mystery.pth", .sourceImage = "fox.png", .factor = 2}));
    CHECK_FALSE(unknown.has("rescale"));
    CHECK(upscaleModelFactor("RealESRGAN_x4plus.pth") == 4);
    CHECK(upscaleModelFactor("2X_model") == 2);
    CHECK(upscaleModelFactor("plain") == 0);
}

TEST_CASE("a direction reads back from its name") {
    const OutpaintDirection directions[] = {
        OutpaintDirection_Left,
        OutpaintDirection_Top,
        OutpaintDirection_Right,
        OutpaintDirection_Bottom,
    };
    for (OutpaintDirection direction : directions)
        CHECK(parseDirection(directionName(direction), OutpaintDirection_Right) == direction);
    CHECK(parseDirection("TOP", OutpaintDirection_Right) == OutpaintDirection_Top);
    CHECK(parseDirection("sideways", OutpaintDirection_Bottom) == OutpaintDirection_Bottom);
}

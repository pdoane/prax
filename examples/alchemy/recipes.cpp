// Recipes: the ComfyUI graphs of alchemy's operations.

#include "examples/alchemy/recipes.h"

#include "pr/ascii.h"

#include "examples/alchemy/comfy_graph.h"
#include "examples/alchemy/text.h"

#include <algorithm>
#include <cmath>

namespace alchemy {

using namespace pr;

namespace {

constexpr PrStringView kLanczos = "lanczos";

// Returns the scale that the graph applies after the model, or 0 if the model's output is already what was asked for or
// its factor is not known.
double rescaleBy(double factor, double modelFactor) {
    if (modelFactor <= 0.0)
        return 0.0;
    double scale = factor / modelFactor;
    return std::fabs(scale - 1.0) < 0.01 ? 0.0 : scale;
}

// The node whose outputs a sampling recipe works from: the checkpoint's own if it loads no adapter, and the last
// adapter's otherwise. Both loaders put the model on output 0 and the text encoder on output 1. The VAE is always the
// checkpoint's, which no adapter changes.
struct SamplingRefs {
    std::string node = "checkpoint";

    ComfyRef model() const { return {node, 0}; }
    ComfyRef clip() const { return {node, 1}; }
};

// Writes the checkpoint, the adapters chained onto it, and the two conditionings that a sampling recipe threads into
// its sampler.
SamplingRefs emitModelAndPrompts(ComfyGraph& g, const SamplingRecipe& recipe) {
    g.node("checkpoint", "CheckpointLoaderSimple");
    g.setText("ckpt_name", recipe.checkpoint);

    SamplingRefs refs;
    for (size_t i = 0; i < recipe.loras.size(); ++i) {
        std::string id = "lora" + std::to_string(i);
        g.node(id, "LoraLoader");
        g.setRef("model", refs.model());
        g.setRef("clip", refs.clip());
        g.setText("lora_name", recipe.loras[i].name);
        g.setNumber("strength_model", recipe.loras[i].weight);
        g.setNumber("strength_clip", recipe.loras[i].weight);
        refs.node = std::move(id);
    }

    g.node("positive", "CLIPTextEncode");
    g.setText("text", recipe.prompt);
    g.setRef("clip", refs.clip());

    if (recipe.negativePrompt.empty()) {
        g.node("negative", "ConditioningZeroOut");
        g.setRef("conditioning", {"positive", 0});
    } else {
        g.node("negative", "CLIPTextEncode");
        g.setText("text", recipe.negativePrompt);
        g.setRef("clip", refs.clip());
    }
    return refs;
}

// Writes a KSampler over `latent` with the recipe's settings.
void emitSampler(ComfyGraph& g, const SamplingRecipe& recipe, const SamplingRefs& refs, ComfyRef latent,
                 double denoise) {
    g.node("sampler", "KSampler");
    g.setRef("model", refs.model());
    g.setRef("positive", {"positive", 0});
    g.setRef("negative", {"negative", 0});
    g.setRef("latent_image", latent);
    g.setNumber("seed", double(recipe.seed));
    g.setNumber("steps", recipe.steps);
    g.setNumber("cfg", recipe.cfg);
    g.setText("sampler_name", recipe.sampler);
    g.setText("scheduler", recipe.scheduler);
    g.setNumber("denoise", denoise);
}

void emitSave(ComfyGraph& g, ComfyRef images) {
    g.node("save", "SaveImage");
    g.setRef("images", images);
    g.setText("filename_prefix", kSavePrefix);
}

bool isHorizontal(OutpaintDirection direction) {
    return direction == OutpaintDirection_Left || direction == OutpaintDirection_Right;
}

// Rounds down to the latent grid, which the VAE encodes in blocks of 8, with one block as the floor.
uint32_t snapToLatentGrid(uint32_t value) {
    uint32_t snapped = value & ~7u;
    return snapped < 8 ? 8 : snapped;
}

// The padded canvas of an outpaint and the rectangle that the sampler works over. The crop is anchored at the edge that
// grew, so that rounding the sampled span down to the latent grid takes context off the far side rather than leaving a
// strip of the new region unpainted.
struct OutpaintCrop {
    uint32_t padWidth = 0;
    uint32_t padHeight = 0;
    uint32_t left = 0;
    uint32_t top = 0;
    uint32_t right = 0;
    uint32_t bottom = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

OutpaintCrop outpaintCrop(const OutpaintRecipe& recipe) {
    OutpaintCrop crop;
    crop.left = recipe.direction == OutpaintDirection_Left ? recipe.amount : 0;
    crop.top = recipe.direction == OutpaintDirection_Top ? recipe.amount : 0;
    crop.right = recipe.direction == OutpaintDirection_Right ? recipe.amount : 0;
    crop.bottom = recipe.direction == OutpaintDirection_Bottom ? recipe.amount : 0;
    crop.padWidth = recipe.sourceWidth + crop.left + crop.right;
    crop.padHeight = recipe.sourceHeight + crop.top + crop.bottom;

    bool horizontal = isHorizontal(recipe.direction);
    uint32_t axis = horizontal ? recipe.sourceWidth : recipe.sourceHeight;
    uint32_t span = snapToLatentGrid(std::min(recipe.context, axis) + recipe.amount);
    if (horizontal) {
        crop.width = std::min(span, crop.padWidth);
        crop.height = std::min(snapToLatentGrid(crop.padHeight), crop.padHeight);
        crop.x = recipe.direction == OutpaintDirection_Left ? 0u : crop.padWidth - crop.width;
    } else {
        crop.height = std::min(span, crop.padHeight);
        crop.width = std::min(snapToLatentGrid(crop.padWidth), crop.padWidth);
        crop.y = recipe.direction == OutpaintDirection_Top ? 0u : crop.padHeight - crop.height;
    }
    return crop;
}

} // namespace

std::string txt2ImgGraph(const Txt2ImgRecipe& recipe) {
    ComfyGraph g;
    SamplingRefs refs = emitModelAndPrompts(g, recipe.sampling);
    bool fromSource = !recipe.sourceImage.empty();
    if (!fromSource) {
        g.node("latent", "EmptyLatentImage");
        g.setNumber("width", recipe.width);
        g.setNumber("height", recipe.height);
        g.setNumber("batch_size", recipe.batch);
    } else {
        g.node("source", "LoadImage");
        g.setText("image", recipe.sourceImage);

        g.node("resize", "ImageScale");
        g.setRef("image", {"source", 0});
        g.setText("upscale_method", kLanczos);
        g.setNumber("width", recipe.width);
        g.setNumber("height", recipe.height);
        g.setText("crop", "center");

        g.node("latent", "VAEEncode");
        g.setRef("pixels", {"resize", 0});
        g.setRef("vae", {"checkpoint", 2});
    }
    emitSampler(g, recipe.sampling, refs, {"latent", 0}, fromSource ? recipe.denoise : 1.0);

    g.node("decode", "VAEDecode");
    g.setRef("samples", {"sampler", 0});
    g.setRef("vae", {"checkpoint", 2});
    emitSave(g, {"decode", 0});
    return g.build();
}

std::string upscaleGraph(const UpscaleRecipe& recipe) {
    ComfyGraph g;
    g.node("source", "LoadImage");
    g.setText("image", recipe.sourceImage);

    g.node("model", "UpscaleModelLoader");
    g.setText("model_name", recipe.model);

    g.node("upscale", "ImageUpscaleWithModel");
    g.setRef("upscale_model", {"model", 0});
    g.setRef("image", {"source", 0});

    double scale = rescaleBy(recipe.factor, upscaleModelFactor(recipe.model));
    ComfyRef image{"upscale", 0};
    if (scale > 0.0) {
        g.node("rescale", "ImageScaleBy");
        g.setRef("image", image);
        g.setText("upscale_method", kLanczos);
        g.setNumber("scale_by", scale);
        image = {"rescale", 0};
    }
    emitSave(g, image);
    return g.build();
}

std::string outpaintGraph(const OutpaintRecipe& recipe) {
    OutpaintCrop crop = outpaintCrop(recipe);
    ComfyGraph g;
    SamplingRefs refs = emitModelAndPrompts(g, recipe.sampling);

    g.node("load", "LoadImage");
    g.setText("image", recipe.sourceImage);

    g.node("pad", "ImagePadForOutpaint");
    g.setRef("image", {"load", 0});
    g.setNumber("left", crop.left);
    g.setNumber("top", crop.top);
    g.setNumber("right", crop.right);
    g.setNumber("bottom", crop.bottom);
    g.setNumber("feathering", recipe.crossfade);

    g.node("crop", "ImageCrop");
    g.setRef("image", {"pad", 0});
    g.setNumber("width", crop.width);
    g.setNumber("height", crop.height);
    g.setNumber("x", crop.x);
    g.setNumber("y", crop.y);

    g.node("cropmask", "CropMask");
    g.setRef("mask", {"pad", 1});
    g.setNumber("x", crop.x);
    g.setNumber("y", crop.y);
    g.setNumber("width", crop.width);
    g.setNumber("height", crop.height);

    // The crop encodes as plain pixels, not for inpainting, so that the sampler sees the real image on the near side of
    // the seam and continues it.
    g.node("encode", "VAEEncode");
    g.setRef("pixels", {"crop", 0});
    g.setRef("vae", {"checkpoint", 2});

    // Sampling reaches past the feathered edge, while the blend follows the feather itself, so the two masks differ.
    g.node("grow", "GrowMask");
    g.setRef("mask", {"cropmask", 0});
    g.setNumber("expand", recipe.growMask);
    g.setBool("tapered_corners", true);

    g.node("noisemask", "SetLatentNoiseMask");
    g.setRef("samples", {"encode", 0});
    g.setRef("mask", {"grow", 0});

    emitSampler(g, recipe.sampling, refs, {"noisemask", 0}, 1.0);

    g.node("decode", "VAEDecode");
    g.setRef("samples", {"sampler", 0});
    g.setRef("vae", {"checkpoint", 2});

    g.node("composite", "ImageCompositeMasked");
    g.setRef("destination", {"pad", 0});
    g.setRef("source", {"decode", 0});
    g.setNumber("x", crop.x);
    g.setNumber("y", crop.y);
    g.setBool("resize_source", false);
    g.setRef("mask", {"cropmask", 0});
    emitSave(g, {"composite", 0});
    return g.build();
}

std::string detailGraph(const DetailRecipe& recipe) {
    ComfyGraph g;
    SamplingRefs refs = emitModelAndPrompts(g, recipe.sampling);
    const SamplingRecipe& sampling = recipe.sampling;

    g.node("load", "LoadImage");
    g.setText("image", recipe.sourceImage);

    g.node("detector", "UltralyticsDetectorProvider");
    g.setText("model_name", recipe.detector);

    g.node("detail", "FaceDetailer");
    g.setRef("image", {"load", 0});
    g.setRef("model", refs.model());
    g.setRef("clip", refs.clip());
    g.setRef("vae", {"checkpoint", 2});
    g.setNumber("guide_size", 512);
    g.setBool("guide_size_for", true);
    g.setNumber("max_size", 1024);
    g.setNumber("seed", double(sampling.seed));
    g.setNumber("steps", sampling.steps);
    g.setNumber("cfg", sampling.cfg);
    g.setText("sampler_name", sampling.sampler);
    g.setText("scheduler", sampling.scheduler);
    g.setRef("positive", {"positive", 0});
    g.setRef("negative", {"negative", 0});
    g.setNumber("denoise", 0.5);
    g.setNumber("feather", 5);
    g.setBool("noise_mask", true);
    g.setBool("force_inpaint", true);
    g.setNumber("bbox_threshold", 0.5);
    g.setNumber("bbox_dilation", 10);
    g.setNumber("bbox_crop_factor", 3);
    g.setText("sam_detection_hint", "center-1");
    g.setNumber("sam_dilation", 0);
    g.setNumber("sam_threshold", 0.93);
    g.setNumber("sam_bbox_expansion", 0);
    g.setNumber("sam_mask_hint_threshold", 0.7);
    g.setText("sam_mask_hint_use_negative", "False");
    g.setNumber("drop_size", 10);
    g.setRef("bbox_detector", {"detector", 0});
    g.setText("wildcard", "");
    g.setNumber("cycle", 1);
    emitSave(g, {"detail", 0});
    return g.build();
}

PrStringView directionName(OutpaintDirection direction) {
    switch (direction) {
        case OutpaintDirection_Left: return "left";
        case OutpaintDirection_Top: return "top";
        case OutpaintDirection_Right: return "right";
        case OutpaintDirection_Bottom: return "bottom";
    }
    return "right";
}

OutpaintDirection parseDirection(PrStringView name, OutpaintDirection fallback) {
    for (OutpaintDirection direction : {OutpaintDirection_Left, OutpaintDirection_Top, OutpaintDirection_Right,
                                        OutpaintDirection_Bottom}) {
        if (asciiContainsFold(name, directionName(direction)))
            return direction;
    }
    return fallback;
}

uint32_t outpaintWidth(const OutpaintRecipe& recipe) {
    return outpaintCrop(recipe).padWidth;
}

uint32_t outpaintHeight(const OutpaintRecipe& recipe) {
    return outpaintCrop(recipe).padHeight;
}

double upscaleModelFactor(PrStringView model) {
    struct Tag {
        PrStringView tag;
        double factor;
    };
    static constexpr Tag kTags[] = {{"x2", 2}, {"2x", 2}, {"x3", 3}, {"3x", 3},
                                    {"x4", 4}, {"4x", 4}, {"x8", 8}, {"8x", 8}};
    for (const Tag& tag : kTags) {
        if (asciiContainsFold(model, tag.tag))
            return tag.factor;
    }
    return 0.0;
}

} // namespace alchemy

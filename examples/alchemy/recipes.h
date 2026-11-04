// Recipes: the ComfyUI graphs of the operations that alchemy runs, built from the names the server knows.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/string.h"
#include "pr/string_view.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

using namespace pr;

// The prefix of the files that a run's SaveImage node writes on the server.
constexpr PrStringView kSavePrefix = "Alchemy";

// An adapter that a sampling recipe chains onto its checkpoint, by its file's name on the server, at one weight for the
// diffusion model and the text encoder alike. The chain applies in order: each adapter takes the model and the text
// encoder that the one before it produced, under the node ids "lora0", "lora1" and so on.
struct LoraRef {
    PrStringView name;
    double weight = 1.0;
};

// What every sampling recipe reads: the model, its adapters, the prompts and the sampler's settings. The model, sampler
// and scheduler are names from the server's vocabulary. A recipe is the argument of one graph's build: its text views
// the caller's strings, which must stay alive until the build returns.
struct SamplingRecipe {
    PrStringView checkpoint;
    Array<LoraRef> loras;
    PrStringView sampler;
    PrStringView scheduler;
    PrStringView prompt;
    PrStringView negativePrompt;
    uint64_t seed = 0;
    uint32_t steps = 20;
    double cfg = 7.0;
};

// A text-to-image run. A non-empty `sourceImage`, the name that the server stored an uploaded image under, makes it an
// image-to-image run: the source is scaled to the requested size and encoded in place of an empty latent, and `denoise`
// is how much of it the sampler may discard. A batch applies only to the empty latent.
struct Txt2ImgRecipe {
    SamplingRecipe sampling;
    PrStringView sourceImage;
    double denoise = 1.0;
    uint32_t width = 1024;
    uint32_t height = 1024;
    uint32_t batch = 1;
};

// An upscale model's pass over an uploaded image. The model enlarges by its own factor, which its name states. The
// graph rescales what the model made to `factor` if the name states the model's factor, and runs the model alone if it
// does not, since a rescale by a guessed ratio would misreport what the model did.
struct UpscaleRecipe {
    PrStringView model;
    PrStringView sourceImage;
    double factor = 2.0;
};

// The side that an outpaint grows the canvas on.
enum {
    OutpaintDirection_Left,
    OutpaintDirection_Top,
    OutpaintDirection_Right,
    OutpaintDirection_Bottom,
};
typedef uint8_t OutpaintDirection;

// An extension of an uploaded image on one side. The crop that the sampler works over is measured against the source's
// own pixel size, which the recipe must state. `amount` is how far the canvas grows, `context` how much of the source
// travels with the new region into the sampled crop, `crossfade` the feathering of the seam, and `growMask` how far the
// sampled region reaches past the feathered seam.
struct OutpaintRecipe {
    SamplingRecipe sampling;
    PrStringView sourceImage;
    OutpaintDirection direction = OutpaintDirection_Right;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t amount = 256;
    uint32_t crossfade = 64;
    uint32_t growMask = 8;
    uint32_t context = 512;
};

// A face detailer's pass over an uploaded image: the detector finds the regions, and the sampler samples each again at
// its own resolution. `detector` is a name from the vocabulary that only a server with the detailer nodes reports.
struct DetailRecipe {
    SamplingRecipe sampling;
    PrStringView sourceImage;
    PrStringView detector;
};

// Return the graph of a recipe. Node ids name the stages ("checkpoint", "sampler", "decode"), so that a progress event
// names the stage that it reached. An empty negative prompt zeroes the positive conditioning rather than encoding empty
// text.
String txt2ImgGraph(const Txt2ImgRecipe& recipe);
String upscaleGraph(const UpscaleRecipe& recipe);
String outpaintGraph(const OutpaintRecipe& recipe);
String detailGraph(const DetailRecipe& recipe);

// Returns the direction's name as commands and records spell it.
PrStringView directionName(OutpaintDirection direction);

// Returns the direction that `name` names, or `fallback` if it names none.
OutpaintDirection parseDirection(PrStringView name, OutpaintDirection fallback);

// Return the size that an outpaint produces: the source grown by `amount` along the direction's axis.
uint32_t outpaintWidth(const OutpaintRecipe& recipe);
uint32_t outpaintHeight(const OutpaintRecipe& recipe);

// Returns the enlargement that an upscale model's name states, such as 4 for "RealESRGAN_x4plus", or 0 if it states
// none.
double upscaleModelFactor(PrStringView model);

} // namespace alchemy

PR_ASSUME_NONNULL_END

// Dimensions: the size of a run, chosen from the ladder of sizes that its checkpoint samples well at.
//
// A checkpoint samples well only near the sizes that it was trained at, so the size control is a ladder rather than a
// free pair of numbers. The ladders are the SDXL buckets, the 512-edge sizes of SD 1.5, and a budget of megapixels at
// an aspect for the architectures that take any size. The ladder follows from the checkpoint: its own header's
// architecture once it has been read, and its filename until then. A size override suspends the ladder and runs at two
// numbers.

#pragma once

#include "pr/base.h"
#include "pr/string.h"
#include "pr/string_view.h"

#include "examples/alchemy/models.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

using namespace pr;

// The ladder that a run's size comes from.
enum {
    DimensionFamily_Sdxl,
    DimensionFamily_Sd15,
    DimensionFamily_Flexible,
};
typedef uint8_t DimensionFamily;

// A run's width and height in pixels.
struct DimensionSize {
    uint32_t width = 0;
    uint32_t height = 0;
};

// The size of a run, and the selection on each ladder that it was resolved from. Each ladder keeps its own selection,
// so a checkpoint that moves the family away and back returns to what was chosen there.
struct Dimensions {
    DimensionSize size = {1024, 1024};
    DimensionFamily family = DimensionFamily_Sdxl;
    uint32_t sdxlPreset;
    uint32_t sd15Preset;
    uint32_t megapixel = 1;

    // The flexible ladder's aspect as log2 of width over height, which puts its two ends an equal distance from square.
    double aspectExponent = 0.0;

    bool customSize = false;    // the ladder stands aside, and the run uses `size` as it is

    // The checkpoint that the family was last chosen for, and whether its file's header chose it rather than its name.
    // A family that a caller chose stands while the checkpoint stays selected.
    String checkpointSeed;
    bool detected = false;

    Dimensions();
};

// Return the ladders: the SDXL buckets and the SD 1.5 sizes, in ascending aspect, and the flexible ladder's budgets in
// megapixels.
const DimensionSize* sdxlBuckets(uint32_t* count);
const DimensionSize* sd15Sizes(uint32_t* count);
const double* megapixelLadder(uint32_t* count);

// Returns the size of a ladder's selection, or the override's size.
DimensionSize resolvedSize(const Dimensions& dims);

// Chooses the family that the checkpoint `checkpoint` samples at, from its architecture if `arch` is known and from its
// name otherwise, unless the family was already chosen for that checkpoint. A detected architecture replaces a family
// that the name chose. Carries the size onto the new family's ladder.
void seedDimensions(Dimensions& dims, PrStringView checkpoint, ModelArchitecture arch);

// Moves to `family`, carrying the current size onto its ladder so that the picture keeps its shape.
void setDimensionFamily(Dimensions& dims, DimensionFamily family);

// Sets the size override. Leaving it carries the override's size onto the ladder.
void setCustomSize(Dimensions& dims, bool custom);

// Moves the selection to the ladder's entry at `preset`, or the flexible ladder to `megapixel` and `aspectExponent`,
// and resolves the size.
void selectPreset(Dimensions& dims, uint32_t preset);
void selectFlexible(Dimensions& dims, uint32_t megapixel, double aspectExponent);

// Flips the selection's orientation.
void swapDimensions(Dimensions& dims);

// Puts the ladder on `size` if a family states it exactly, trying the current family first, so that a recorded size
// lands on the ladder that it came from. Returns false if no ladder reaches it.
bool adoptExactSize(Dimensions& dims, DimensionSize size);

// Fits `shape`'s aspect onto the current family's sizes, keeping the pixels that the family spends: the nearest entry,
// the flexible ladder's aspect, or the override's area at the new aspect. Returns the new size.
DimensionSize applyAspectRatio(Dimensions& dims, DimensionSize shape);

// Return the family a model architecture samples at, and the family that a checkpoint's filename reads as.
DimensionFamily familyForArchitecture(ModelArchitecture arch);
DimensionFamily familyForCheckpoint(PrStringView filename, DimensionFamily fallback);

// Return the family's name as commands and records spell it, the family that a name names or `fallback`, and the
// family's name as the panel shows it.
PrStringView familyName(DimensionFamily family);
DimensionFamily parseFamily(PrStringView name, DimensionFamily fallback);
PrStringView familyLabel(DimensionFamily family);

// Return the size as a report names it, such as "1024 x 1024 (1:1)", and its two halves as the panel shows them.
String dimensionLabel(DimensionSize size);
String dimensionSizeLabel(DimensionSize size);
String dimensionRatioLabel(DimensionSize size);

// Return the aspect and the megapixels that a size states.
double sizeAspect(DimensionSize size);
double sizeMegapixels(DimensionSize size);

// Returns the size that a budget of `megapixels` at `aspect` resolves to, in steps of 64.
DimensionSize flexibleSize(double megapixels, double aspect);

// Return the range of the flexible ladder's aspect exponent.
double flexibleAspectMin();
double flexibleAspectMax();

} // namespace alchemy

PR_ASSUME_NONNULL_END

// Dimensions: the size of a run, from its checkpoint's ladder.

#include "examples/alchemy/dimensions.h"

#include "pr/ascii.h"
#include "pr/count_of.h"

#include "examples/alchemy/text.h"

#include <algorithm>
#include <cmath>

namespace alchemy {

using namespace pr;

namespace {

// The SDXL buckets: the sizes that the base model was trained at (Podell et al., "SDXL: Improving Latent Diffusion
// Models for High-Resolution Image Synthesis", appendix I), in ascending aspect. Each is a multiple of 64 near a
// megapixel, which keeps a run inside the distribution that the model learned.
constexpr DimensionSize kSdxlBuckets[] = {
    {512, 2048}, {512, 1984}, {512, 1920}, {512, 1856}, {576, 1792}, {576, 1728}, {576, 1664},
    {640, 1600}, {640, 1536}, {704, 1472}, {704, 1408}, {704, 1344}, {768, 1344}, {768, 1280},
    {832, 1216}, {832, 1152}, {896, 1152}, {896, 1088}, {960, 1088}, {960, 1024}, {1024, 1024},
    {1024, 960}, {1088, 960}, {1088, 896}, {1152, 896}, {1152, 832}, {1216, 832}, {1280, 768},
    {1344, 768}, {1344, 704}, {1408, 704}, {1472, 704}, {1536, 640}, {1600, 640}, {1664, 576},
    {1728, 576}, {1792, 576}, {1856, 512}, {1920, 512}, {1984, 512}, {2048, 512}};

// SD 1.5 samples at 512 on one edge, and the other edge steps up to a 16:9 frame.
constexpr DimensionSize kSd15Sizes[] = {{512, 912}, {512, 832}, {512, 768}, {512, 704}, {512, 640}, {512, 512},
                                    {640, 512}, {704, 512}, {768, 512}, {832, 512}, {912, 512}};

constexpr double kMegapixels[] = {0.5, 1.0, 1.5, 2.0};

constexpr uint32_t kSdxlCount = uint32_t(countOf(kSdxlBuckets));
constexpr uint32_t kSd15Count = uint32_t(countOf(kSd15Sizes));
constexpr uint32_t kMegapixelCount = uint32_t(countOf(kMegapixels));

// The square entries, where a ladder starts.
constexpr uint32_t kSdxlSquare = 20;
constexpr uint32_t kSd15Square = 5;

constexpr uint32_t kSizeStep = 64;
constexpr uint32_t kMinEdge = 256;
constexpr uint32_t kMaxEdge = 2048;

// The narrowest and widest frames of the flexible ladder, 1:4 and 4:1, as log2 of the aspect.
constexpr double kMinAspectExponent = -2.0;
constexpr double kMaxAspectExponent = 2.0;

uint32_t snapEdge(double value) {
    double steps = std::max(std::round(value / kSizeStep), 1.0);
    return std::clamp(uint32_t(steps) * kSizeStep, kMinEdge, kMaxEdge);
}

double areaOf(DimensionSize size) {
    return double(size.width) * double(size.height);
}

// Returns the entry of `sizes` closest to `aspect`, and among equals the one closest to `tieBreak` in area.
uint32_t nearestAspect(const DimensionSize* sizes, uint32_t count, double aspect, DimensionSize tieBreak) {
    double area = areaOf(tieBreak);
    uint32_t nearest = 0;
    double bestAspect = HUGE_VAL;
    double bestArea = HUGE_VAL;
    for (uint32_t i = 0; i < count; ++i) {
        double aspectDelta = std::fabs(sizeAspect(sizes[i]) - aspect);
        double areaDelta = std::fabs(areaOf(sizes[i]) - area);
        if (aspectDelta > bestAspect || (aspectDelta == bestAspect && areaDelta >= bestArea))
            continue;
        nearest = i;
        bestAspect = aspectDelta;
        bestArea = areaDelta;
    }
    return nearest;
}

uint32_t nearestSize(const DimensionSize* sizes, uint32_t count, DimensionSize size) {
    return nearestAspect(sizes, count, sizeAspect(size), size);
}

uint32_t exactSize(const DimensionSize* sizes, uint32_t count, DimensionSize size) {
    for (uint32_t i = 0; i < count; ++i) {
        if (sizes[i].width == size.width && sizes[i].height == size.height)
            return i;
    }
    return count;
}

uint32_t nearestMegapixel(double megapixels) {
    uint32_t nearest = 0;
    for (uint32_t i = 1; i < kMegapixelCount; ++i) {
        if (std::fabs(kMegapixels[i] - megapixels) < std::fabs(kMegapixels[nearest] - megapixels))
            nearest = i;
    }
    return nearest;
}

double flexibleAspectExponent(double aspect) {
    return std::clamp(std::log2(aspect), kMinAspectExponent, kMaxAspectExponent);
}

// Moves the current family's selection to the entry nearest the current size, so that the picture keeps its shape when
// the family changes.
void adoptCurrentSize(Dimensions& dims) {
    switch (dims.family) {
        case DimensionFamily_Sdxl: dims.sdxlPreset = nearestSize(kSdxlBuckets, kSdxlCount, dims.size); break;
        case DimensionFamily_Sd15: dims.sd15Preset = nearestSize(kSd15Sizes, kSd15Count, dims.size); break;
        case DimensionFamily_Flexible:
            dims.aspectExponent = flexibleAspectExponent(sizeAspect(dims.size));
            dims.megapixel = nearestMegapixel(sizeMegapixels(dims.size));
            break;
    }
}

void resolve(Dimensions& dims) {
    dims.size = resolvedSize(dims);
}

// The selection that `family` needs to state `size` exactly.
struct LadderPick {
    uint32_t preset = 0;
    uint32_t megapixel = 0;
    double aspectExponent = 0.0;
};

bool ladderStatesSize(DimensionFamily family, DimensionSize size, LadderPick* pick) {
    switch (family) {
        case DimensionFamily_Sdxl:
            pick->preset = exactSize(kSdxlBuckets, kSdxlCount, size);
            return pick->preset < kSdxlCount;
        case DimensionFamily_Sd15:
            pick->preset = exactSize(kSd15Sizes, kSd15Count, size);
            return pick->preset < kSd15Count;
        case DimensionFamily_Flexible: {
            pick->megapixel = nearestMegapixel(sizeMegapixels(size));
            pick->aspectExponent = flexibleAspectExponent(sizeAspect(size));
            DimensionSize fitted = flexibleSize(kMegapixels[pick->megapixel], std::exp2(pick->aspectExponent));
            return fitted.width == size.width && fitted.height == size.height;
        }
    }
    return false;
}

uint32_t greatestCommonDivisor(uint32_t a, uint32_t b) {
    while (b != 0) {
        uint32_t rest = a % b;
        a = b;
        b = rest;
    }
    return a;
}

DimensionSize ratioOf(DimensionSize size) {
    uint32_t divisor = std::max(greatestCommonDivisor(size.width, size.height), 1u);
    return {size.width / divisor, size.height / divisor};
}

} // namespace

Dimensions::Dimensions() : sdxlPreset(kSdxlSquare), sd15Preset(kSd15Square) {}

const DimensionSize* sdxlBuckets(uint32_t* count) {
    *count = kSdxlCount;
    return kSdxlBuckets;
}

const DimensionSize* sd15Sizes(uint32_t* count) {
    *count = kSd15Count;
    return kSd15Sizes;
}

const double* megapixelLadder(uint32_t* count) {
    *count = kMegapixelCount;
    return kMegapixels;
}

DimensionSize resolvedSize(const Dimensions& dims) {
    if (dims.customSize)
        return dims.size;
    switch (dims.family) {
        case DimensionFamily_Sdxl: return kSdxlBuckets[std::min(dims.sdxlPreset, kSdxlCount - 1)];
        case DimensionFamily_Sd15: return kSd15Sizes[std::min(dims.sd15Preset, kSd15Count - 1)];
        case DimensionFamily_Flexible:
            return flexibleSize(kMegapixels[std::min(dims.megapixel, kMegapixelCount - 1)],
                                std::exp2(std::clamp(dims.aspectExponent, kMinAspectExponent, kMaxAspectExponent)));
    }
    return dims.size;
}

void seedDimensions(Dimensions& dims, PrStringView checkpoint, ModelArchitecture arch) {
    if (checkpoint.size == 0)
        return;
    bool detected = arch != ModelArchitecture_Unknown;
    // The family is chosen again once the checkpoint's own file answers for what its name had been read as.
    if (checkpoint == dims.checkpointSeed && (dims.detected || !detected))
        return;
    dims.checkpointSeed = stringOf(checkpoint);
    setDimensionFamily(dims, detected ? familyForArchitecture(arch) : familyForCheckpoint(checkpoint, dims.family));
    dims.detected = detected;
}

void setDimensionFamily(Dimensions& dims, DimensionFamily family) {
    if (dims.family == family)
        return;
    dims.family = family;
    // A family that something other than the file chose no longer reads as the file's.
    dims.detected = false;
    adoptCurrentSize(dims);
    resolve(dims);
}

void setCustomSize(Dimensions& dims, bool custom) {
    if (dims.customSize == custom)
        return;
    dims.customSize = custom;
    if (!custom)
        adoptCurrentSize(dims);
    resolve(dims);
}

void selectPreset(Dimensions& dims, uint32_t preset) {
    if (dims.family == DimensionFamily_Sdxl)
        dims.sdxlPreset = std::min(preset, kSdxlCount - 1);
    else if (dims.family == DimensionFamily_Sd15)
        dims.sd15Preset = std::min(preset, kSd15Count - 1);
    resolve(dims);
}

void selectFlexible(Dimensions& dims, uint32_t megapixel, double aspectExponent) {
    dims.megapixel = std::min(megapixel, kMegapixelCount - 1);
    dims.aspectExponent = std::clamp(aspectExponent, kMinAspectExponent, kMaxAspectExponent);
    resolve(dims);
}

void swapDimensions(Dimensions& dims) {
    DimensionSize flipped{dims.size.height, dims.size.width};
    if (dims.customSize) {
        dims.size = flipped;
        return;
    }
    switch (dims.family) {
        case DimensionFamily_Sdxl: dims.sdxlPreset = nearestSize(kSdxlBuckets, kSdxlCount, flipped); break;
        case DimensionFamily_Sd15: dims.sd15Preset = nearestSize(kSd15Sizes, kSd15Count, flipped); break;
        case DimensionFamily_Flexible:
            dims.aspectExponent = std::clamp(-dims.aspectExponent, kMinAspectExponent, kMaxAspectExponent);
            break;
    }
    resolve(dims);
}

bool adoptExactSize(Dimensions& dims, DimensionSize size) {
    const DimensionFamily families[] = {dims.family, DimensionFamily_Sdxl, DimensionFamily_Sd15,
                                        DimensionFamily_Flexible};
    for (DimensionFamily family : families) {
        LadderPick pick;
        if (!ladderStatesSize(family, size, &pick))
            continue;
        setDimensionFamily(dims, family);
        switch (family) {
            case DimensionFamily_Sdxl: dims.sdxlPreset = pick.preset; break;
            case DimensionFamily_Sd15: dims.sd15Preset = pick.preset; break;
            case DimensionFamily_Flexible:
                dims.megapixel = pick.megapixel;
                dims.aspectExponent = pick.aspectExponent;
                break;
        }
        dims.customSize = false;
        resolve(dims);
        return true;
    }
    return false;
}

DimensionSize applyAspectRatio(Dimensions& dims, DimensionSize shape) {
    double aspect = sizeAspect(shape);
    if (dims.customSize) {
        dims.size = flexibleSize(sizeMegapixels(dims.size), aspect);
        return dims.size;
    }
    switch (dims.family) {
        case DimensionFamily_Sdxl: dims.sdxlPreset = nearestAspect(kSdxlBuckets, kSdxlCount, aspect, dims.size); break;
        case DimensionFamily_Sd15: dims.sd15Preset = nearestAspect(kSd15Sizes, kSd15Count, aspect, dims.size); break;
        case DimensionFamily_Flexible: dims.aspectExponent = flexibleAspectExponent(aspect); break;
    }
    resolve(dims);
    return dims.size;
}

DimensionFamily familyForArchitecture(ModelArchitecture arch) {
    switch (arch) {
        case ModelArchitecture_Sd15: return DimensionFamily_Sd15;
        case ModelArchitecture_Sd3:
        case ModelArchitecture_Flux:
        case ModelArchitecture_Cascade: return DimensionFamily_Flexible;
        default: return DimensionFamily_Sdxl;
    }
}

DimensionFamily familyForCheckpoint(PrStringView filename, DimensionFamily fallback) {
    if (asciiContainsFold(filename, "xl"))
        return DimensionFamily_Sdxl;
    for (PrStringView marker : {"sd15", "sd-1-5", "sd_1_5", "v1-5", "1.5"}) {
        if (asciiContainsFold(filename, marker))
            return DimensionFamily_Sd15;
    }
    for (PrStringView marker : {"flux", "sd3", "cascade"}) {
        if (asciiContainsFold(filename, marker))
            return DimensionFamily_Flexible;
    }
    return fallback;
}

PrStringView familyName(DimensionFamily family) {
    switch (family) {
        case DimensionFamily_Sdxl: return "sdxl";
        case DimensionFamily_Sd15: return "sd15";
        case DimensionFamily_Flexible: return "flexible";
    }
    return "sdxl";
}

DimensionFamily parseFamily(PrStringView name, DimensionFamily fallback) {
    for (DimensionFamily family : {DimensionFamily_Sdxl, DimensionFamily_Sd15, DimensionFamily_Flexible}) {
        if (asciiContainsFold(name, familyName(family)))
            return family;
    }
    return fallback;
}

PrStringView familyLabel(DimensionFamily family) {
    switch (family) {
        case DimensionFamily_Sdxl: return "SDXL";
        case DimensionFamily_Sd15: return "SD 1.5";
        case DimensionFamily_Flexible: return "FLEXIBLE";
    }
    return "SDXL";
}

std::string dimensionLabel(DimensionSize size) {
    return dimensionSizeLabel(size) + " (" + dimensionRatioLabel(size) + ")";
}

std::string dimensionSizeLabel(DimensionSize size) {
    return std::to_string(size.width) + " x " + std::to_string(size.height);
}

std::string dimensionRatioLabel(DimensionSize size) {
    DimensionSize ratio = ratioOf(size);
    return std::to_string(ratio.width) + ":" + std::to_string(ratio.height);
}

double sizeAspect(DimensionSize size) {
    return size.height > 0 ? double(size.width) / double(size.height) : 1.0;
}

double sizeMegapixels(DimensionSize size) {
    return areaOf(size) / 1.0e6;
}

DimensionSize flexibleSize(double megapixels, double aspect) {
    double pixels = std::max(megapixels, 0.1) * 1.0e6;
    double ratio = std::clamp(aspect, std::exp2(kMinAspectExponent), std::exp2(kMaxAspectExponent));
    return {snapEdge(std::sqrt(pixels * ratio)), snapEdge(std::sqrt(pixels / ratio))};
}

double flexibleAspectMin() {
    return kMinAspectExponent;
}

double flexibleAspectMax() {
    return kMaxAspectExponent;
}

} // namespace alchemy

#include "image/src/convert.h"

#include "pr/array.h"
#include "pr/assert.h"
#include "pr/color_profile.h"
#include "pr/color_space.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/mem.h"

namespace pr_impl {
namespace {

uint32_t colorChannels(PrColorModel model) {
    switch (model) {
        case PrColorModel_Gray:
            return 1;
        case PrColorModel_Rgb:
        case PrColorModel_Xyz:
        case PrColorModel_Lab:
        case PrColorModel_Lch:
        case PrColorModel_Hsl:
        case PrColorModel_Hwb:
            return 3;
        case PrColorModel_Cmyk:
            return 4;
        default:
            return 0;
    }
}

uint32_t sampleBytes(PrImageSample sample) {
    switch (sample) {
        case PrImageSample_U8:
            return 1;
        case PrImageSample_U16:
        case PrImageSample_I16:
        case PrImageSample_F16:
            return 2;
        case PrImageSample_F32:
            return 4;
        default:
            return 0;
    }
}

bool validColor(const PrImageFormat& format) {
    const PrImageColor& color = format.color;
    switch (color.encoding) {
        case PrImageColorEncoding_Unknown:
            return true;
        case PrImageColorEncoding_Icc:
            return color.profile.data != nullptr && color.profile.size != 0;
        case PrImageColorEncoding_Named: {
            const PrColorModel model = prColorSpaceModel(color.space);
            return model != PrColorModel_Count && (format.model == model ||
                (format.model == PrColorModel_Gray && model == PrColorModel_Rgb));
        }
        default:
            return false;
    }
}

uint32_t pixelBytes(const PrImageFormat& format) {
    const uint32_t colors = colorChannels(format.model);
    const uint32_t sample = sampleBytes(format.sample);
    if (colors == 0 || sample == 0 || format.alpha > PrImageAlpha_Independent || !validColor(format)) {
        return 0;
    }
    return (colors + (format.alpha != PrImageAlpha_None ? 1u : 0u)) * sample;
}

float floatFromBits(uint32_t bits) {
    float value = 0;
    prMemCopy(&value, &bits, sizeof(value));
    return value;
}

double readSample(const uint8_t* source, PrImageSample sample) {
    if (sample == PrImageSample_U8) {
        return *source / 255.0;
    }
    if (sample == PrImageSample_F32) {
        float value = 0;
        prMemCopy(&value, source, sizeof(value));
        return double(value);
    }
    uint16_t value = 0;
    prMemCopy(&value, source, sizeof(value));
    if (sample == PrImageSample_U16) {
        return value / 65535.0;
    }
    if (sample == PrImageSample_I16) {
        int16_t signedValue = 0;
        prMemCopy(&signedValue, &value, sizeof(value));
        return signedValue == -32768 ? -1.0 : signedValue / 32767.0;
    }
    PrFloat16 stored{};
    prMemCopy(&stored, source, sizeof(stored));
    return double(prFloat16ToFloat(stored));
}

double clampUnit(double value) {
    if (!(value > 0)) {
        return 0;
    }
    return value >= 1 ? 1 : value;
}

void writeSample(uint8_t* out, PrImageSample sample, double value) {
    if (sample == PrImageSample_U8) {
        *out = uint8_t(clampUnit(value) * 255.0 + 0.5);
        return;
    }
    if (sample == PrImageSample_U16) {
        const uint16_t converted = uint16_t(clampUnit(value) * 65535.0 + 0.5);
        prMemCopy(out, &converted, sizeof(converted));
        return;
    }
    if (sample == PrImageSample_I16) {
        if (!(value == value)) {
            value = 0;
        }
        value = value < -1 ? -1 : (value > 1 ? 1 : value);
        const int16_t converted = int16_t(value * 32767.0 + (value < 0 ? -0.5 : 0.5));
        prMemCopy(out, &converted, sizeof(converted));
        return;
    }
    float converted = 0;
    if (value > double(kPrF32Max)) {
        converted = floatFromBits(0x7F800000u);
    } else if (value < -double(kPrF32Max)) {
        converted = floatFromBits(0xFF800000u);
    } else {
        converted = float(value);
    }
    if (sample == PrImageSample_F32) {
        prMemCopy(out, &converted, sizeof(converted));
    } else {
        const PrFloat16 narrow = prFloat16FromFloat(converted);
        prMemCopy(out, &narrow, sizeof(narrow));
    }
}

// Converts a row of eight-bit pixels between alpha forms, and from gray to RGB.
void convertEightBit(const uint8_t* input, const PrImageFormat& from, uint8_t* output, const PrImageFormat& to, uint32_t width) {
    const uint32_t sourceChannels = colorChannels(from.model);
    const uint32_t destinationChannels = colorChannels(to.model);
    const uint32_t sourcePixel = pixelBytes(from);
    const uint32_t destinationPixel = pixelBytes(to);
    const bool fromPremultiplied = from.alpha == PrImageAlpha_Premultiplied;
    const bool toPremultiplied = to.alpha == PrImageAlpha_Premultiplied;
    // Opaque pixels keep their channels in either alpha form. The common forms have loops of their own, with the
    // decisions made outside the loops.
    const bool addAlpha = to.alpha != PrImageAlpha_None;
    if (from.alpha == PrImageAlpha_None && sourceChannels == 3 && destinationChannels == 3 && addAlpha) {
        for (uint32_t x = 0; x < width; ++x) {
            output[x * 4] = input[x * 3];
            output[x * 4 + 1] = input[x * 3 + 1];
            output[x * 4 + 2] = input[x * 3 + 2];
            output[x * 4 + 3] = 255;
        }
        return;
    }
    if (from.alpha == PrImageAlpha_None && sourceChannels == 1 && destinationChannels == 3 && addAlpha) {
        for (uint32_t x = 0; x < width; ++x) {
            output[x * 4] = output[x * 4 + 1] = output[x * 4 + 2] = input[x];
            output[x * 4 + 3] = 255;
        }
        return;
    }
    if (from.alpha == PrImageAlpha_None && (sourceChannels == destinationChannels || sourceChannels == 1)) {
        for (uint32_t x = 0; x < width; ++x, input += sourcePixel, output += destinationPixel) {
            for (uint32_t c = 0; c < destinationChannels; ++c)
                output[c] = input[sourceChannels == 1 ? 0 : c];
            if (addAlpha)
                output[destinationChannels] = 255;
        }
        return;
    }
    for (uint32_t x = 0; x < width; ++x, input += sourcePixel, output += destinationPixel) {
        const uint32_t alpha = from.alpha == PrImageAlpha_None ? 255 : input[sourceChannels];
        uint32_t channels[4] = {input[0], 0, 0, 0};
        for (uint32_t c = 1; c < sourceChannels; ++c)
            channels[c] = input[c];
        if (sourceChannels == 1 && destinationChannels == 3)
            channels[1] = channels[2] = channels[0];
        for (uint32_t c = 0; c < destinationChannels; ++c) {
            uint32_t value = channels[c];
            if (fromPremultiplied && !toPremultiplied) {
                value = alpha == 0 ? 0 : (2 * value * 255 + alpha) / (2 * alpha);
                value = value < 255 ? value : 255;
            } else if (toPremultiplied && !fromPremultiplied) {
                value = (2 * value * alpha + 255) / 510;
            } else if (fromPremultiplied && alpha == 0) {
                value = 0;
            }
            output[c] = uint8_t(value);
        }
        if (to.alpha != PrImageAlpha_None)
            output[destinationChannels] = uint8_t(alpha);
    }
}

// The grid points along each input of the table that a profile's conversion is sampled into, by the number of the
// profile's channels. More channels take fewer points each, so that a table stays small.
constexpr uint32_t kProfileGridPoints[kPrColorProfileMaxChannels + 1] = {0, 256, 65, 33, 17, 9, 7, 5, 4};

// A device color of a profile converted to a named space: through the profile directly, or by interpolating a table of
// the conversion sampled on a grid.
struct ProfileConversion {
    const PrColorProfile* profile;
    PrColorSpace space;
    uint32_t channels;
    uint32_t points;            // grid points along each input, or 0 to convert directly
    Array<float> table;         // three channels for each grid point, the first input varying slowest

    void direct(const double* in, double out[3]) const {
        double xyz[3];
        prColorProfileToXyzD50(profile, PrColorIntent_Perceptual, in, xyz);
        prColorConvert(PrColorSpace_XyzD50, space, xyz, out);
    }

    // Samples the conversion at every grid point, if the image has more pixels than the grid has points.
    void prepare(uint64_t pixels) {
        uint32_t grid = kProfileGridPoints[channels];
        uint64_t total = 1;
        for (uint32_t i = 0; i < channels; ++i)
            total *= grid;
        if (pixels <= total)
            return;
        points = grid;
        table.resize(uint32_t(total) * 3);
        double in[kPrColorProfileMaxChannels];
        for (uint32_t index = 0; index < uint32_t(total); ++index) {
            uint32_t rest = index;
            for (uint32_t i = channels; i-- > 0;) {
                in[i] = double(rest % grid) / double(grid - 1);
                rest /= grid;
            }
            double out[3];
            direct(in, out);
            for (uint32_t c = 0; c < 3; ++c)
                table.data[index * 3 + c] = float(out[c]);
        }
    }

    // Converts `in`, interpolating the table across the corners of its cell if there is one.
    void convert(const double* in, double out[3]) const {
        if (points == 0) {
            direct(in, out);
            return;
        }
        uint32_t base[kPrColorProfileMaxChannels];
        double fraction[kPrColorProfileMaxChannels];
        uint32_t stride[kPrColorProfileMaxChannels];
        uint32_t step = 3;
        for (uint32_t i = channels; i-- > 0;) {
            stride[i] = step;
            step *= points;
        }
        for (uint32_t i = 0; i < channels; ++i) {
            double value = in[i] < 0 ? 0 : in[i] > 1 ? 1 : in[i];
            double position = value * double(points - 1);
            base[i] = uint32_t(position);
            if (base[i] >= points - 1)
                base[i] = points - 2;
            fraction[i] = position - double(base[i]);
        }
        out[0] = out[1] = out[2] = 0;
        for (uint32_t corner = 0; corner < (1u << channels); ++corner) {
            double weight = 1;
            uint32_t at = 0;
            for (uint32_t i = 0; i < channels; ++i) {
                bool high = (corner >> i & 1) != 0;
                weight *= high ? fraction[i] : 1 - fraction[i];
                at += (base[i] + (high ? 1 : 0)) * stride[i];
            }
            if (weight == 0)
                continue;
            for (uint32_t c = 0; c < 3; ++c)
                out[c] += weight * double(table.data[at + c]);
        }
    }
};

// Converts pixels whose color `profile` describes to `to`, a named space of three channels, by its default rendering
// intent. An image of more pixels than the table has points converts through the table.
PrStatus convertProfile(PrPixelView source, const PrImageFormat& from, const PrColorProfile* profile,
        PrPixels destination, const PrImageFormat& to) {
    const uint32_t sourceChannels = colorChannels(from.model);
    const uint32_t sourceSample = sampleBytes(from.sample);
    const uint32_t destinationSample = sampleBytes(to.sample);
    ProfileConversion conversion{.profile = profile, .space = to.color.space, .channels = sourceChannels};
    conversion.prepare(uint64_t(source.extent.x) * source.extent.y);
    for (uint32_t y = 0; y < source.extent.y; ++y) {
        const uint8_t* input = source.data + size_t(y) * source.stride;
        uint8_t* output = destination.data + size_t(y) * destination.stride;
        for (uint32_t x = 0; x < source.extent.x; ++x) {
            double channels[kPrColorProfileMaxChannels] = {};
            for (uint32_t c = 0; c < sourceChannels; ++c)
                channels[c] = readSample(input + c * sourceSample, from.sample);
            double alpha = 1.0;
            if (from.alpha != PrImageAlpha_None)
                alpha = readSample(input + sourceChannels * sourceSample, from.sample);
            if (from.alpha == PrImageAlpha_Premultiplied) {
                for (uint32_t c = 0; c < sourceChannels; ++c)
                    channels[c] = alpha != 0 ? channels[c] / alpha : 0;
            }
            double converted[3];
            conversion.convert(channels, converted);
            for (uint32_t c = 0; c < 3; ++c) {
                double value = to.alpha == PrImageAlpha_Premultiplied ? converted[c] * alpha : converted[c];
                writeSample(output + c * destinationSample, to.sample, value);
            }
            if (to.alpha != PrImageAlpha_None)
                writeSample(output + 3 * destinationSample, to.sample, alpha);
            input += pixelBytes(from);
            output += pixelBytes(to);
        }
    }
    conversion.table.reset();
    return PrStatus_Ok;
}

bool colorEquals(const PrImageColor& a, const PrImageColor& b) {
    if (a.encoding != b.encoding) {
        return false;
    }
    if (a.encoding == PrImageColorEncoding_Named) {
        return a.space == b.space;
    }
    if (a.encoding == PrImageColorEncoding_Icc) {
        return a.profile.size == b.profile.size && prMemEquals(a.profile.data, b.profile.data, a.profile.size);
    }
    return true;
}

} // namespace

uint32_t imagePixelBytes(const PrImageFormat& format) {
    return pixelBytes(format);
}

PrStatus imageConvert(PrPixelView source, const PrImageFormat& from, PrPixels destination,
        const PrImageFormat& to, PrImageConversionFlags policy) {
    PR_ASSERT(source.extent.x == destination.extent.x && source.extent.y == destination.extent.y);
    PR_ASSERT(pixelBytes(from) != 0 && pixelBytes(to) != 0);
    PR_ASSERT(size_t(source.extent.x) * pixelBytes(from) <= source.stride);
    PR_ASSERT(size_t(destination.extent.x) * pixelBytes(to) <= destination.stride);
    if (source.extent.x == 0 || source.extent.y == 0)
        return PrStatus_Ok;
    const uint8_t* sourceEnd = source.data + source.stride * (source.extent.y - 1) + source.extent.x * pixelBytes(from);
    const uint8_t* destinationEnd =
        destination.data + destination.stride * (destination.extent.y - 1) + destination.extent.x * pixelBytes(to);
    PR_ASSERT(destinationEnd <= source.data || sourceEnd <= destination.data);
    if ((from.alpha == PrImageAlpha_Independent) != (to.alpha == PrImageAlpha_Independent)) {
        return PrStatus_NotSupported;
    }
    const bool equalColor = colorEquals(from.color, to.color);
    const bool equalModel = from.model == to.model;
    const bool replicateGray = from.model == PrColorModel_Gray && to.model == PrColorModel_Rgb;
    const bool cmyk = from.model == PrColorModel_Cmyk && to.model == PrColorModel_Rgb;
    const bool namedModels = colorChannels(from.model) == 3 && colorChannels(to.model) == 3 &&
        from.color.encoding == PrImageColorEncoding_Named && to.color.encoding == PrImageColorEncoding_Named;
    // A source that an ICC profile describes converts through the profile to a named space, whatever its model. A
    // profile that the engine does not read is not supported.
    PrColorProfile* profile = nullptr;
    if (from.color.encoding == PrImageColorEncoding_Icc && to.color.encoding == PrImageColorEncoding_Named &&
            !equalColor && colorChannels(to.model) == 3 &&
            prColorProfileCreate(from.color.profile, &profile) == PrStatus_Ok &&
            prColorProfileChannels(profile) != colorChannels(from.model)) {
        prColorProfileDestroy(profile);
        profile = nullptr;
    }
    // A profile of a named space converts as that space does.
    PrColorSpace named = profile != nullptr ? prColorProfileNamedSpace(profile) : PrColorSpace_Count;
    if (named != PrColorSpace_Count) {
        prColorProfileDestroy(profile);
        PrImageFormat format = from;
        format.color = PrImageColor{.encoding = PrImageColorEncoding_Named, .space = named};
        return imageConvert(source, format, destination, to, policy);
    }
    if (profile != nullptr) {
        PrStatus status = convertProfile(source, from, profile, destination, to);
        prColorProfileDestroy(profile);
        return status;
    }
    if (!equalModel && !replicateGray && !cmyk && !namedModels) {
        return PrStatus_NotSupported;
    }
    if (cmyk && ((policy & PrImageConversionFlag_ApproximateCmyk) == 0)) {
        return PrStatus_NotSupported;
    }
    if ((from.color.encoding == PrImageColorEncoding_Icc || to.color.encoding == PrImageColorEncoding_Icc) &&
            (!equalColor || !equalModel)) {
        return PrStatus_NotSupported;
    }
    const bool changeSpace = !equalColor || (!equalModel && !replicateGray);
    PrColorSpace fromSpace = from.color.space;
    PrColorSpace toSpace = to.color.space;
    if (changeSpace) {
        if (from.color.encoding == PrImageColorEncoding_Unknown && !cmyk) {
            if ((policy & PrImageConversionFlag_AssumeSrgb) == 0 ||
                    (from.model != PrColorModel_Rgb && from.model != PrColorModel_Gray)) {
                return PrStatus_NotSupported;
            }
            fromSpace = PrColorSpace_Srgb;
        }
        if (to.color.encoding == PrImageColorEncoding_Unknown) {
            if ((policy & PrImageConversionFlag_AssumeSrgb) == 0 ||
                    (to.model != PrColorModel_Rgb && to.model != PrColorModel_Gray)) {
                return PrStatus_NotSupported;
            }
            toSpace = PrColorSpace_Srgb;
        }
        if (cmyk) {
            fromSpace = PrColorSpace_Srgb;
        } else if (from.model == PrColorModel_Cmyk) {
            return PrStatus_NotSupported;
        }
    }
    const uint32_t sourceChannels = colorChannels(from.model);
    const uint32_t destinationChannels = colorChannels(to.model);
    const uint32_t sourceSample = sampleBytes(from.sample);
    const uint32_t destinationSample = sampleBytes(to.sample);
    const uint32_t sourcePixel = pixelBytes(from);
    const uint32_t destinationPixel = pixelBytes(to);
    const bool identical = equalModel && equalColor && from.sample == to.sample && from.alpha == to.alpha;
    // Converting eight bits to eight bits in one space needs no floating point. Premultiplying and unpremultiplying
    // round the exact quotient half up.
    const bool eightBit = from.sample == PrImageSample_U8 && to.sample == PrImageSample_U8 && !cmyk &&
        !(changeSpace && fromSpace != toSpace);
    for (uint32_t y = 0; y < source.extent.y; ++y) {
        const uint8_t* input = source.data + size_t(y) * source.stride;
        uint8_t* output = destination.data + size_t(y) * destination.stride;
        if (identical) {
            prMemCopy(output, input, size_t(source.extent.x) * sourcePixel);
            continue;
        }
        if (eightBit) {
            convertEightBit(input, from, output, to, source.extent.x);
            continue;
        }
        for (uint32_t x = 0; x < source.extent.x; ++x) {
            double channels[4] = {};
            for (uint32_t c = 0; c < sourceChannels; ++c) {
                channels[c] = readSample(input + c * sourceSample, from.sample);
            }
            double alpha = 1.0;
            if (from.alpha != PrImageAlpha_None)
                alpha = readSample(input + sourceChannels * sourceSample, from.sample);
            if (from.alpha == PrImageAlpha_Premultiplied) {
                for (uint32_t c = 0; c < sourceChannels; ++c) {
                    channels[c] = alpha != 0 ? channels[c] / alpha : 0;
                }
            }
            if (replicateGray) {
                channels[1] = channels[2] = channels[0];
            } else if (cmyk) {
                const double black = 1.0 - channels[3];
                for (uint32_t c = 0; c < 3; ++c) {
                    channels[c] = (1.0 - channels[c]) * black;
                }
            }
            if (changeSpace && fromSpace != toSpace) {
                if (from.model == PrColorModel_Gray && equalModel) {
                    channels[1] = channels[2] = channels[0];
                }
                prColorConvert(fromSpace, toSpace, channels, channels);
            }
            if (to.alpha == PrImageAlpha_Premultiplied) {
                for (uint32_t c = 0; c < destinationChannels; ++c) {
                    channels[c] *= alpha;
                }
            }
            for (uint32_t c = 0; c < destinationChannels; ++c) {
                writeSample(output + c * destinationSample, to.sample, channels[c]);
            }
            if (to.alpha != PrImageAlpha_None) {
                if (from.alpha == PrImageAlpha_Independent && from.sample == to.sample) {
                    prMemCopy(output + destinationChannels * destinationSample, input + sourceChannels * sourceSample,
                        sourceSample);
                } else {
                    writeSample(output + destinationChannels * destinationSample, to.sample, alpha);
                }
            }
            input += sourcePixel;
            output += destinationPixel;
        }
    }
    return PrStatus_Ok;
}

} // namespace pr_impl

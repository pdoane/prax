#include "pr/color_profile.h"

#include "pr/color_space.h"
#include "pr/float.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/memory.h"

namespace pr_impl {

namespace {

// The white of the profile connection space, D50.
constexpr double kD50[3] = {0.9642, 1.0, 0.8249};

uint32_t readU16(const uint8_t* p) {
    return uint32_t(p[0]) << 8 | uint32_t(p[1]);
}

double readS15Fixed16(const uint8_t* p) {
    return double(int32_t(readBeU32(p))) / 65536.0;
}

constexpr uint32_t signature(const char (&text)[5]) {
    return uint32_t(uint8_t(text[0])) << 24 | uint32_t(uint8_t(text[1])) << 16 | uint32_t(uint8_t(text[2])) << 8
           | uint32_t(uint8_t(text[3]));
}

double clamp01(double v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

// Returns `base` to the power `exponent`, for a base in [0, 1].
double power(double base, double exponent) {
    return base > 0 ? powF64(base, exponent) : exponent == 0 ? 1 : 0;
}

// A one-dimensional curve: a curv or para element at an offset into the profile's bytes.
struct Curve {
    uint32_t offset;            // 0 for the identity
};

// A lookup table's transform to the connection space: input curves, a grid of points, and output curves, with an
// optional matrix and middle curves between them (mft1, mft2 and mAB).
struct Lut {
    uint32_t type;              // signature of the element
    uint32_t inputs;
    uint32_t outputs;
    uint32_t grid[kPrColorProfileMaxChannels];
    uint32_t clut;              // offset of the grid's points, or 0 for none
    uint32_t precision;         // bytes of a grid point's value: 1 or 2
    uint32_t inputTables;       // mft1 and mft2: offset of the input tables
    uint32_t outputTables;      // mft1 and mft2: offset of the output tables
    uint32_t inputEntries;      // mft2: entries of each input table, and of each output table
    uint32_t outputEntries;
    Curve a[kPrColorProfileMaxChannels]; // mAB: curves before the grid
    Curve m[3];                          // mAB: curves before the matrix
    double matrix[12];                   // mAB: the matrix after the middle curves, and its offsets
    bool hasMatrix;
    Curve b[3];                 // mAB: curves at the end
};

struct ColorProfile {
    uint8_t* bytes;
    uint32_t size;
    PrColorModel model;
    uint32_t channels;
    bool labConnection;         // the connection space is Lab, rather than XYZ
    double white[3];            // the media white point, for absolute colorimetric

    // The matrix and curves of a gray or RGB profile, which it uses when it has no lookup table.
    bool hasMatrix;
    double columns[9];          // the red, green and blue columns
    Curve curves[3];

    // The lookup tables, by intent. Null where a profile has none.
    bool hasLut[4];
    Lut luts[4];
};

// Returns the offset and size of the tag `name`, or false if the profile has none.
bool findTag(const uint8_t* bytes, uint32_t size, uint32_t name, uint32_t* offset, uint32_t* length) {
    uint32_t count = readBeU32(bytes + 128);
    if (count > (size - 132) / 12)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* entry = bytes + 132 + 12 * i;
        if (readBeU32(entry) != name)
            continue;
        uint32_t at = readBeU32(entry + 4);
        uint32_t span = readBeU32(entry + 8);
        if (at > size || span > size - at || span < 8)
            return false;
        *offset = at;
        *length = span;
        return true;
    }
    return false;
}

// Reads the curve element at `offset`, of at most `limit` bytes. Writes how many bytes it takes, padded to four.
bool readCurve(const uint8_t* bytes, uint32_t offset, uint32_t limit, Curve* out, uint32_t* used) {
    if (limit < 12)
        return false;
    uint32_t type = readBeU32(bytes + offset);
    if (type == signature("curv")) {
        uint32_t count = readBeU32(bytes + offset + 8);
        if (count > (limit - 12) / 2)
            return false;
        *used = alignUp(12 + 2 * count, 4);
    } else if (type == signature("para")) {
        constexpr uint32_t kParameters[] = {1, 3, 4, 5, 7};
        uint32_t function = readU16(bytes + offset + 8);
        if (function > 4 || 12 + 4 * kParameters[function] > limit)
            return false;
        *used = 12 + 4 * kParameters[function];
    } else {
        return false;
    }
    out->offset = offset;
    return true;
}

double evaluateCurve(const uint8_t* bytes, Curve curve, double x) {
    x = clamp01(x);
    if (curve.offset == 0)
        return x;
    const uint8_t* p = bytes + curve.offset;
    if (readBeU32(p) == signature("curv")) {
        uint32_t count = readBeU32(p + 8);
        if (count == 0)
            return x;
        if (count == 1)
            return power(x, double(readU16(p + 12)) / 256.0);
        double at = x * double(count - 1);
        uint32_t i = uint32_t(at);
        if (i >= count - 1)
            return double(readU16(p + 12 + 2 * (count - 1))) / 65535.0;
        double f = at - double(i);
        double lo = double(readU16(p + 12 + 2 * i)) / 65535.0;
        double hi = double(readU16(p + 12 + 2 * (i + 1))) / 65535.0;
        return lo + (hi - lo) * f;
    }
    uint32_t function = readU16(p + 8);
    double g[7] = {};
    constexpr uint32_t kParameters[] = {1, 3, 4, 5, 7};
    for (uint32_t i = 0; i < kParameters[function]; ++i)
        g[i] = readS15Fixed16(p + 12 + 4 * i);
    switch (function) {
        case 0: return power(x, g[0]);
        case 1: return g[1] != 0 && x >= -g[2] / g[1] ? power(clamp01(g[1] * x + g[2]), g[0]) : 0;
        case 2: return g[1] != 0 && x >= -g[2] / g[1] ? power(clamp01(g[1] * x + g[2]), g[0]) + g[3] : g[3];
        case 3: return x >= g[4] ? power(clamp01(g[1] * x + g[2]), g[0]) : g[3] * x;
        default: return x >= g[4] ? power(clamp01(g[1] * x + g[2]), g[0]) + g[5] : g[3] * x + g[6];
    }
}

// Interpolates the table of `entries` values, each `width` bytes, starting at `at`, at x in [0, 1].
double interpolateTable(const uint8_t* bytes, uint32_t at, uint32_t entries, uint32_t width, double x) {
    double position = clamp01(x) * double(entries - 1);
    uint32_t i = uint32_t(position);
    if (i >= entries - 1)
        i = entries - 2;
    double f = position - double(i);
    double scale = width == 1 ? 255.0 : 65535.0;
    double lo = double(width == 1 ? bytes[at + i] : readU16(bytes + at + 2 * i)) / scale;
    double hi = double(width == 1 ? bytes[at + i + 1] : readU16(bytes + at + 2 * (i + 1))) / scale;
    return lo + (hi - lo) * f;
}

// Reads a lookup table element of `length` bytes at `offset`.
bool readLut(const uint8_t* bytes, uint32_t offset, uint32_t length, uint32_t channels, Lut* lut) {
    const uint8_t* p = bytes + offset;
    *lut = Lut{};
    lut->type = readBeU32(p);
    if (length < 32)
        return false;
    lut->inputs = p[8];
    lut->outputs = p[9];
    if (lut->inputs != channels || lut->outputs != 3)
        return false;
    uint64_t points = 1;
    if (lut->type == signature("mft1") || lut->type == signature("mft2")) {
        uint32_t grid = p[10];
        if (grid < 2)
            return false;
        bool wide = lut->type == signature("mft2");
        lut->precision = wide ? 2 : 1;
        uint32_t header = wide ? 52 : 48;
        if (length < header)
            return false;
        lut->inputEntries = wide ? readU16(p + 48) : 256;
        lut->outputEntries = wide ? readU16(p + 50) : 256;
        if (lut->inputEntries < 2 || lut->outputEntries < 2)
            return false;
        for (uint32_t i = 0; i < lut->inputs; ++i) {
            lut->grid[i] = grid;
            points *= grid;
        }
        uint64_t tables = uint64_t(lut->inputs) * lut->inputEntries * lut->precision;
        uint64_t clut = points * lut->outputs * lut->precision;
        uint64_t outputs = uint64_t(lut->outputs) * lut->outputEntries * lut->precision;
        if (header + tables + clut + outputs > length)
            return false;
        lut->inputTables = offset + header;
        lut->clut = lut->inputTables + uint32_t(tables);
        lut->outputTables = lut->clut + uint32_t(clut);
        return true;
    }
    if (lut->type != signature("mAB "))
        return false;
    uint32_t offsets[5];
    for (uint32_t i = 0; i < 5; ++i) {
        offsets[i] = readBeU32(p + 12 + 4 * i);
        if (offsets[i] >= length)
            return false;
    }
    auto curves = [&](uint32_t at, Curve* out, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t used = 0;
            if (at >= length || !readCurve(bytes, offset + at, length - at, &out[i], &used))
                return false;
            at += used;
        }
        return true;
    };
    // B curves are required; the matrix and M curves come together, and so do the grid and A curves.
    if (offsets[0] == 0 || !curves(offsets[0], lut->b, 3))
        return false;
    if (offsets[1] != 0) {
        if (offsets[1] + 48 > length || offsets[2] == 0 || !curves(offsets[2], lut->m, 3))
            return false;
        for (uint32_t i = 0; i < 12; ++i)
            lut->matrix[i] = readS15Fixed16(p + offsets[1] + 4 * i);
        lut->hasMatrix = true;
    }
    if (offsets[3] != 0) {
        if (offsets[3] + 20 > length || offsets[4] == 0 || !curves(offsets[4], lut->a, lut->inputs))
            return false;
        const uint8_t* clut = p + offsets[3];
        for (uint32_t i = 0; i < lut->inputs; ++i) {
            lut->grid[i] = clut[i];
            if (lut->grid[i] < 2)
                return false;
            points *= lut->grid[i];
        }
        lut->precision = clut[16];
        if ((lut->precision != 1 && lut->precision != 2)
            || offsets[3] + 20 + points * lut->outputs * lut->precision > length)
            return false;
        lut->clut = offset + offsets[3] + 20;
    }
    return true;
}

// Interpolates the grid of a lookup table at `in`, in [0, 1] along each input, across the corners of its cell.
void interpolateGrid(const uint8_t* bytes, const Lut& lut, const double* in, double out[3]) {
    uint32_t base[kPrColorProfileMaxChannels];
    double fraction[kPrColorProfileMaxChannels];
    uint32_t stride[kPrColorProfileMaxChannels];
    uint32_t step = lut.outputs;
    for (uint32_t i = lut.inputs; i-- > 0;) {
        stride[i] = step;
        step *= lut.grid[i];
    }
    for (uint32_t i = 0; i < lut.inputs; ++i) {
        double position = clamp01(in[i]) * double(lut.grid[i] - 1);
        base[i] = uint32_t(position);
        if (base[i] >= lut.grid[i] - 1)
            base[i] = lut.grid[i] - 2;
        fraction[i] = position - double(base[i]);
    }
    double scale = lut.precision == 1 ? 255.0 : 65535.0;
    out[0] = out[1] = out[2] = 0;
    for (uint32_t corner = 0; corner < (1u << lut.inputs); ++corner) {
        double weight = 1;
        uint32_t index = 0;
        for (uint32_t i = 0; i < lut.inputs; ++i) {
            bool high = (corner >> i & 1) != 0;
            weight *= high ? fraction[i] : 1 - fraction[i];
            index += (base[i] + (high ? 1 : 0)) * stride[i];
        }
        if (weight == 0)
            continue;
        for (uint32_t o = 0; o < 3; ++o) {
            uint32_t at = lut.clut + (index + o) * lut.precision;
            double value = lut.precision == 1 ? double(bytes[at]) : double(readU16(bytes + at));
            out[o] += weight * value / scale;
        }
    }
}

// Converts a Lab color of the connection space to XYZ under D50.
void labToXyz(const double lab[3], double out[3]) {
    double fy = (lab[0] + 16) / 116;
    double fx = fy + lab[1] / 500;
    double fz = fy - lab[2] / 200;
    constexpr double kEpsilon = 216.0 / 24389.0;
    constexpr double kKappa = 24389.0 / 27.0;
    double f[3] = {fx, fy, fz};
    for (uint32_t i = 0; i < 3; ++i) {
        double cube = f[i] * f[i] * f[i];
        double linear = i == 1 ? (lab[0] > kKappa * kEpsilon ? cube : lab[0] / kKappa)
                               : (cube > kEpsilon ? cube : (116 * f[i] - 16) / kKappa);
        out[i] = linear * kD50[i];
    }
}

// Converts a device color through a lookup table to the connection space, then to XYZ under D50.
void evaluateLut(const ColorProfile* profile, const Lut& lut, const double* in, double out[3]) {
    const uint8_t* bytes = profile->bytes;
    double values[kPrColorProfileMaxChannels];
    double pcs[3];
    if (lut.type == signature("mAB ")) {
        for (uint32_t i = 0; i < lut.inputs; ++i)
            values[i] = lut.clut != 0 ? evaluateCurve(bytes, lut.a[i], in[i]) : clamp01(in[i]);
        if (lut.clut != 0) {
            interpolateGrid(bytes, lut, values, pcs);
        } else {
            for (uint32_t i = 0; i < 3; ++i)
                pcs[i] = values[i];
        }
        if (lut.hasMatrix) {
            double m[3];
            for (uint32_t i = 0; i < 3; ++i)
                m[i] = evaluateCurve(bytes, lut.m[i], pcs[i]);
            for (uint32_t r = 0; r < 3; ++r)
                pcs[r] = lut.matrix[3 * r] * m[0] + lut.matrix[3 * r + 1] * m[1] + lut.matrix[3 * r + 2] * m[2]
                         + lut.matrix[9 + r];
        }
        for (uint32_t i = 0; i < 3; ++i)
            pcs[i] = evaluateCurve(bytes, lut.b[i], pcs[i]);
    } else {
        for (uint32_t i = 0; i < lut.inputs; ++i)
            values[i] = interpolateTable(bytes, lut.inputTables + i * lut.inputEntries * lut.precision,
                                         lut.inputEntries, lut.precision, in[i]);
        double grid[3];
        interpolateGrid(bytes, lut, values, grid);
        for (uint32_t o = 0; o < 3; ++o)
            pcs[o] = interpolateTable(bytes, lut.outputTables + o * lut.outputEntries * lut.precision,
                                      lut.outputEntries, lut.precision, grid[o]);
    }
    if (profile->labConnection) {
        // Lab's encodings: mAB and 8-bit tables span L* from 0 to 100 and a* and b* from -128 to 127 across [0, 1]. A
        // 16-bit table of version 2 puts 100 at 0xff00 of 0xffff.
        double scale = lut.type == signature("mft2") ? 65535.0 / 65280.0 : 1.0;
        double lab[3] = {pcs[0] * scale * 100, pcs[1] * scale * 255 - 128, pcs[2] * scale * 255 - 128};
        labToXyz(lab, out);
    } else {
        // XYZ is encoded with 1 at 0x8000.
        for (uint32_t i = 0; i < 3; ++i)
            out[i] = pcs[i] * 65535.0 / 32768.0;
    }
}

// Reads an XYZ element into `out`.
bool readXyz(const uint8_t* bytes, uint32_t size, uint32_t name, double out[3]) {
    uint32_t offset = 0;
    uint32_t length = 0;
    if (!findTag(bytes, size, name, &offset, &length) || length < 20 || readBeU32(bytes + offset) != signature("XYZ "))
        return false;
    for (uint32_t i = 0; i < 3; ++i)
        out[i] = readS15Fixed16(bytes + offset + 8 + 4 * i);
    return true;
}

bool readCurveTag(const uint8_t* bytes, uint32_t size, uint32_t name, Curve* out) {
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t used = 0;
    return findTag(bytes, size, name, &offset, &length) && readCurve(bytes, offset, length, out, &used);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrColorProfile : ColorProfile {};

PrStatus prColorProfileCreate(PrByteView bytes, PrColorProfile* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (bytes.size < 132 || bytes.size > 0xffffffffu || readBeU32(bytes.data + 36) != signature("acsp")
        || readBeU32(bytes.data) > bytes.size)
        return PrStatus_InvalidData;
    uint32_t size = uint32_t(bytes.size);
    uint32_t space = readBeU32(bytes.data + 16);
    uint32_t connection = readBeU32(bytes.data + 20);
    if (connection != signature("XYZ ") && connection != signature("Lab "))
        return PrStatus_InvalidData;
    PrColorModel model = PrColorModel_Count;
    uint32_t channels = 0;
    if (space == signature("GRAY")) {
        model = PrColorModel_Gray;
        channels = 1;
    } else if (space == signature("RGB ")) {
        model = PrColorModel_Rgb;
        channels = 3;
    } else if (space == signature("CMYK")) {
        model = PrColorModel_Cmyk;
        channels = 4;
    } else if (space == signature("Lab ")) {
        model = PrColorModel_Lab;
        channels = 3;
    } else if (space == signature("XYZ ")) {
        model = PrColorModel_Xyz;
        channels = 3;
    } else {
        // nCLR: two to eight channels, written in hexadecimal before CLR.
        uint32_t digit = space >> 24;
        uint32_t count = digit >= '2' && digit <= '9' ? digit - '0' : 0;
        if (count < 2 || count > kPrColorProfileMaxChannels || (space & 0xffffff) != (signature("0CLR") & 0xffffff))
            return PrStatus_InvalidData;
        channels = count;
    }
    PrColorProfile* profile = static_cast<PrColorProfile*>(prMemoryAlloc(sizeof(PrColorProfile)));
    *profile = PrColorProfile{};
    profile->bytes = static_cast<uint8_t*>(prMemoryAlloc(size));
    for (uint32_t i = 0; i < size; ++i)
        profile->bytes[i] = bytes.data[i];
    profile->size = size;
    profile->model = model;
    profile->channels = channels;
    profile->labConnection = connection == signature("Lab ");
    if (!readXyz(profile->bytes, size, signature("wtpt"), profile->white))
        for (uint32_t i = 0; i < 3; ++i)
            profile->white[i] = kD50[i];
    const uint32_t kTags[4] = {signature("A2B0"), signature("A2B1"), signature("A2B2"), signature("A2B1")};
    bool any = false;
    for (uint32_t intent = 0; intent < 4; ++intent) {
        uint32_t offset = 0;
        uint32_t length = 0;
        if (findTag(profile->bytes, size, kTags[intent], &offset, &length))
            profile->hasLut[intent] = readLut(profile->bytes, offset, length, channels, &profile->luts[intent]);
        any = any || profile->hasLut[intent];
    }
    // A gray or RGB profile without a lookup table maps by its curves and, for RGB, its colorants' matrix.
    if (!any && model == PrColorModel_Gray
        && readCurveTag(profile->bytes, size, signature("kTRC"), &profile->curves[0])) {
        profile->hasMatrix = true;
        any = true;
    }
    if (!any && model == PrColorModel_Rgb && !profile->labConnection) {
        const uint32_t kColumns[3] = {signature("rXYZ"), signature("gXYZ"), signature("bXYZ")};
        const uint32_t kCurves[3] = {signature("rTRC"), signature("gTRC"), signature("bTRC")};
        bool complete = true;
        for (uint32_t c = 0; c < 3 && complete; ++c) {
            complete = readXyz(profile->bytes, size, kColumns[c], &profile->columns[3 * c])
                       && readCurveTag(profile->bytes, size, kCurves[c], &profile->curves[c]);
        }
        profile->hasMatrix = complete;
        any = complete;
    }
    if (!any) {
        prColorProfileDestroy(profile);
        return PrStatus_InvalidData;
    }
    *out = profile;
    return PrStatus_Ok;
}

void prColorProfileDestroy(PrColorProfile* PR_NULLABLE profile) {
    if (profile == nullptr)
        return;
    prMemoryFree(profile->bytes);
    prMemoryFree(profile);
}

PrColorModel prColorProfileModel(const PrColorProfile* profile) {
    return profile->model;
}

uint32_t prColorProfileChannels(const PrColorProfile* profile) {
    return profile->channels;
}

void prColorProfileToXyzD50(const PrColorProfile* profile, PrColorIntent intent, const double* in,
                            double out[PR_NONNULL 3]) {
    // Absolute colorimetric is relative colorimetric scaled from the connection space's white to the media's.
    bool absolute = intent == PrColorIntent_AbsoluteColorimetric;
    uint32_t table = intent <= PrColorIntent_AbsoluteColorimetric ? intent : PrColorIntent_Perceptual;
    if (!profile->hasLut[table])
        table = PrColorIntent_Perceptual;
    if (profile->hasLut[table]) {
        evaluateLut(profile, profile->luts[table], in, out);
    } else if (profile->model == PrColorModel_Gray) {
        double y = evaluateCurve(profile->bytes, profile->curves[0], in[0]);
        for (uint32_t i = 0; i < 3; ++i)
            out[i] = kD50[i] * y;
    } else {
        double linear[3];
        for (uint32_t c = 0; c < 3; ++c)
            linear[c] = evaluateCurve(profile->bytes, profile->curves[c], in[c]);
        for (uint32_t r = 0; r < 3; ++r)
            out[r] = profile->columns[r] * linear[0] + profile->columns[3 + r] * linear[1]
                     + profile->columns[6 + r] * linear[2];
    }
    if (absolute) {
        for (uint32_t i = 0; i < 3; ++i)
            out[i] *= profile->white[i] / kD50[i];
    }
}

PrColorSpace prColorProfileNamedSpace(const PrColorProfile* profile) {
    if (profile->model != PrColorModel_Rgb && profile->model != PrColorModel_Gray)
        return PrColorSpace_Count;
    // The spaces of RGB images, the most common first. A profile matches if a grid of colors converts through it to
    // within a fraction of an 8-bit step of the space's own conversion, measured in the space.
    constexpr PrColorSpace kCandidates[] = {PrColorSpace_Srgb, PrColorSpace_DisplayP3, PrColorSpace_A98Rgb,
                                            PrColorSpace_Rec2020, PrColorSpace_ProPhotoRgb};
    constexpr uint32_t kGrid = 5;
    constexpr double kTolerance = 0.6 / 255;
    for (PrColorSpace space : kCandidates) {
        bool matches = true;
        for (uint32_t index = 0; index < kGrid * kGrid * kGrid && matches; ++index) {
            double in[3] = {double(index / (kGrid * kGrid)) / (kGrid - 1), double(index / kGrid % kGrid) / (kGrid - 1),
                            double(index % kGrid) / (kGrid - 1)};
            if (profile->model == PrColorModel_Gray)
                in[1] = in[2] = in[0];
            double xyz[3];
            prColorProfileToXyzD50(profile, PrColorIntent_RelativeColorimetric, in, xyz);
            double back[3];
            prColorConvert(PrColorSpace_XyzD50, space, xyz, back);
            for (uint32_t c = 0; c < 3 && matches; ++c) {
                double difference = back[c] - in[c];
                matches = difference < kTolerance && difference > -kTolerance;
            }
        }
        if (matches)
            return space;
    }
    return PrColorSpace_Count;
}

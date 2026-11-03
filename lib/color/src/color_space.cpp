#include "pr/color_space.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/geom.h"

namespace pr_impl {

// The ratio of a circle to its diameter.
constexpr double kPi = 3.14159265358979323846;

namespace {

struct Space {
    PrStringView name;
    PrColorModel model;
    PrColorTransfer transfer;
};

constexpr Space kSpaces[] = {
    {"srgb", PrColorModel_Rgb, PrColorTransfer_Srgb},                  // PrColorSpace_Srgb
    {"srgb-linear", PrColorModel_Rgb, PrColorTransfer_Linear},         // PrColorSpace_SrgbLinear
    {"display-p3", PrColorModel_Rgb, PrColorTransfer_Srgb},            // PrColorSpace_DisplayP3
    {"display-p3-linear", PrColorModel_Rgb, PrColorTransfer_Linear},   // PrColorSpace_DisplayP3Linear
    {"a98-rgb", PrColorModel_Rgb, PrColorTransfer_A98Rgb},             // PrColorSpace_A98Rgb
    {"prophoto-rgb", PrColorModel_Rgb, PrColorTransfer_ProPhotoRgb},   // PrColorSpace_ProPhotoRgb
    {"rec2020", PrColorModel_Rgb, PrColorTransfer_Rec2020},            // PrColorSpace_Rec2020
    {"xyz-d50", PrColorModel_Xyz, PrColorTransfer_Linear},             // PrColorSpace_XyzD50
    {"xyz-d65", PrColorModel_Xyz, PrColorTransfer_Linear},             // PrColorSpace_XyzD65
    {"lab", PrColorModel_Lab, PrColorTransfer_Linear},                 // PrColorSpace_Lab
    {"lch", PrColorModel_Lch, PrColorTransfer_Linear},                 // PrColorSpace_Lch
    {"oklab", PrColorModel_Lab, PrColorTransfer_Linear},               // PrColorSpace_Oklab
    {"oklch", PrColorModel_Lch, PrColorTransfer_Linear},               // PrColorSpace_Oklch
    {"hsl", PrColorModel_Hsl, PrColorTransfer_Linear},                 // PrColorSpace_Hsl
    {"hwb", PrColorModel_Hwb, PrColorTransfer_Linear},                 // PrColorSpace_Hwb
    {"rec2100-pq", PrColorModel_Rgb, PrColorTransfer_Pq},              // PrColorSpace_Rec2100Pq
    {"rec2100-hlg", PrColorModel_Rgb, PrColorTransfer_Hlg},            // PrColorSpace_Rec2100Hlg
    {"rec2100-linear", PrColorModel_Rgb, PrColorTransfer_Linear},      // PrColorSpace_Rec2100Linear
    {"ictcp", PrColorModel_Lab, PrColorTransfer_Linear},               // PrColorSpace_Ictcp
    {"jzazbz", PrColorModel_Lab, PrColorTransfer_Linear},              // PrColorSpace_Jzazbz
    {"jzczhz", PrColorModel_Lch, PrColorTransfer_Linear},              // PrColorSpace_Jzczhz
};

static_assert(sizeof(kSpaces) == PrColorSpace_Count * sizeof(Space));

// Returns one channel of the sRGB color for an HSL color. `n` is 0 for red, 8 for green and 4 for blue. Hue is in [0,
// 360), and saturation and lightness are in [0, 1].
double hslChannel(double n, double hue, double saturation, double lightness) {
    double k = n + hue / 30.0;
    k = k < 12.0 ? k : k - 12.0;
    double a = saturation * (lightness < 1.0 - lightness ? lightness : 1.0 - lightness);
    double ramp = k - 3.0 < 9.0 - k ? k - 3.0 : 9.0 - k;
    ramp = ramp < 1.0 ? ramp : 1.0;
    ramp = ramp > -1.0 ? ramp : -1.0;
    return lightness - a * ramp;
}

// Writes `matrix` times the column vector `in` to `out`.
void multiply(const PrDMatrix3& matrix, const double* in, double* out) {
    const double (&m)[3][3] = matrix.m;
    double x = in[0];
    double y = in[1];
    double z = in[2];
    out[0] = m[0][0] * x + m[0][1] * y + m[0][2] * z;
    out[1] = m[1][0] * x + m[1][1] * y + m[1][2] * z;
    out[2] = m[2][0] * x + m[2][1] * y + m[2][2] * z;
}

// Matrices that convert the linear RGB of each space to XYZ, and back, relative to the space's own white. The white is
// D50 for ProPhoto and D65 for the rest.
constexpr PrDMatrix3 kSrgbToXyz = {
    {
        {506752.0 / 1228815, 87881.0 / 245763, 12673.0 / 70218},
        {87098.0 / 409605, 175762.0 / 245763, 12673.0 / 175545},
        {7918.0 / 409605, 87881.0 / 737289, 1001167.0 / 1053270},
    },
};
constexpr PrDMatrix3 kXyzToSrgb = {
    {
        {12831.0 / 3959, -329.0 / 214, -1974.0 / 3959},
        {-851781.0 / 878810, 1648619.0 / 878810, 36519.0 / 878810},
        {705.0 / 12673, -2585.0 / 12673, 705.0 / 667},
    },
};
constexpr PrDMatrix3 kP3ToXyz = {
    {
        {608311.0 / 1250200, 189793.0 / 714400, 198249.0 / 1000160},
        {35783.0 / 156275, 247089.0 / 357200, 198249.0 / 2500400},
        {0.0, 32229.0 / 714400, 5220557.0 / 5000800},
    },
};
constexpr PrDMatrix3 kXyzToP3 = {
    {
        {446124.0 / 178915, -333277.0 / 357830, -72051.0 / 178915},
        {-14852.0 / 17905, 63121.0 / 35810, 423.0 / 17905},
        {11844.0 / 330415, -50337.0 / 660830, 316169.0 / 330415},
    },
};
constexpr PrDMatrix3 kA98ToXyz = {
    {
        {573536.0 / 994567, 263643.0 / 1420810, 187206.0 / 994567},
        {591459.0 / 1989134, 6239551.0 / 9945670, 374412.0 / 4972835},
        {53769.0 / 1989134, 351524.0 / 4972835, 4929758.0 / 4972835},
    },
};
constexpr PrDMatrix3 kXyzToA98 = {
    {
        {1829569.0 / 896150, -506331.0 / 896150, -308931.0 / 896150},
        {-851781.0 / 878810, 1648619.0 / 878810, 36519.0 / 878810},
        {16779.0 / 1248040, -147721.0 / 1248040, 1266979.0 / 1248040},
    },
};
constexpr PrDMatrix3 kRec2020ToXyz = {
    {
        {63426534.0 / 99577255, 20160776.0 / 139408157, 47086771.0 / 278816314},
        {26158966.0 / 99577255, 472592308.0 / 697040785, 8267143.0 / 139408157},
        {0.0, 19567812.0 / 697040785, 295819943.0 / 278816314},
    },
};
constexpr PrDMatrix3 kXyzToRec2020 = {
    {
        {30757411.0 / 17917100, -6372589.0 / 17917100, -4539589.0 / 17917100},
        {-19765991.0 / 29648200, 47925759.0 / 29648200, 467509.0 / 29648200},
        {792561.0 / 44930125, -1921689.0 / 44930125, 42328811.0 / 44930125},
    },
};
constexpr PrDMatrix3 kProPhotoToXyz = {
    {
        {0.79776664490064230, 0.13518129740053308, 0.03134773412839220},
        {0.28807482881940130, 0.71183523424187300, 0.00008993693872564},
        {0.0, 0.0, 0.82510460251046020},
    },
};
constexpr PrDMatrix3 kXyzToProPhoto = {
    {
        {1.34578688164715830, -0.25557208737979464, -0.05110186497554526},
        {-0.54463070512490190, 1.50824774284514680, 0.02052744743642139},
        {0.0, 0.0, 1.21196754563894520},
    },
};

// Bradford chromatic adaptation between the D65 and D50 whites.
constexpr PrDMatrix3 kD65ToD50 = {
    {
        {1.0479297925449969, 0.022946870601609652, -0.05019226628920524},
        {0.02962780877005599, 0.9904344267538799, -0.017073799063418826},
        {-0.009243040646204504, 0.015055191490298152, 0.7518742814281371},
    },
};
constexpr PrDMatrix3 kD50ToD65 = {
    {
        {0.955473421488075, -0.02309845494876471, 0.06325924320057072},
        {-0.0283697093338637, 1.0099953980813041, 0.021041441191917323},
        {0.012314014864481998, -0.020507649298898964, 1.330365926242124},
    },
};

// Matrices that convert XYZ relative to D65 to Oklab's cone responses and then to Oklab, and back.
constexpr PrDMatrix3 kXyzToLms = {
    {
        {0.8190224379967030, 0.3619062600528904, -0.1288737815209879},
        {0.0329836539323885, 0.9292868615863434, 0.0361446663506424},
        {0.0481771893596242, 0.2642395317527308, 0.6335478284694309},
    },
};
constexpr PrDMatrix3 kLmsToOklab = {
    {
        {0.2104542683093140, 0.7936177747023054, -0.0040720430116193},
        {1.9779985324311684, -2.4285922420485799, 0.4505937096174110},
        {0.0259040424655478, 0.7827717124575296, -0.8086757549230774},
    },
};
constexpr PrDMatrix3 kLmsToXyz = {
    {
        {1.2268798758459243, -0.5578149944602171, 0.2813910456659647},
        {-0.0405757452148008, 1.1122868032803170, -0.0717110580655164},
        {-0.0763729366746601, -0.4214933324022432, 1.5869240198367816},
    },
};
constexpr PrDMatrix3 kOklabToLms = {
    {
        {1.0, 0.3963377773761749, 0.2158037573099136},
        {1.0, -0.1055613458156586, -0.0638541728258133},
        {1.0, -0.0894841775298119, -1.2914855480194092},
    },
};

// Returns the inverse of the matrix. Unlike prDMatrix3Invert, it runs at compile time, for the tables below.
constexpr PrDMatrix3 inverse(const PrDMatrix3& matrix) {
    PrDMatrix3 inverted{};
    const double (&m)[3][3] = matrix.m;
    double (&out)[3][3] = inverted.m;
    double a = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    double b = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    double c = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    double det = m[0][0] * a + m[0][1] * b + m[0][2] * c;
    out[0][0] = a / det;
    out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    out[1][0] = b / det;
    out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    out[2][0] = c / det;
    out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    return inverted;
}

// HDR Reference White in cd/m², and PQ's constants (Rec. ITU-R BT.2100).
constexpr double kReferenceWhite = 203;
constexpr double kPqM1 = 2610.0 / 16384;
constexpr double kPqM2 = 2523.0 / 32;
constexpr double kPqC1 = 3424.0 / 4096;
constexpr double kPqC2 = 2413.0 / 128;
constexpr double kPqC3 = 2392.0 / 128;

// HLG's constants (Rec. ITU-R BT.2100), and the scale that places 18% gray at 0.38 and SDR white at 0.75 (Rec. ITU-R
// BT.2408).
constexpr double kHlgA = 0.17883277;
constexpr double kHlgB = 0.28466892;
constexpr double kHlgC = 0.55991073;
constexpr double kHlgScale = 3.7743;

// ICtCp: absolute XYZ to LMS with the crosstalk built in, then PQ-encoded LMS to ICtCp.
constexpr PrDMatrix3 kXyzToIctcpLms = {
    {
        {0.3592832590121217, 0.6976051147779502, -0.0358915932320290},
        {-0.1920808463704993, 1.1004767970374321, 0.0753748658519118},
        {0.0070797844607479, 0.0748396662186362, 0.8433265453898765},
    },
};
constexpr PrDMatrix3 kIctcpLmsToIctcp = {
    {
        {2048.0 / 4096, 2048.0 / 4096, 0},
        {6610.0 / 4096, -13613.0 / 4096, 7003.0 / 4096},
        {17933.0 / 4096, -17390.0 / 4096, -543.0 / 4096},
    },
};
constexpr PrDMatrix3 kIctcpLmsToXyz = inverse(kXyzToIctcpLms);
constexpr PrDMatrix3 kIctcpToIctcpLms = inverse(kIctcpLmsToIctcp);

// Jzazbz (Safdar et al. 2017): modified absolute XYZ to LMS, then PQ-encoded LMS to Iz, az and bz.
constexpr PrDMatrix3 kXyzToJzLms = {
    {{0.41478972, 0.579999, 0.0146480}, {-0.2015100, 1.120649, 0.0531008}, {-0.0166008, 0.264800, 0.6684799}},
};
constexpr PrDMatrix3 kJzLmsToIzazbz = {
    {{0.5, 0.5, 0}, {3.524000, -4.066708, 0.542708}, {0.199076, 1.096799, -1.295875}},
};
constexpr PrDMatrix3 kJzLmsToXyz = inverse(kXyzToJzLms);
constexpr PrDMatrix3 kIzazbzToJzLms = inverse(kJzLmsToIzazbz);
constexpr double kJzB = 1.15;
constexpr double kJzG = 0.66;
constexpr double kJzD = -0.56;
constexpr double kJzD0 = 1.6295499532821566e-11;
constexpr double kJzP = 1.7 * 2523 / 32;

// The D50 white, and the constants of Lab's lightness curve.
constexpr double kD50[3] = {0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585};
constexpr double kLabKappa = 24389.0 / 27;
constexpr double kLabEpsilon = 216.0 / 24389;

// Rec. 2020's transfer function constants.
constexpr double kRec2020Alpha = 1.09929682680944;
constexpr double kRec2020Beta = 0.018053968510807;

double signedPow(double value, double exponent) {
    double magnitude = prF64Pow(prF64Abs(value), exponent);
    return value < 0 ? -magnitude : magnitude;
}

// PQ's encoding of an absolute luminance in cd/m², with exponent `m2`, and its inverse. Negative values are black.
double pqEncode(double luminance, double m2) {
    double y = prF64Pow((luminance > 0 ? luminance : 0) / 10000, kPqM1);
    return prF64Pow((kPqC1 + kPqC2 * y) / (1 + kPqC3 * y), m2);
}

double pqDecode(double encoded, double m2) {
    double e = prF64Pow(encoded > 0 ? encoded : 0, 1 / m2);
    double num = e - kPqC1;
    return 10000 * prF64Pow((num > 0 ? num : 0) / (kPqC2 - kPqC3 * e), 1 / kPqM1);
}

// The transfer functions, in doubles. A function that extends to negative values mirrors there, and PQ clamps them to
// black.
double decode(PrColorTransfer transfer, double v) {
    double magnitude = prF64Abs(v);
    double sign = v < 0 ? -1 : 1;
    switch (transfer) {
        case PrColorTransfer_Srgb:
            return magnitude <= 0.04045 ? v / 12.92 : sign * prF64Pow((magnitude + 0.055) / 1.055, 2.4);
        case PrColorTransfer_A98Rgb:
            return signedPow(v, 563.0 / 256);
        case PrColorTransfer_ProPhotoRgb:
            return magnitude <= 16.0 / 512 ? v / 16 : signedPow(v, 1.8);
        case PrColorTransfer_Rec2020:
            if (magnitude < kRec2020Beta * 4.5)
                return v / 4.5;
            return sign * prF64Pow((magnitude + kRec2020Alpha - 1) / kRec2020Alpha, 1 / 0.45);
        case PrColorTransfer_Pq:
            return pqDecode(v, kPqM2) / kReferenceWhite;
        case PrColorTransfer_Hlg: {
            double linear = magnitude * magnitude / 3;
            if (magnitude > 0.5)
                linear = (prF64Exp((magnitude - kHlgC) / kHlgA) + kHlgB) / 12;
            return sign * linear * kHlgScale;
        }
        default:
            return v;
    }
}

double encode(PrColorTransfer transfer, double v) {
    double magnitude = prF64Abs(v);
    double sign = v < 0 ? -1 : 1;
    switch (transfer) {
        case PrColorTransfer_Srgb:
            return magnitude > 0.0031308 ? sign * (1.055 * prF64Pow(magnitude, 1 / 2.4) - 0.055) : 12.92 * v;
        case PrColorTransfer_A98Rgb:
            return signedPow(v, 256.0 / 563);
        case PrColorTransfer_ProPhotoRgb:
            return magnitude >= 1.0 / 512 ? signedPow(v, 1 / 1.8) : 16 * v;
        case PrColorTransfer_Rec2020:
            if (magnitude <= kRec2020Beta)
                return 4.5 * v;
            return sign * (kRec2020Alpha * prF64Pow(magnitude, 0.45) - (kRec2020Alpha - 1));
        case PrColorTransfer_Pq:
            return pqEncode(v * kReferenceWhite, kPqM2);
        case PrColorTransfer_Hlg: {
            double scaled = magnitude / kHlgScale;
            double encoded = scaled <= 1.0 / 12 ? prF64Sqrt(3 * scaled) : kHlgA * prF64Log(12 * scaled - kHlgB) + kHlgC;
            return sign * encoded;
        }
        default:
            return v;
    }
}

// Convert lightness and two opponent axes to lightness, chroma and hue in degrees, and back.
void toPolar(double* c) {
    double chroma = prF64Hypot(c[1], c[2]);
    double hue = chroma < 1e-12 ? 0 : prColorHue(prF64Atan2(c[2], c[1]) * 180 / kPi);
    c[1] = chroma;
    c[2] = hue;
}

void fromPolar(double* c) {
    double radians = c[2] * kPi / 180;
    double chroma = c[1];
    c[1] = chroma * prF64Cos(radians);
    c[2] = chroma * prF64Sin(radians);
}

double labF(double t) {
    return t > kLabEpsilon ? signedPow(t, 1.0 / 3) : (kLabKappa * t + 16) / 116;
}

double labFInverse(double f) {
    double cube = f * f * f;
    return cube > kLabEpsilon ? cube : (116 * f - 16) / kLabKappa;
}

// Converts sRGB channels in [0, 1] to HSL's hue, saturation and lightness, with the last two in [0, 100].
void srgbToHsl(double* c) {
    double r = c[0];
    double g = c[1];
    double b = c[2];
    double high = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double low = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double lightness = (high + low) / 2;
    double delta = high - low;
    double hue = 0;
    double saturation = 0;
    if (delta != 0) {
        if (lightness == 0 || lightness == 1)
            saturation = 0;
        else
            saturation = (high - lightness) / (lightness < 1 - lightness ? lightness : 1 - lightness);
        if (high == r)
            hue = (g - b) / delta + (g < b ? 6 : 0);
        else if (high == g)
            hue = (b - r) / delta + 2;
        else
            hue = (r - g) / delta + 4;
        hue *= 60;
    }
    if (saturation < 0) {
        hue = prColorHue(hue + 180);
        saturation = -saturation;
    }
    c[0] = hue;
    c[1] = saturation * 100;
    c[2] = lightness * 100;
}

// Converts the channels of any space to XYZ relative to D65.
void toXyzD65(PrColorSpace space, const double* in, double* out) {
    double c[3] = {in[0], in[1], in[2]};
    switch (space) {
        case PrColorSpace_Srgb:
        case PrColorSpace_SrgbLinear:
        case PrColorSpace_DisplayP3:
        case PrColorSpace_DisplayP3Linear:
        case PrColorSpace_A98Rgb:
        case PrColorSpace_Rec2020:
        case PrColorSpace_ProPhotoRgb: {
            for (double& channel : c)
                channel = decode(kSpaces[space].transfer, channel);
            const PrDMatrix3* m = &kSrgbToXyz;
            if (space == PrColorSpace_DisplayP3 || space == PrColorSpace_DisplayP3Linear)
                m = &kP3ToXyz;
            else if (space == PrColorSpace_A98Rgb)
                m = &kA98ToXyz;
            else if (space == PrColorSpace_Rec2020)
                m = &kRec2020ToXyz;
            else if (space == PrColorSpace_ProPhotoRgb)
                m = &kProPhotoToXyz;
            multiply(*m, c, out);
            if (space == PrColorSpace_ProPhotoRgb)
                multiply(kD50ToD65, out, out);
            return;
        }
        case PrColorSpace_XyzD65:
            break;
        case PrColorSpace_XyzD50:
            multiply(kD50ToD65, c, c);
            break;
        case PrColorSpace_Lch:
        case PrColorSpace_Lab: {
            if (space == PrColorSpace_Lch)
                fromPolar(c);
            double fy = (c[0] + 16) / 116;
            double fx = c[1] / 500 + fy;
            double fz = fy - c[2] / 200;
            double y = c[0] > kLabKappa * kLabEpsilon ? fy * fy * fy : c[0] / kLabKappa;
            double xyz[3] = {labFInverse(fx) * kD50[0], y * kD50[1], labFInverse(fz) * kD50[2]};
            multiply(kD50ToD65, xyz, c);
            break;
        }
        case PrColorSpace_Oklch:
        case PrColorSpace_Oklab: {
            if (space == PrColorSpace_Oklch)
                fromPolar(c);
            double lms[3];
            multiply(kOklabToLms, c, lms);
            for (double& channel : lms)
                channel = channel * channel * channel;
            multiply(kLmsToXyz, lms, c);
            break;
        }
        case PrColorSpace_Hsl:
        case PrColorSpace_Hwb: {
            PrColor rgb = space == PrColorSpace_Hsl ? prColorFromHsl(c[0], c[1], c[2], 1) : prColorFromHwb(c[0], c[1], c[2], 1);
            double srgb[3] = {double(rgb.r), double(rgb.g), double(rgb.b)};
            toXyzD65(PrColorSpace_Srgb, srgb, c);
            break;
        }
        case PrColorSpace_Rec2100Pq:
        case PrColorSpace_Rec2100Hlg:
        case PrColorSpace_Rec2100Linear:
            for (double& channel : c)
                channel = decode(kSpaces[space].transfer, channel);
            multiply(kRec2020ToXyz, c, c);
            break;
        case PrColorSpace_Ictcp: {
            double lms[3];
            multiply(kIctcpToIctcpLms, c, lms);
            for (double& channel : lms)
                channel = pqDecode(channel, kPqM2);
            multiply(kIctcpLmsToXyz, lms, c);
            for (double& channel : c)
                channel /= kReferenceWhite;
            break;
        }
        case PrColorSpace_Jzczhz:
        case PrColorSpace_Jzazbz: {
            if (space == PrColorSpace_Jzczhz)
                fromPolar(c);
            double jz = c[0] + kJzD0;
            c[0] = jz / (1 + kJzD - kJzD * jz);
            double lms[3];
            multiply(kIzazbzToJzLms, c, lms);
            for (double& channel : lms)
                channel = pqDecode(channel, kJzP);
            double modified[3];
            multiply(kJzLmsToXyz, lms, modified);
            double z = modified[2];
            double x = (modified[0] + (kJzB - 1) * z) / kJzB;
            double y = (modified[1] + (kJzG - 1) * x) / kJzG;
            c[0] = x / kReferenceWhite;
            c[1] = y / kReferenceWhite;
            c[2] = z / kReferenceWhite;
            break;
        }
        default:
            break;
    }
    out[0] = c[0];
    out[1] = c[1];
    out[2] = c[2];
}

// Converts XYZ relative to D65 to the channels of any space.
void fromXyzD65(PrColorSpace space, const double* in, double* out) {
    double c[3] = {in[0], in[1], in[2]};
    switch (space) {
        case PrColorSpace_Srgb:
        case PrColorSpace_SrgbLinear:
        case PrColorSpace_DisplayP3:
        case PrColorSpace_DisplayP3Linear:
        case PrColorSpace_A98Rgb:
        case PrColorSpace_Rec2020:
        case PrColorSpace_ProPhotoRgb: {
            if (space == PrColorSpace_ProPhotoRgb)
                multiply(kD65ToD50, c, c);
            const PrDMatrix3* m = &kXyzToSrgb;
            if (space == PrColorSpace_DisplayP3 || space == PrColorSpace_DisplayP3Linear)
                m = &kXyzToP3;
            else if (space == PrColorSpace_A98Rgb)
                m = &kXyzToA98;
            else if (space == PrColorSpace_Rec2020)
                m = &kXyzToRec2020;
            else if (space == PrColorSpace_ProPhotoRgb)
                m = &kXyzToProPhoto;
            multiply(*m, c, c);
            for (double& channel : c)
                channel = encode(kSpaces[space].transfer, channel);
            break;
        }
        case PrColorSpace_XyzD65:
            break;
        case PrColorSpace_XyzD50:
            multiply(kD65ToD50, c, c);
            break;
        case PrColorSpace_Lch:
        case PrColorSpace_Lab: {
            double xyz[3];
            multiply(kD65ToD50, c, xyz);
            double fx = labF(xyz[0] / kD50[0]);
            double fy = labF(xyz[1] / kD50[1]);
            double fz = labF(xyz[2] / kD50[2]);
            c[0] = 116 * fy - 16;
            c[1] = 500 * (fx - fy);
            c[2] = 200 * (fy - fz);
            if (space == PrColorSpace_Lch)
                toPolar(c);
            break;
        }
        case PrColorSpace_Oklch:
        case PrColorSpace_Oklab: {
            double lms[3];
            multiply(kXyzToLms, c, lms);
            for (double& channel : lms)
                channel = signedPow(channel, 1.0 / 3);
            multiply(kLmsToOklab, lms, c);
            if (space == PrColorSpace_Oklch)
                toPolar(c);
            break;
        }
        case PrColorSpace_Rec2100Pq:
        case PrColorSpace_Rec2100Hlg:
        case PrColorSpace_Rec2100Linear:
            multiply(kXyzToRec2020, c, c);
            for (double& channel : c)
                channel = encode(kSpaces[space].transfer, channel);
            break;
        case PrColorSpace_Ictcp: {
            double absolute[3] = {c[0] * kReferenceWhite, c[1] * kReferenceWhite, c[2] * kReferenceWhite};
            double lms[3];
            multiply(kXyzToIctcpLms, absolute, lms);
            for (double& channel : lms)
                channel = pqEncode(channel, kPqM2);
            multiply(kIctcpLmsToIctcp, lms, c);
            break;
        }
        case PrColorSpace_Jzazbz:
        case PrColorSpace_Jzczhz: {
            double x = c[0] * kReferenceWhite;
            double y = c[1] * kReferenceWhite;
            double z = c[2] * kReferenceWhite;
            double modified[3] = {kJzB * x - (kJzB - 1) * z, kJzG * y - (kJzG - 1) * x, z};
            double lms[3];
            multiply(kXyzToJzLms, modified, lms);
            for (double& channel : lms)
                channel = pqEncode(channel, kJzP);
            multiply(kJzLmsToIzazbz, lms, c);
            c[0] = (1 + kJzD) * c[0] / (1 + kJzD * c[0]) - kJzD0;
            if (space == PrColorSpace_Jzczhz)
                toPolar(c);
            break;
        }
        case PrColorSpace_Hsl:
        case PrColorSpace_Hwb: {
            fromXyzD65(PrColorSpace_Srgb, c, c);
            if (space == PrColorSpace_Hsl) {
                srgbToHsl(c);
            } else {
                double high = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
                double low = c[0] < c[1] ? (c[0] < c[2] ? c[0] : c[2]) : (c[1] < c[2] ? c[1] : c[2]);
                srgbToHsl(c);
                c[1] = low * 100;
                c[2] = (1 - high) * 100;
            }
            break;
        }
        default:
            break;
    }
    out[0] = c[0];
    out[1] = c[1];
    out[2] = c[2];
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrColorModel prColorSpaceModel(PrColorSpace space) {
    if (space >= PrColorSpace_Count)
        return PrColorModel_Count;
    return kSpaces[space].model;
}

uint32_t prColorSpaceHueChannel(PrColorSpace space) {
    switch (prColorSpaceModel(space)) {
        case PrColorModel_Lch:
            return 2;
        case PrColorModel_Hsl:
        case PrColorModel_Hwb:
            return 0;
        default:
            return 3;
    }
}

PrColorTransfer prColorSpaceTransfer(PrColorSpace space) {
    PR_ASSERT(space < PrColorSpace_Count);
    return kSpaces[space].transfer;
}

float prColorTransferDecode(PrColorTransfer transfer, float encoded) {
    // sRGB's is the common case, and runs in floats.
    if (transfer == PrColorTransfer_Srgb) {
        float magnitude = prF32Abs(encoded);
        float linear = magnitude <= 0.04045f ? magnitude / 12.92f : prF32Pow((magnitude + 0.055f) / 1.055f, 2.4f);
        return encoded < 0 ? -linear : linear;
    }
    return float(decode(transfer, double(encoded)));
}

float prColorTransferEncode(PrColorTransfer transfer, float linear) {
    if (transfer == PrColorTransfer_Srgb) {
        float magnitude = prF32Abs(linear);
        float encoded = magnitude <= 0.0031308f ? magnitude * 12.92f : 1.055f * prF32Pow(magnitude, 1 / 2.4f) - 0.055f;
        return linear < 0 ? -encoded : encoded;
    }
    return float(encode(transfer, double(linear)));
}

double prColorTransferDecodeDouble(PrColorTransfer transfer, double encoded) {
    return decode(transfer, encoded);
}

double prColorTransferEncodeDouble(PrColorTransfer transfer, double linear) {
    return encode(transfer, linear);
}

PrStringView prColorSpaceName(PrColorSpace space) {
    if (space >= PrColorSpace_Count)
        return PrStringView{};
    return kSpaces[space].name;
}

double prColorHue(double degrees) {
    return degrees - 360.0 * prF64Floor(degrees / 360.0);
}

PrColor prColorFromHsl(double hue, double saturation, double lightness, double alpha) {
    hue = prColorHue(hue);
    saturation /= 100.0;
    lightness /= 100.0;
    return PrColor{
        float(hslChannel(0.0, hue, saturation, lightness)),
        float(hslChannel(8.0, hue, saturation, lightness)),
        float(hslChannel(4.0, hue, saturation, lightness)), float(alpha),
    };
}

PrColor prColorFromHwb(double hue, double whiteness, double blackness, double alpha) {
    whiteness /= 100.0;
    blackness /= 100.0;
    if (whiteness + blackness >= 1.0) {
        float gray = float(whiteness / (whiteness + blackness));
        return PrColor{gray, gray, gray, float(alpha)};
    }

    hue = prColorHue(hue);
    double scale = 1.0 - whiteness - blackness;
    return PrColor{
        float(hslChannel(0.0, hue, 1.0, 0.5) * scale + whiteness),
        float(hslChannel(8.0, hue, 1.0, 0.5) * scale + whiteness),
        float(hslChannel(4.0, hue, 1.0, 0.5) * scale + whiteness), float(alpha),
    };
}

void prColorConvert(PrColorSpace from, PrColorSpace to, const double* in, double* out) {
    if (from == to) {
        double c[3] = {in[0], in[1], in[2]};
        out[0] = c[0];
        out[1] = c[1];
        out[2] = c[2];
        return;
    }
    double xyz[3];
    toXyzD65(from, in, xyz);
    fromXyzD65(to, xyz, out);
}

PrColor prColorHdrResolve(const PrColorHdr* color, float headroom) {
    float h1 = color->headrooms[0];
    float h2 = color->headrooms[1];
    if (h1 == h2)
        return prColorFromColor16(color->colors[0]);
    // The second color's weight.
    float w2 = h2 > h1 ? 1.0f : 0.0f;
    if (prF32IsFinite(headroom)) {
        w2 = (headroom - h1) / (h2 - h1);
        w2 = w2 < 0 ? 0 : w2 > 1 ? 1 : w2;
    }
    if (w2 == 0)
        return prColorFromColor16(color->colors[0]);
    if (w2 == 1)
        return prColorFromColor16(color->colors[1]);
    // The geometric mean of absolute tristimulus values, offset so that black has a mean.
    constexpr double kOffset = 0.001;
    PrColor a = prColorFromColor16(color->colors[0]);
    PrColor b = prColorFromColor16(color->colors[1]);
    double first[3] = {double(a.r), double(a.g), double(a.b)};
    double second[3] = {double(b.r), double(b.g), double(b.b)};
    prColorConvert(PrColorSpace_Srgb, PrColorSpace_XyzD65, first, first);
    prColorConvert(PrColorSpace_Srgb, PrColorSpace_XyzD65, second, second);
    double mixed[3];
    for (uint32_t i = 0; i < 3; ++i) {
        double x = prF64Max(kOffset, first[i] * kReferenceWhite + kOffset);
        double y = prF64Max(kOffset, second[i] * kReferenceWhite + kOffset);
        mixed[i] = (prF64Pow(x, double(1 - w2)) * prF64Pow(y, double(w2)) - kOffset) / kReferenceWhite;
    }
    prColorConvert(PrColorSpace_XyzD65, PrColorSpace_Srgb, mixed, mixed);
    return PrColor{float(mixed[0]), float(mixed[1]), float(mixed[2]), a.a + (b.a - a.a) * w2};
}

PrColor prColorLimit(PrColor color, float limit) {
    if (!(limit > 0))
        return color;
    double linear[3] = {
        decode(PrColorTransfer_Srgb, double(color.r)), decode(PrColorTransfer_Srgb, double(color.g)),
        decode(PrColorTransfer_Srgb, double(color.b)),
    };
    double largest = linear[0] > linear[1] ? linear[0] : linear[1];
    largest = linear[2] > largest ? linear[2] : largest;
    if (!(largest > double(limit)))
        return color;
    double scale = double(limit) / largest;
    return PrColor{
        float(encode(PrColorTransfer_Srgb, linear[0] * scale)),
        float(encode(PrColorTransfer_Srgb, linear[1] * scale)),
        float(encode(PrColorTransfer_Srgb, linear[2] * scale)), color.a,
    };
}

PrColor prColorToneMap(PrColor color, float headroom) {
    double linear[3] = {
        decode(PrColorTransfer_Srgb, double(color.r)), decode(PrColorTransfer_Srgb, double(color.g)),
        decode(PrColorTransfer_Srgb, double(color.b)),
    };
    double luminance = double(kPrColorLuminanceRed) * linear[0] + double(kPrColorLuminanceGreen) * linear[1] +
        double(kPrColorLuminanceBlue) * linear[2];
    double limit = headroom > 1 ? double(headroom) : 1;
    double knee = (1 + limit) / 2;
    if (!(luminance > knee))
        return color;
    // Past the knee, x / (1 + x) leaves the knee with a slope of one and approaches the headroom.
    double mapped = limit;
    if (limit > knee) {
        double x = (luminance - knee) / (limit - knee);
        mapped = knee + (limit - knee) * x / (1 + x);
    }
    double scale = mapped / luminance;
    return PrColor{
        float(encode(PrColorTransfer_Srgb, linear[0] * scale)),
        float(encode(PrColorTransfer_Srgb, linear[1] * scale)),
        float(encode(PrColorTransfer_Srgb, linear[2] * scale)), color.a,
    };
}

PrColor prColorGamutMap(PrColor color, PrColorSpace gamut, float limit) {
    PR_ASSERT(prColorSpaceModel(gamut) == PrColorModel_Rgb);
    if (gamut == PrColorSpace_Rec2100Pq || gamut == PrColorSpace_Rec2100Hlg)
        gamut = PrColorSpace_Rec2100Linear;
    double white = limit > 1 ? double(limit) : 1;
    // The gamut's channels as encoded, which clip to [0, high] as their linear values clip to [0, white]. A tolerance
    // keeps a color that rounding moved past an edge from mapping.
    double high = encode(kSpaces[gamut].transfer, white);
    constexpr double kTolerance = 1e-5;
    auto within = [&](const double* rgb) {
        for (uint32_t i = 0; i < 3; ++i) {
            if (!(rgb[i] >= -kTolerance && rgb[i] <= high + kTolerance))
                return false;
        }
        return true;
    };
    auto srgbOf = [&](const double* rgb) {
        double xyz[3];
        toXyzD65(gamut, rgb, xyz);
        double out[3];
        fromXyzD65(PrColorSpace_Srgb, xyz, out);
        return PrColor{float(out[0]), float(out[1]), float(out[2]), color.a};
    };
    double xyz[3];
    double srgb[3] = {double(color.r), double(color.g), double(color.b)};
    toXyzD65(PrColorSpace_Srgb, srgb, xyz);
    double rgb[3];
    fromXyzD65(gamut, xyz, rgb);
    if (within(rgb))
        return color;
    // OKLab's lightness of an achromatic color is the cube root of its luminance.
    double lch[3];
    fromXyzD65(PrColorSpace_Oklch, xyz, lch);
    if (lch[0] >= prF64Cbrt(white)) {
        double top[3] = {high, high, high};
        return srgbOf(top);
    }
    if (lch[0] <= 0)
        return PrColor{0, 0, 0, color.a};
    // Clips the OKLCH color `current` to the gamut, writing the clipped channels to `clipped`, and returns the distance
    // between the two in OKLab.
    auto clip = [&](const double* current, double* clipped) {
        double at[3];
        toXyzD65(PrColorSpace_Oklch, current, at);
        fromXyzD65(gamut, at, clipped);
        for (uint32_t i = 0; i < 3; ++i)
            clipped[i] = clipped[i] < 0 ? 0 : clipped[i] > high ? high : clipped[i];
        double a[3] = {current[0], current[1], current[2]};
        fromPolar(a);
        double b[3];
        toXyzD65(gamut, clipped, at);
        fromXyzD65(PrColorSpace_Oklab, at, b);
        return prF64Sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    };
    constexpr double kJnd = 0.02;
    constexpr double kEpsilon = 0.0001;
    double clipped[3];
    double current[3] = {lch[0], lch[1], lch[2]};
    if (clip(current, clipped) < kJnd)
        return srgbOf(clipped);
    double low = 0;
    double chroma = lch[1];
    bool lowInGamut = true;
    while (chroma - low > kEpsilon) {
        current[1] = (low + chroma) / 2;
        if (lowInGamut) {
            double at[3];
            double inside[3];
            toXyzD65(PrColorSpace_Oklch, current, at);
            fromXyzD65(gamut, at, inside);
            if (within(inside)) {
                low = current[1];
                continue;
            }
        }
        double difference = clip(current, clipped);
        if (difference < kJnd) {
            if (kJnd - difference < kEpsilon)
                break;
            lowInGamut = false;
            low = current[1];
        } else {
            chroma = current[1];
        }
    }
    return srgbOf(clipped);
}

float prColorGamutMaxChroma(PrColorSpace gamut, float lightness, float hue) {
    PR_ASSERT(prColorSpaceModel(gamut) == PrColorModel_Rgb);
    if (gamut == PrColorSpace_Rec2100Pq || gamut == PrColorSpace_Rec2100Hlg)
        gamut = PrColorSpace_Rec2100Linear;
    if (!(lightness > 0) || !(lightness < 1))
        return 0;
    double high = encode(kSpaces[gamut].transfer, 1);
    auto holds = [&](double chroma) {
        double lch[3] = {double(lightness), chroma, double(hue)};
        double xyz[3];
        toXyzD65(PrColorSpace_Oklch, lch, xyz);
        double rgb[3];
        fromXyzD65(gamut, xyz, rgb);
        for (double channel : rgb) {
            if (!(channel >= 0 && channel <= high))
                return false;
        }
        return true;
    };
    // A chroma of 1 lies past every RGB space's surface, ProPhoto's imaginary primaries included.
    double low = 0;
    double top = 1;
    for (uint32_t i = 0; i < 26; ++i) {
        double middle = (low + top) / 2;
        if (holds(middle))
            low = middle;
        else
            top = middle;
    }
    return float(low);
}

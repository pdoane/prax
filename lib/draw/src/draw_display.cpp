// The display shader (draw_display.h).

#include "pr/draw_display.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/color_space.h"
#include "pr/float16.h"

#include "draw/src/kDrawDisplayShaders.h"

struct PrDrawDisplay {
    PrGpuDevice* device;
    PrGpuShaderLibrary* library;
    PrGpuPipeline pipeline;     // for `format`, made on first use
    PrGpuFormat format;

    // The most OKLCH chroma that each gamut of kGamuts holds at each lightness and hue, in red, as half floats: a
    // column for each hue and a row for each lightness from 0 to 1.
    PrGpuTexture tables[3];
};

namespace pr_impl {

namespace {

// The extent of a gamut's table (PrDrawDisplay::tables): its hues and its lightnesses.
constexpr uint32_t kTableHues = 256;
constexpr uint32_t kTableLightnesses = 64;

// SDR white in PQ's luminance, where 1 is 10,000 cd/m² (Rec. ITU-R BT.2408).
constexpr float kPqWhite = 203.0f / 10000.0f;

// How the shader encodes the colors that it writes, numbered as the shader numbers them.
enum {
    DisplayOutput_Encoded,      // extended sRGB, encoded
    DisplayOutput_Linear,       // extended sRGB in linear light, which is scRGB
    DisplayOutput_Pq,           // Rec. 2020's primaries, encoded with PQ
};
typedef uint32_t DisplayOutput;

// What the shader converts a frame with, laid out as the shader declares it. Each matrix is three rows of a float4,
// whose last element is unused.
struct DisplayArguments {
    float toGamut[3][4];        // from linear sRGB to the linear channels of the display's gamut
    float fromGamut[3][4];      // from the linear channels of the display's gamut to linear sRGB
    float toOutput[3][4];       // Pq: from linear sRGB to the linear channels of Rec. 2020
    float limit;                // the display's headroom, at least 1
    DisplayOutput output;
    float pqWhite;              // Pq: SDR white as a fraction of PQ's 10,000 cd/m²
    uint32_t frame;
    uint32_t table;
    uint32_t _pad[3];
};

// The gamuts that the shader maps into, which index PrDrawDisplay::tables.
constexpr PrColorSpace kGamuts[3] = {PrColorSpace_Srgb, PrColorSpace_DisplayP3, PrColorSpace_Rec2020};

// Returns the gamut that the shader maps into for a display's: the one of sRGB, Display P3 and Rec. 2020 whose
// primaries the display's are, or Rec. 2020 for any other, which is the widest.
PrColorSpace gamutOf(PrColorSpace gamut) {
    switch (gamut) {
        case PrColorSpace_Srgb:
        case PrColorSpace_SrgbLinear: return PrColorSpace_Srgb;
        case PrColorSpace_DisplayP3:
        case PrColorSpace_DisplayP3Linear: return PrColorSpace_DisplayP3;
        default: return PrColorSpace_Rec2020;
    }
}

// Returns the space of a gamut of gamutOf in linear light.
PrColorSpace linearOf(PrColorSpace gamut) {
    if (gamut == PrColorSpace_Srgb)
        return PrColorSpace_SrgbLinear;
    return gamut == PrColorSpace_DisplayP3 ? PrColorSpace_DisplayP3Linear : PrColorSpace_Rec2100Linear;
}

// Writes the rows of the matrix from the RGB space `from` to `to`, both in linear light.
void writeMatrix(PrColorSpace from, PrColorSpace to, float rows[3][4]) {
    for (uint32_t column = 0; column < 3; ++column) {
        double in[3] = {column == 0 ? 1.0 : 0.0, column == 1 ? 1.0 : 0.0, column == 2 ? 1.0 : 0.0};
        double out[3];
        prColorConvert(from, to, in, out);
        for (uint32_t row = 0; row < 3; ++row)
            rows[row][column] = float(out[row]);
    }
    for (uint32_t row = 0; row < 3; ++row)
        rows[row][3] = 0;
}

// Returns the table of the most chroma that the gamut holds, or zero if the device cannot make it.
PrGpuTexture createTable(PrGpuDevice* device, PrColorSpace gamut) {
    PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba16F, PrUVec2{kTableHues, kTableLightnesses},
        PrGpuTextureUsageFlag_Sampled);
    desc.name = "display gamut";
    PrGpuTexture table{};
    if (prGpuTextureCreate(device, &desc, &table) != PrStatus_Ok)
        return PrGpuTexture{};
    Array<PrFloat16> texels;
    texels.resize(kTableHues * kTableLightnesses * 4);
    for (uint32_t y = 0; y < kTableLightnesses; ++y) {
        float lightness = float(y) / float(kTableLightnesses - 1);
        for (uint32_t x = 0; x < kTableHues; ++x) {
            float hue = 360.0f * float(x) / float(kTableHues);
            texels.data[(y * kTableHues + x) * 4] = prFloat16FromFloat(prColorGamutMaxChroma(gamut, lightness, hue));
        }
    }
    PrPixelView view{
        reinterpret_cast<const uint8_t*>(texels.data), size_t(kTableHues) * 8,
        {kTableHues, kTableLightnesses},
    };
    prGpuTextureWrite(device, table, PrIVec2{0, 0}, view);
    texels.reset();
    return table;
}

// Returns the pipeline for the pass's target's format, made the first time that format is drawn in. Returns zero if the
// device makes none.
PrGpuPipeline pipelineFor(PrDrawDisplay* display, PrGpuFormat format) {
    if (display->pipeline.id != 0 && display->format == format)
        return display->pipeline;
    prGpuPipelineDestroy(display->device, display->pipeline);
    PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(display->library);
    desc.vertex = "wholeVertex";
    desc.fragment = "convertPixel";
    desc.formats.colorCount = 1;
    desc.formats.color[0] = format;
    desc.name = "display";
    display->pipeline = PrGpuPipeline{};
    prGpuPipelineCreateGraphics(display->device, &desc, &display->pipeline);
    display->format = format;
    return display->pipeline;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prDrawDisplayCreate(PrGpuDevice* device, PrDrawDisplay* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrGpuShaderLibrary* library = nullptr;
    if (prGpuShaderLibraryCreate(device, kDrawDisplayShaders, sizeof(kDrawDisplayShaders), &library) != PrStatus_Ok)
        return PrStatus_NotSupported;
    PrDrawDisplay* display = allocZeroed<PrDrawDisplay>();
    display->device = device;
    display->library = library;
    for (uint32_t i = 0; i < 3; ++i)
        display->tables[i] = createTable(device, kGamuts[i]);
    *out = display;
    return PrStatus_Ok;
}

void prDrawDisplayDestroy(PrDrawDisplay* PR_NULLABLE display) {
    if (display == nullptr)
        return;
    for (PrGpuTexture table : display->tables)
        prGpuTextureDestroy(display->device, table);
    prGpuPipelineDestroy(display->device, display->pipeline);
    prGpuShaderLibraryDestroy(display->library);
    prMemoryFree(display);
}

void prDrawDisplayDraw(PrDrawDisplay* display, PrGpuPass* pass, PrGpuTexture frame, const PrDrawDisplayDesc* desc) {
    PrColorSpace gamut = gamutOf(desc->gamut);
    uint32_t index = gamut == PrColorSpace_Srgb ? 0 : gamut == PrColorSpace_DisplayP3 ? 1 : 2;
    PrGpuPipeline pipeline = pipelineFor(display, prGpuPassFormats(pass).color[0]);
    if (pipeline.id == 0 || display->tables[index].id == 0)
        return;
    DisplayArguments arguments{};
    writeMatrix(PrColorSpace_SrgbLinear, linearOf(gamut), arguments.toGamut);
    writeMatrix(linearOf(gamut), PrColorSpace_SrgbLinear, arguments.fromGamut);
    writeMatrix(PrColorSpace_SrgbLinear, PrColorSpace_Rec2100Linear, arguments.toOutput);
    arguments.limit = desc->headroom > 1 ? desc->headroom : 1;
    if (desc->target == PrColorSpace_Rec2100Pq)
        arguments.output = DisplayOutput_Pq;
    else if (desc->target == PrColorSpace_SrgbLinear)
        arguments.output = DisplayOutput_Linear;
    else
        arguments.output = DisplayOutput_Encoded;
    arguments.pqWhite = kPqWhite;
    arguments.frame = frame.id;
    arguments.table = display->tables[index].id;
    PrGpuCommands* commands = prGpuPassCommands(pass);
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetUniforms(commands, &arguments, sizeof(arguments));
    prGpuPassDraw(pass, 3, 1, 0, 0);
}

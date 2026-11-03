// The display shader, which maps a frame that lib/draw rendered in half floats to a display.
//
// lib/draw renders extended sRGB, encoded, and leaves colors past the display to the application. The shader tone maps
// each color for the display's headroom (prColorToneMap), maps it into the display's gamut up to the headroom
// (prColorGamutMap), and encodes it as the target holds colors. The gamut map reads a table of the most chroma at each
// lightness and hue (prColorGamutMaxChroma), which lies within a just noticeable difference of the CPU's search.
//
// The application owns the pass that the shader draws in, and the target, such as a swap chain's texture.

#pragma once

#include "pr/base.h"
#include "pr/color_space.h"
#include "pr/gpu.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The display shader's pipelines and gamut tables on one device.
typedef struct PrDrawDisplay PrDrawDisplay;

// What a frame is mapped to.
typedef struct PrDrawDisplayDesc {
    // How the target holds colors: Srgb (extended sRGB, encoded), SrgbLinear (scRGB), or Rec2100Pq (HDR10's PQ in Rec.
    // 2020's primaries, with SDR white at 203 cd/m²).
    PrColorSpace target;

    float headroom;             // the display's headroom, as a multiple of SDR white; 1 or less for SDR
    PrColorSpace gamut;         // the display's primaries (PrAppDisplay::gamut)
} PrDrawDisplayDesc;

// Creates the display shader on `device`. Returns NotSupported if the device takes none of its shaders.
PrStatus prDrawDisplayCreate(PrGpuDevice* device, PrDrawDisplay* PR_NULLABLE* PR_NONNULL out);

// Frees the display shader. The device's work that uses it must be done.
void prDrawDisplayDestroy(PrDrawDisplay* PR_NULLABLE display);

// Records a draw into `pass` that covers its target with `frame` mapped as `desc` says. `frame` holds lib/draw's colors
// in half floats, at the top left, and may be larger than the target. It is in the accesses of shader reads.
void prDrawDisplayDraw(PrDrawDisplay* display, PrGpuPass* pass, PrGpuTexture frame, const PrDrawDisplayDesc* desc);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

// An animated GIF that the tests of animation play: a few frames of one color each.

#pragma once

#include "pr/array.h"
#include "pr/count_of.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

using namespace pr;

// Returns the bytes of a GIF of 4 by 2 pixels with `frames` frames, each `centiseconds` long. Frame 0 is red, frame 1
// is green, frame 2 is blue and frame 3 is yellow, and later frames repeat the colors. The GIF plays forever if
// `forever` is set, and once otherwise.
inline Array<uint8_t> animatedGif(uint32_t frames, uint16_t centiseconds, bool forever) {
    Array<uint8_t> gif;
    const uint8_t header[] = {
        'G', 'I', 'F', '8', '9', 'a', 4, 0, 2, 0, 0x81, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0,
    };
    const uint8_t loop[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 3, 1, 0, 0, 0};
    const uint8_t frameHead[] = {
        0x21, 0xF9, 4, 0, uint8_t(centiseconds), uint8_t(centiseconds >> 8), 0, 0, 0x2C, 0, 0, 0, 0, 4, 0, 2, 0, 0,
    };
    gif.append(header, countOf(header));
    // A GIF without the loop extension plays once. A loop count of zero plays forever.
    if (forever)
        gif.append(loop, countOf(loop));
    for (uint32_t frame = 0; frame < frames; ++frame) {
        gif.append(frameHead, countOf(frameHead));
        // The image data: every code has three bits, and a clear code comes before every two pixels, so the size of the
        // codes never grows. The frame's eight pixels all have the palette index of the frame's color.
        const uint32_t color = frame % 4;
        uint32_t bits = 0;
        uint32_t count = 0;
        Array<uint8_t> packed;
        auto put = [&](uint32_t code) {
            bits |= code << count;
            count += 3;
            while (count >= 8) {
                packed.push(uint8_t(bits));
                bits >>= 8;
                count -= 8;
            }
        };
        for (uint32_t pixel = 0; pixel < 8; ++pixel) {
            if (pixel % 2 == 0)
                put(4);
            put(color);
        }
        put(5);
        if (count != 0)
            packed.push(uint8_t(bits));
        gif.push(2);
        gif.push(uint8_t(packed.count()));
        gif.append(packed.data(), packed.count());
        gif.push(0);
    }
    gif.push(0x3B);
    return gif;
}

} // namespace harness

PR_ASSUME_NONNULL_END

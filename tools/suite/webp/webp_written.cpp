// Files that libwebp's encoders write for each feature: lossless images with each transform, color caches, palettes and
// prefix code groups that its methods and qualities choose; lossy images over the quality, the loop filter's type,
// strength and sharpness, segments, partitions and spatial noise shaping; lossy images with alpha raw or compressed,
// under each filter and with its levels reduced; and animations of lossy, lossless and mixed frames, with the blending,
// disposal and key frames that the animation encoder chooses.

#include "tools/suite/webp/webp_written.h"

#include "pr/array.h"
#include "pr/file.h"
#include "pr/mem.h"
#include "pr/string.h"

#include "tools/suite/webp/webp_reference.h"

using namespace pr;

namespace webp {

namespace {

// A sequence of pseudo-random numbers, the same on every run.
struct Sequence {
    uint32_t state = 0x2545F491u;

    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
};

// What a picture shows.
enum {
    Content_Photo,      // smooth gradients, with noise and sharp shapes
    Content_Palette,    // a few colors in blocks, with some scattered
    Content_Alpha,      // the photo, under alpha that ranges from transparent to opaque
};
typedef uint8_t Content;

uint8_t clamp8(int32_t v) {
    return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
}

Array<uint8_t> picture(uint32_t width, uint32_t height, Content content, uint32_t colors, uint32_t seed) {
    Sequence random;
    random.state ^= seed * 0x9E3779B9u;
    Array<uint8_t> palette(colors * 4);
    for (uint32_t i = 0; i < colors * 4; ++i)
        palette[i] = uint8_t(random.next());
    Array<uint8_t> out(width * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* p = out.data() + (size_t(y) * width + x) * 4;
            if (content == Content_Palette) {
                uint32_t index = (x / 5 + (y / 3) * 7) % colors;
                if (random.next() % 11 == 0)
                    index = random.next() % colors;
                prMemCopy(p, palette.data() + index * 4, 4);
                p[3] = colors > 2 ? p[3] | 0x80 : 0xFF;
                continue;
            }
            const int32_t noise = int32_t(random.next() % 25) - 12;
            const bool disc = (x - width / 3) * (x - width / 3) + (y - height / 2) * (y - height / 2) < (width * height) / 9;
            p[0] = clamp8(int32_t(x * 255 / (width == 1 ? 1 : width - 1)) + noise);
            p[1] = clamp8(int32_t(y * 255 / (height == 1 ? 1 : height - 1)) - noise);
            p[2] = disc ? 230 : clamp8(int32_t(((x * y) >> 3) & 0xFF) + noise / 2);
            p[3] = 0xFF;
            if ((x / 7 + y / 9) % 5 == 0)
                p[1] = uint8_t(255 - p[1]);
            if (content == Content_Alpha) {
                const uint32_t band = (x + 2 * y) % 64;
                p[3] = band < 12 ? 0 : band > 50 ? 255 : clamp8(int32_t(band * 4) + noise);
            }
        }
    }
    return out;
}

struct Size {
    uint32_t width;
    uint32_t height;
};

constexpr Size kSizes[] = {{1, 1}, {2, 3}, {17, 13}, {64, 48}, {129, 67}};

// Writes `file` as `name` under `directory`.
bool put(const char* directory, PrStringView name, const Array<uint8_t>& file) {
    String path = format("{}/{}", PrStringView(directory), name);
    return file.count() != 0 && prFileWrite(path.view(), PrByteView{file.data(), file.count()});
}

const char* const kContentNames[] = {"photo", "palette", "alpha"};

} // namespace

bool writeAll(const char* directory) {
    bool ok = true;
    const char* const parts[] = {"lossless", "lossy", "alpha", "animation"};
    for (const char* part : parts)
        prFileCreateDirectory(format("{}/{}", PrStringView(directory), PrStringView(part)).view());

    // Lossless, by content, colors, method and quality.
    {
        const Content contents[] = {Content_Photo, Content_Palette, Content_Palette, Content_Palette, Content_Alpha};
        const uint32_t colors[] = {0, 2, 4, 16, 0};
        const int methods[] = {0, 3, 6};
        const float qualities[] = {0, 60, 100};
        uint32_t seed = 1;
        for (const Size& size : kSizes) {
            for (uint32_t c = 0; c < 5; ++c) {
                for (int method : methods) {
                    for (float quality : qualities) {
                        const Array<uint8_t> rgba = picture(size.width, size.height, contents[c], colors[c] + 1, seed++);
                        Options options;
                        options.lossless = true;
                        options.method = method;
                        options.quality = quality;
                        options.exact = true;
                        ok = put(directory, format("lossless/{}x{}.{}{}.m{}.q{}.webp", size.width, size.height,
                            kContentNames[contents[c]], colors[c] + 1, method, int(quality)),
                            encode(rgba.data(), size.width, size.height, options)) && ok;
                    }
                }
            }
        }
        // Many colors and palettes, and near-lossless preprocessing.
        const uint32_t counts[] = {3, 5, 17, 200, 256};
        for (uint32_t count : counts) {
            const Array<uint8_t> rgba = picture(97, 61, Content_Palette, count, count);
            Options options;
            options.lossless = true;
            options.method = 6;
            options.quality = 100;
            ok = put(directory, format("lossless/97x61.palette{}.m6.q100.webp", count),
                encode(rgba.data(), 97, 61, options)) && ok;
        }
        const int levels[] = {0, 40, 80};
        for (int level : levels) {
            const Array<uint8_t> rgba = picture(200, 120, Content_Alpha, 1, uint32_t(level));
            Options options;
            options.lossless = true;
            options.nearLossless = level;
            ok = put(directory, format("lossless/200x120.near{}.webp", level), encode(rgba.data(), 200, 120, options)) && ok;
        }
    }

    // Lossy, over the quality, the method, the loop filter, segments, partitions and spatial noise shaping.
    {
        struct Case {
            float quality;
            int method;
            int filterType;
            int filterStrength;
            int sharpness;
            int segments;
            int partitions;
            int sns;
        };
        const Case cases[] = {
            {75, 4, 1, 60, 0, 4, 0, 50},
            {5, 0, 0, 100, 0, 1, 0, 0},
            {20, 2, 0, 40, 3, 2, 1, 80},
            {50, 6, 1, 100, 7, 4, 3, 100},
            {95, 3, 1, 20, 2, 3, 2, 25},
            {100, 6, 0, 0, 0, 1, 0, 0},
            {35, 5, 1, 80, 5, 4, 3, 70},
            {60, 1, 0, 70, 1, 3, 1, 10},
        };
        uint32_t seed = 100;
        for (const Size& size : kSizes) {
            for (const Case& c : cases) {
                const Array<uint8_t> rgba = picture(size.width, size.height, Content_Photo, 1, seed++);
                Options options;
                options.quality = c.quality;
                options.method = c.method;
                options.filterType = c.filterType;
                options.filterStrength = c.filterStrength;
                options.filterSharpness = c.sharpness;
                options.segments = c.segments;
                options.partitions = c.partitions;
                options.sns = c.sns;
                ok = put(directory, format("lossy/{}x{}.q{}.m{}.f{}s{}h{}.g{}.p{}.n{}.webp", size.width, size.height,
                    int(c.quality), c.method, c.filterType, c.filterStrength, c.sharpness, c.segments, c.partitions,
                    c.sns), encode(rgba.data(), size.width, size.height, options)) && ok;
            }
        }
        // Automatic filtering and sharp chroma.
        const Array<uint8_t> rgba = picture(320, 240, Content_Photo, 1, 7);
        Options options;
        options.autofilter = true;
        options.sharpYuv = true;
        options.method = 6;
        ok = put(directory, "lossy/320x240.autofilter.sharp.webp", encode(rgba.data(), 320, 240, options)) && ok;
    }

    // Lossy with alpha, raw or compressed, under each filter, at full and reduced levels.
    {
        const int qualities[] = {100, 40};
        uint32_t seed = 300;
        for (const Size& size : kSizes) {
            for (int compression = 0; compression < 2; ++compression) {
                for (int filtering = 0; filtering < 3; ++filtering) {
                    for (int quality : qualities) {
                        const Array<uint8_t> rgba = picture(size.width, size.height, Content_Alpha, 1, seed++);
                        Options options;
                        options.alphaCompression = compression;
                        options.alphaFiltering = filtering;
                        options.alphaQuality = quality;
                        options.quality = 60;
                        ok = put(directory, format("alpha/{}x{}.c{}.f{}.q{}.webp", size.width, size.height,
                            compression, filtering, quality), encode(rgba.data(), size.width, size.height, options)) && ok;
                    }
                }
            }
        }
    }

    // Animations: a sprite that moves over a background that shows through, then a frame that covers the canvas, then
    // the sprite alone over a transparent canvas, from which the encoder chooses rectangles, blending and disposal.
    {
        constexpr uint32_t kWidth = 61;
        constexpr uint32_t kHeight = 45;
        constexpr uint32_t kFrames = 6;
        Array<uint8_t> frames(kWidth * kHeight * 4 * kFrames);
        const Array<uint8_t> photo = picture(kWidth, kHeight, Content_Photo, 1, 9);
        const Array<uint8_t> alpha = picture(kWidth, kHeight, Content_Alpha, 1, 10);
        for (uint32_t f = 0; f < kFrames; ++f) {
            uint8_t* canvas = frames.data() + size_t(f) * kWidth * kHeight * 4;
            const Array<uint8_t>& under = f == 3 ? photo : alpha;
            prMemCopy(canvas, under.data(), under.count());
            if (f >= 4)
                prMemZero(canvas, under.count());
            for (uint32_t y = 0; y < 12; ++y) {
                for (uint32_t x = 0; x < 14; ++x) {
                    uint8_t* p = canvas + ((y + 3 * f) * kWidth + x + 7 * f) * 4;
                    p[0] = uint8_t(40 * f);
                    p[1] = 200;
                    p[2] = uint8_t(x * 18);
                    p[3] = uint8_t(f % 2 == 0 ? 255 : 128 + y * 8);
                }
            }
        }
        const uint32_t durations[kFrames] = {100, 40, 0, 250, 70, 1000};
        struct Case {
            bool lossless;
            bool mixed;
            bool minimize;
            int kmin;
            int kmax;
        };
        const Case cases[] = {
            {false, false, false, 0, 0},
            {true, false, false, 0, 0},
            {false, true, false, 0, 0},
            {true, false, true, 1, 2},
            {false, false, true, 2, 5},
            {true, true, true, 0, 0},
        };
        for (const Case& c : cases) {
            Options options;
            options.lossless = c.lossless;
            options.exact = c.lossless;
            options.quality = 70;
            AnimationOptions animation;
            animation.allowMixed = c.mixed;
            animation.minimizeSize = c.minimize;
            animation.kmin = c.kmin;
            animation.kmax = c.kmax;
            animation.loopCount = c.lossless ? 3 : 0;
            ok = put(directory, format("animation/{}{}{}.k{}-{}.webp", c.lossless ? "lossless" : "lossy",
                c.mixed ? ".mixed" : "", c.minimize ? ".minimized" : "", c.kmin, c.kmax),
                encodeAnimation(frames.data(), durations, kFrames, kWidth, kHeight, options, animation)) && ok;
        }
    }
    return ok;
}

} // namespace webp

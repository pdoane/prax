// lib/exr's decoder beside OpenEXR's core library, over files that OpenEXR's encoders write in each compression and
// layout: scan lines, tiles of one level, and tiled mip chains and rip maps, of channels of each pixel type, with
// subsampled chroma in scan lines, and extents that leave partial blocks and tiles. Every sample decodes to OpenEXR's
// exactly, lossy compressions among them, since both decode the same bytes.

#include "pr/array.h"
#include "pr/float16.h"
#include "pr/mem.h"

#include "tools/suite/exr/exr_reference.h"

#include "tools/harness/test.h"

using namespace pr;

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

// The compressions that the decoder reads, by the value of the compression attribute.
constexpr uint32_t kCompressions[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

// Returns a smooth value at pixel (x, y) of channel `channel`, with some noise.
float value(uint32_t channel, uint32_t x, uint32_t y, Sequence& noise) {
    float base = float((x * (channel + 3) + y * 7) % 97) / 40.0f + float(channel) * 0.25f;
    return base + float(noise.next() % 64) / 4096.0f;
}

// Adds a plane of `pixel` named `name` to `level`, of the level's extent sampled at `sampling`, with smooth values. A
// perceptually linear plane says so in its pLinear.
void addPlane(exr::Level* level, const char* name, uint32_t pixel, uint32_t sampling, Sequence& noise, bool linear = false) {
    exr::Plane plane;
    plane.name = name;
    plane.pixel = pixel;
    plane.linear = linear;
    plane.xSampling = sampling;
    plane.ySampling = sampling;
    plane.sampleBytes = pixel == 1 ? 2 : 4;
    plane.width = level->width / sampling;
    plane.height = level->height / sampling;
    plane.samples.resize(plane.width * plane.height * plane.sampleBytes);
    uint32_t channel = level->planes.count();
    for (uint32_t y = 0; y < plane.height; ++y) {
        for (uint32_t x = 0; x < plane.width; ++x) {
            uint8_t* at = plane.samples.data() + (size_t(y) * plane.width + x) * plane.sampleBytes;
            float f = value(channel, x, y, noise);
            if (pixel == 0) {
                uint32_t u = uint32_t(f * 1000);
                prMemCopy(at, &u, 4);
            } else if (pixel == 1) {
                PrFloat16 h = prFloat16FromFloat(f);
                prMemCopy(at, &h, 2);
            } else {
                prMemCopy(at, &f, 4);
            }
        }
    }
    level->planes.push(static_cast<exr::Plane&&>(plane));
}

// The channels of a test image.
enum {
    Channels_Rgba,          // RGBA halves with a float depth and a uint id
    Channels_Chroma,        // luminance with chroma subsampled 2 by 2
    Channels_Layers,        // layers of color, of a float alpha, and of perceptually linear channels
};
typedef uint8_t Channels;

// An image of one part and one level of `width` by `height` pixels, of `channels`.
exr::Image makeImage(uint32_t width, uint32_t height, Channels channels) {
    Sequence noise;
    exr::Level level;
    level.width = width;
    level.height = height;
    if (channels == Channels_Chroma) {
        addPlane(&level, "BY", 1, 2, noise);
        addPlane(&level, "RY", 1, 2, noise);
        addPlane(&level, "Y", 1, 1, noise);
    } else if (channels == Channels_Layers) {
        addPlane(&level, "diffuse.B", 1, 1, noise);
        addPlane(&level, "diffuse.G", 1, 1, noise);
        addPlane(&level, "diffuse.R", 1, 1, noise);
        addPlane(&level, "mask.A", 2, 1, noise);
        addPlane(&level, "spec.R", 2, 1, noise, true);
        addPlane(&level, "spec.Y", 1, 1, noise, true);
    } else {
        addPlane(&level, "A", 1, 1, noise);
        addPlane(&level, "B", 1, 1, noise);
        addPlane(&level, "G", 1, 1, noise);
        addPlane(&level, "R", 1, 1, noise);
        addPlane(&level, "Z", 2, 1, noise);
        addPlane(&level, "id", 0, 1, noise);
    }
    exr::Part part;
    part.displayWidth = width;
    part.displayHeight = height;
    part.levels.push(static_cast<exr::Level&&>(level));
    exr::Image image;
    image.parts.push(static_cast<exr::Part&&>(part));
    return image;
}

// Checks that lib/exr decodes the file as OpenEXR does, sample for sample.
void checkSame(const Array<uint8_t>& file) {
    REQUIRE(file.count() != 0);
    PrByteView bytes{file.data(), file.count()};
    exr::Image reference;
    exr::Image prax;
    REQUIRE(exr::referenceImage(bytes, &reference));
    CHECK(exr::praxImage(bytes, &prax) == PrStatus_Ok);
    exr::Comparison comparison = exr::compare(prax, reference);
    CHECK(comparison.shapes);
    CHECK(comparison.compared != 0);
    CHECK(comparison.differ == 0);
    CHECK(comparison.outside == 0);
}

} // namespace

TEST_CASE("exr scan lines decode as OpenEXR decodes them, in each compression") {
    exr::Image image = makeImage(83, 71, Channels_Rgba);
    for (uint32_t compression : kCompressions) {
        CAPTURE(compression);
        checkSame(exr::encode(image, compression, exr::Layout_ScanLines));
    }
}

TEST_CASE("exr subsampled chroma decodes as OpenEXR decodes it, in each compression") {
    exr::Image image = makeImage(84, 70, Channels_Chroma);
    for (uint32_t compression : kCompressions) {
        CAPTURE(compression);
        checkSame(exr::encode(image, compression, exr::Layout_ScanLines));
    }
}

TEST_CASE("exr tiles decode as OpenEXR decodes them, in each compression") {
    exr::Image image = makeImage(150, 97, Channels_Rgba);
    for (uint32_t compression : kCompressions) {
        CAPTURE(compression);
        checkSame(exr::encode(image, compression, exr::Layout_Tiles));
    }
}

// OpenEXR's encoder writes the partial tiles of a level past the first at the extent that they have in the first, so
// the image's levels are whole tiles or smaller than one.
TEST_CASE("exr mip chains and rip maps decode as OpenEXR decodes them, in each compression") {
    exr::Image image = makeImage(256, 128, Channels_Rgba);
    const exr::Layout layouts[] = {exr::Layout_MipTiles, exr::Layout_RipTiles};
    for (exr::Layout layout : layouts) {
        for (uint32_t compression : kCompressions) {
            CAPTURE(layout);
            CAPTURE(compression);
            checkSame(exr::encode(image, compression, layout));
        }
    }
}

TEST_CASE("exr layers, float alpha and perceptually linear channels decode as OpenEXR decodes them") {
    exr::Image image = makeImage(77, 45, Channels_Layers);
    for (uint32_t compression : kCompressions) {
        CAPTURE(compression);
        checkSame(exr::encode(image, compression, exr::Layout_ScanLines));
        checkSame(exr::encode(image, compression, exr::Layout_Tiles));
    }
}

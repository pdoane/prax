#include "pr/align.h"
#include "pr/color_profile.h"
#include "pr/integer.h"

#include "tools/harness/test.h"

#include <vector>

namespace pr_impl {

namespace {

// Builds the bytes of an ICC profile from its tags, each an element written in full.
struct ProfileBytes {
    struct Tag {
        const char* name;
        std::vector<uint8_t> element;
    };
    const char* space;
    const char* connection;
    std::vector<Tag> tags;

    static void putU32(std::vector<uint8_t>& out, uint32_t v) {
        for (uint32_t shift = 24;; shift -= 8) {
            out.push_back(uint8_t(v >> shift));
            if (shift == 0)
                break;
        }
    }
    static void putU16(std::vector<uint8_t>& out, uint32_t v) {
        out.push_back(uint8_t(v >> 8));
        out.push_back(uint8_t(v));
    }
    static void text(std::vector<uint8_t>& out, const char* four) {
        for (uint32_t i = 0; i < 4; ++i)
            out.push_back(uint8_t(four[i]));
    }
    static void fixed(std::vector<uint8_t>& out, double v) {
        putU32(out, uint32_t(int32_t(v * 65536 + (v < 0 ? -0.5 : 0.5))));
    }

    std::vector<uint8_t> build() const {
        std::vector<uint8_t> out(128, 0);
        uint32_t size = 132 + 12 * uint32_t(tags.size());
        std::vector<uint32_t> offsets;
        for (const Tag& tag : tags) {
            offsets.push_back(size);
            size += alignUp(uint32_t(tag.element.size()), 4);
        }
        std::vector<uint8_t> head;
        putU32(head, size);
        for (uint32_t i = 0; i < 4; ++i)
            out[i] = head[i];
        for (uint32_t i = 0; i < 4; ++i) {
            out[16 + i] = uint8_t(space[i]);
            out[20 + i] = uint8_t(connection[i]);
            out[36 + i] = uint8_t("acsp"[i]);
        }
        putU32(out, uint32_t(tags.size()));
        for (size_t i = 0; i < tags.size(); ++i) {
            text(out, tags[i].name);
            putU32(out, offsets[i]);
            putU32(out, uint32_t(tags[i].element.size()));
        }
        for (const Tag& tag : tags) {
            out.insert(out.end(), tag.element.begin(), tag.element.end());
            while (out.size() % 4 != 0)
                out.push_back(0);
        }
        return out;
    }
};

std::vector<uint8_t> xyzElement(double x, double y, double z) {
    std::vector<uint8_t> e;
    ProfileBytes::text(e, "XYZ ");
    ProfileBytes::putU32(e, 0);
    ProfileBytes::fixed(e, x);
    ProfileBytes::fixed(e, y);
    ProfileBytes::fixed(e, z);
    return e;
}

// A curve of one gamma.
std::vector<uint8_t> gammaElement(double gamma) {
    std::vector<uint8_t> e;
    ProfileBytes::text(e, "curv");
    ProfileBytes::putU32(e, 0);
    ProfileBytes::putU32(e, 1);
    ProfileBytes::putU16(e, uint32_t(gamma * 256));
    return e;
}

// A parametric curve of the first kind: its input to the power `gamma`.
std::vector<uint8_t> parametricElement(double gamma) {
    std::vector<uint8_t> e;
    ProfileBytes::text(e, "para");
    ProfileBytes::putU32(e, 0);
    ProfileBytes::putU16(e, 0);
    ProfileBytes::putU16(e, 0);
    ProfileBytes::fixed(e, gamma);
    return e;
}

PrColorProfile* create(const std::vector<uint8_t>& bytes) {
    PrColorProfile* profile = nullptr;
    REQUIRE(prColorProfileCreate(bytes, &profile) == PrStatus_Ok);
    return profile;
}

bool near(const double a[3], double x, double y, double z) {
    auto close = [](double p, double q) { return p - q < 1e-3 && q - p < 1e-3; };
    return close(a[0], x) && close(a[1], y) && close(a[2], z);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an RGB profile maps by its curves and then its colorants' matrix") {
    // The colorants of red and green swapped, with linear curves for green and blue and a gamma of 2 for red.
    ProfileBytes p{
        "RGB ", "XYZ ",
        {
            {"rXYZ", xyzElement(0.3851, 0.7169, 0.0971)},
            {"gXYZ", xyzElement(0.4361, 0.2225, 0.0139)},
            {"bXYZ", xyzElement(0.1431, 0.0606, 0.7141)},
            {"rTRC", gammaElement(2)},
            {"gTRC", parametricElement(1)},
            {"bTRC", gammaElement(1)},
        },
    };
    PrColorProfile* profile = create(p.build());
    CHECK(prColorProfileModel(profile) == PrColorModel_Rgb);
    CHECK(prColorProfileChannels(profile) == 3);
    double out[3];
    const double red[3] = {1, 0, 0};
    prColorProfileToXyzD50(profile, PrColorIntent_RelativeColorimetric, red, out);
    CHECK(near(out, 0.3851, 0.7169, 0.0971));
    const double half[3] = {0.5, 0.5, 0};
    prColorProfileToXyzD50(profile, PrColorIntent_RelativeColorimetric, half, out);
    CHECK(near(out, 0.25 * 0.3851 + 0.5 * 0.4361, 0.25 * 0.7169 + 0.5 * 0.2225, 0.25 * 0.0971 + 0.5 * 0.0139));
    prColorProfileDestroy(profile);
}

TEST_CASE("a CMYK profile maps through its lookup table, whose Lab it converts to XYZ") {
    // A 16-bit table with a grid of two points per input and identity curves, whose outputs are paper white except
    // under full black ink, which is L* 0. Lab's 16-bit encoding of version 2 puts L* 100 at 0xff00 and a* and b* 0 at
    // 0x8000.
    std::vector<uint8_t> e;
    ProfileBytes::text(e, "mft2");
    ProfileBytes::putU32(e, 0);
    e.push_back(4);
    e.push_back(3);
    e.push_back(2);
    e.push_back(0);
    for (uint32_t i = 0; i < 9; ++i)
        ProfileBytes::fixed(e, i % 4 == 0 ? 1 : 0);
    ProfileBytes::putU16(e, 2);
    ProfileBytes::putU16(e, 2);
    for (uint32_t i = 0; i < 4; ++i) {
        ProfileBytes::putU16(e, 0);
        ProfileBytes::putU16(e, 0xffff);
    }
    for (uint32_t c = 0; c < 2; ++c)
        for (uint32_t m = 0; m < 2; ++m)
            for (uint32_t y = 0; y < 2; ++y)
                for (uint32_t k = 0; k < 2; ++k) {
                    ProfileBytes::putU16(e, k == 1 ? 0 : 0xff00);
                    ProfileBytes::putU16(e, 0x8000);
                    ProfileBytes::putU16(e, 0x8000);
                }
    for (uint32_t i = 0; i < 3; ++i) {
        ProfileBytes::putU16(e, 0);
        ProfileBytes::putU16(e, 0xffff);
    }
    ProfileBytes p{"CMYK", "Lab ", {{"A2B0", e}}};
    PrColorProfile* profile = create(p.build());
    CHECK(prColorProfileModel(profile) == PrColorModel_Cmyk);
    CHECK(prColorProfileChannels(profile) == 4);
    double out[3];
    const double paper[4] = {0.3, 0.2, 0.1, 0};
    prColorProfileToXyzD50(profile, PrColorIntent_RelativeColorimetric, paper, out);
    CHECK(near(out, 0.9642, 1, 0.8249));
    const double black[4] = {0, 0, 0, 1};
    prColorProfileToXyzD50(profile, PrColorIntent_Perceptual, black, out);
    CHECK(near(out, 0, 0, 0));
    prColorProfileDestroy(profile);
}

TEST_CASE("bytes that are no profile the engine reads are invalid data") {
    PrColorProfile* profile = nullptr;
    const uint8_t few[16] = {};
    CHECK(prColorProfileCreate(PrByteView{few, sizeof(few)}, &profile) == PrStatus_InvalidData);
    CHECK(profile == nullptr);
    // An RGB profile missing a curve, and one whose tag lies past its end.
    ProfileBytes partial{
        "RGB ", "XYZ ",
        {
            {"rXYZ", xyzElement(1, 0, 0)},
            {"gXYZ", xyzElement(0, 1, 0)},
            {"bXYZ", xyzElement(0, 0, 1)},
            {"rTRC", gammaElement(1)},
            {"gTRC", gammaElement(1)},
        },
    };
    std::vector<uint8_t> bytes = partial.build();
    CHECK(prColorProfileCreate(bytes, &profile) == PrStatus_InvalidData);
    ProfileBytes truncated{"GRAY", "XYZ ", {{"kTRC", gammaElement(1)}}};
    bytes = truncated.build();
    bytes.resize(bytes.size() - 4);
    CHECK(prColorProfileCreate(bytes, &profile) == PrStatus_InvalidData);
    prColorProfileDestroy(nullptr);
}

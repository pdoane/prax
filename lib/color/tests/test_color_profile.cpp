#include "pr/align.h"
#include "pr/color_profile.h"
#include "pr/integer.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

// Builds the bytes of an ICC profile from its tags, each an element written in full.
struct ProfileBytes {
    struct Tag {
        const char* name;
        Array<uint8_t> element;
    };
    const char* space;
    const char* connection;
    Array<Tag> tags;

    void tag(const char* name, Array<uint8_t> element) { tags.push(Tag{name, move(element)}); }

    static void putU32(Array<uint8_t>& out, uint32_t v) {
        for (uint32_t shift = 24;; shift -= 8) {
            out.push(uint8_t(v >> shift));
            if (shift == 0)
                break;
        }
    }
    static void putU16(Array<uint8_t>& out, uint32_t v) {
        out.push(uint8_t(v >> 8));
        out.push(uint8_t(v));
    }
    static void text(Array<uint8_t>& out, const char* four) {
        for (uint32_t i = 0; i < 4; ++i)
            out.push(uint8_t(four[i]));
    }
    static void fixed(Array<uint8_t>& out, double v) {
        putU32(out, uint32_t(int32_t(v * 65536 + (v < 0 ? -0.5 : 0.5))));
    }

    Array<uint8_t> build() const {
        Array<uint8_t> out(128);
        uint32_t size = 132 + 12 * tags.count();
        Array<uint32_t> offsets;
        for (const Tag& tag : tags) {
            offsets.push(size);
            size += alignUp(tag.element.count(), 4);
        }
        Array<uint8_t> head;
        putU32(head, size);
        for (uint32_t i = 0; i < 4; ++i)
            out[i] = head[i];
        for (uint32_t i = 0; i < 4; ++i) {
            out[16 + i] = uint8_t(space[i]);
            out[20 + i] = uint8_t(connection[i]);
            out[36 + i] = uint8_t("acsp"[i]);
        }
        putU32(out, tags.count());
        for (uint32_t i = 0; i < tags.count(); ++i) {
            text(out, tags[i].name);
            putU32(out, offsets[i]);
            putU32(out, tags[i].element.count());
        }
        for (const Tag& tag : tags) {
            out.append(tag.element.span());
            while (out.count() % 4 != 0)
                out.push(0);
        }
        return out;
    }
};

Array<uint8_t> xyzElement(double x, double y, double z) {
    Array<uint8_t> e;
    ProfileBytes::text(e, "XYZ ");
    ProfileBytes::putU32(e, 0);
    ProfileBytes::fixed(e, x);
    ProfileBytes::fixed(e, y);
    ProfileBytes::fixed(e, z);
    return e;
}

// A curve of one gamma.
Array<uint8_t> gammaElement(double gamma) {
    Array<uint8_t> e;
    ProfileBytes::text(e, "curv");
    ProfileBytes::putU32(e, 0);
    ProfileBytes::putU32(e, 1);
    ProfileBytes::putU16(e, uint32_t(gamma * 256));
    return e;
}

// A parametric curve of the first kind: its input to the power `gamma`.
Array<uint8_t> parametricElement(double gamma) {
    Array<uint8_t> e;
    ProfileBytes::text(e, "para");
    ProfileBytes::putU32(e, 0);
    ProfileBytes::putU16(e, 0);
    ProfileBytes::putU16(e, 0);
    ProfileBytes::fixed(e, gamma);
    return e;
}

PrColorProfile* create(const Array<uint8_t>& bytes) {
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
    ProfileBytes p{"RGB ", "XYZ "};
    p.tag("rXYZ", xyzElement(0.3851, 0.7169, 0.0971));
    p.tag("gXYZ", xyzElement(0.4361, 0.2225, 0.0139));
    p.tag("bXYZ", xyzElement(0.1431, 0.0606, 0.7141));
    p.tag("rTRC", gammaElement(2));
    p.tag("gTRC", parametricElement(1));
    p.tag("bTRC", gammaElement(1));
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
    Array<uint8_t> e;
    ProfileBytes::text(e, "mft2");
    ProfileBytes::putU32(e, 0);
    e.push(4);
    e.push(3);
    e.push(2);
    e.push(0);
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
    ProfileBytes p{"CMYK", "Lab "};
    p.tag("A2B0", e);
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
    ProfileBytes partial{"RGB ", "XYZ "};
    partial.tag("rXYZ", xyzElement(1, 0, 0));
    partial.tag("gXYZ", xyzElement(0, 1, 0));
    partial.tag("bXYZ", xyzElement(0, 0, 1));
    partial.tag("rTRC", gammaElement(1));
    partial.tag("gTRC", gammaElement(1));
    Array<uint8_t> bytes = partial.build();
    CHECK(prColorProfileCreate(bytes, &profile) == PrStatus_InvalidData);
    ProfileBytes truncated{"GRAY", "XYZ "};
    truncated.tag("kTRC", gammaElement(1));
    bytes = truncated.build();
    bytes.resize(bytes.count() - 4);
    CHECK(prColorProfileCreate(bytes, &profile) == PrStatus_InvalidData);
    prColorProfileDestroy(nullptr);
}

TEST_CASE("a profile's description is read from a version 2 description tag, version 4 localized text, or plain text") {
    auto describe = [](const ProfileBytes& p, size_t capacity = 64) {
        Array<uint8_t> bytes = p.build();
        char out[64];
        size_t size = prColorProfileDescription(PrByteView{bytes.data(), bytes.count()}, out, capacity);
        return String(PrStringView{out, size});
    };
    // Version 2: an ASCII count and text. The text ends at its NUL, and its spaces are trimmed.
    ProfileBytes v2{"RGB ", "XYZ "};
    Array<uint8_t> desc;
    ProfileBytes::text(desc, "desc");
    ProfileBytes::putU32(desc, 0);
    const char ascii[] = "  sRGB IEC61966-2.1 \0junk";
    ProfileBytes::putU32(desc, sizeof ascii - 1);
    for (size_t i = 0; i + 1 < sizeof ascii; ++i)
        desc.push(uint8_t(ascii[i]));
    v2.tag("desc", move(desc));
    CHECK(describe(v2) == "sRGB IEC61966-2.1");
    CHECK(describe(v2, 4) == "sRGB");

    // Version 4: the English record of several, in UTF-16.
    ProfileBytes v4{"RGB ", "XYZ "};
    Array<uint8_t> mluc;
    ProfileBytes::text(mluc, "mluc");
    ProfileBytes::putU32(mluc, 0);
    ProfileBytes::putU32(mluc, 2);
    ProfileBytes::putU32(mluc, 12);
    const char* const languages[] = {"deDE", "enUS"};
    const char16_t* const texts[] = {u"Anzeige", u"Display P3 \u00e9"};
    uint32_t at = 16 + 2 * 12;
    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t units = 0;
        while (texts[i][units] != 0)
            ++units;
        for (uint32_t k = 0; k < 4; ++k)
            mluc.push(uint8_t(languages[i][k]));
        ProfileBytes::putU32(mluc, units * 2);
        ProfileBytes::putU32(mluc, at);
        at += units * 2;
    }
    for (const char16_t* t : texts) {
        for (uint32_t k = 0; t[k] != 0; ++k)
            ProfileBytes::putU16(mluc, t[k]);
    }
    v4.tag("desc", move(mluc));
    CHECK(describe(v4) == "Display P3 \xc3\xa9");
    // A cut never splits a character.
    CHECK(describe(v4, 12) == "Display P3");

    ProfileBytes plain{"GRAY", "XYZ "};
    Array<uint8_t> text;
    ProfileBytes::text(text, "text");
    ProfileBytes::putU32(text, 0);
    for (const char* c = "Gray 2.2"; *c != 0; ++c)
        text.push(uint8_t(*c));
    plain.tag("desc", move(text));
    CHECK(describe(plain) == "Gray 2.2");

    ProfileBytes none{"RGB ", "XYZ "};
    none.tag("rTRC", gammaElement(1));
    CHECK(describe(none).isEmpty());
}

#include "pr/align.h"
#include "pr/array.h"
#include "pr/bit_cast.h"
#include "pr/float.h"

#include "font/src/hvgl.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

using namespace pr;

namespace {

constexpr double kQuarterTurn = 1.5707963267948966;

void le16(Array<uint8_t>& out, uint32_t value) {
    out.push(uint8_t(value));
    out.push(uint8_t(value >> 8));
}

void le32(Array<uint8_t>& out, uint32_t value) {
    le16(out, value & 0xffff);
    le16(out, value >> 16);
}

void putFloat(Array<uint8_t>& out, float value) {
    le32(out, bitCast<uint32_t>(value));
}

void putDouble(Array<uint8_t>& out, double value) {
    uint64_t bits = bitCast<uint64_t>(value);
    le32(out, uint32_t(bits));
    le32(out, uint32_t(bits >> 32));
}

void be16(Array<uint8_t>& out, uint32_t value) {
    out.push(uint8_t(value >> 8));
    out.push(uint8_t(value));
}

void be32(Array<uint8_t>& out, uint32_t value) {
    be16(out, value >> 16);
    be16(out, value & 0xffff);
}

void pad(Array<uint8_t>& out, uint32_t alignment) {
    while (out.count() % alignment != 0)
        out.push(0);
}

// A segment of a shape: its blend type, the four values of its master vector, and the deltas of its axis's positive
// extremum.
struct Segment {
    uint8_t blend;
    double master[4];
    double positive[4];
};

// Appends a shape of one axis and one path to the table.
void shape(Array<uint8_t>& table, const Array<Segment>& segments) {
    uint32_t count = segments.count();
    le16(table, 0);
    le16(table, 1);
    le16(table, 1);
    le16(table, count);
    le16(table, count);
    for (const Segment& s : segments)
        table.push(s.blend);
    pad(table, 8);
    for (const Segment& s : segments) {
        for (double v : s.master)
            putDouble(table, v);
    }
    // The negative extremum's column, then the positive's.
    for (uint32_t i = 0; i < 4 * count; ++i)
        putDouble(table, 0);
    for (const Segment& s : segments) {
        for (double v : s.positive)
            putDouble(table, v);
    }
}

// The values that a composite gives its tree: (row, column, value) for an extremum, with the column unused for a
// master.
struct Delta {
    uint16_t row;
    uint16_t column;
    float x;
    float y;
};

// A composite of one axis and one subpart.
struct Composite {
    uint32_t subpart;
    uint16_t treeParts;
    uint16_t treeAxes;
    Array<Delta> masterAxes;
    Array<Delta> extremumAxes;              // in order of column, then of row
    Array<Delta> masterTranslations;
    Array<Delta> extremumTranslations;
    Array<Delta> masterRotations;
};

void composite(Array<uint8_t>& table, const Composite& c) {
    uint32_t start = table.count();
    Array<uint8_t> body;
    // The subpart array, after the 36 bytes of the header.
    uint32_t subparts = 36;
    le32(body, c.subpart);
    le16(body, 0);
    le16(body, 0);
    uint32_t columns = 36 + body.count();
    uint16_t starts[3] = {0, 0, 0};
    for (const Delta& d : c.extremumAxes) {
        for (uint32_t k = d.column + 1u; k < 3; ++k)
            ++starts[k];
    }
    for (uint16_t s : starts)
        le16(body, s);
    for (const Delta& d : c.masterAxes)
        le16(body, d.row);
    for (const Delta& d : c.extremumAxes)
        le16(body, d.row);
    pad(body, 4);
    uint32_t masterAxes = 36 + body.count();
    for (const Delta& d : c.masterAxes)
        putFloat(body, d.x);
    uint32_t extremumAxes = 36 + body.count();
    for (const Delta& d : c.extremumAxes)
        putFloat(body, d.x);
    uint32_t translations = 36 + body.count();
    for (const Delta& d : c.masterTranslations) {
        putFloat(body, d.x);
        putFloat(body, d.y);
    }
    for (const Delta& d : c.extremumTranslations) {
        putFloat(body, d.x);
        putFloat(body, d.y);
    }
    for (const Delta& d : c.extremumTranslations) {
        le16(body, d.row);
        le16(body, d.column);
    }
    for (const Delta& d : c.masterTranslations)
        le16(body, d.row);
    pad(body, 4);
    uint32_t rotations = 36 + body.count();
    for (const Delta& d : c.masterRotations)
        putFloat(body, d.x);
    for (const Delta& d : c.masterRotations)
        le16(body, d.row);
    pad(body, 4);
    le16(table, 1);
    le16(table, 1);
    le16(table, 1);
    le16(table, c.treeParts);
    le16(table, c.treeAxes);
    le16(table, 2);
    le16(table, c.masterAxes.count());
    le16(table, c.extremumAxes.count());
    le16(table, c.masterTranslations.count());
    le16(table, c.masterRotations.count());
    le16(table, c.extremumTranslations.count());
    le16(table, 0);
    const uint32_t offsets[] = {subparts, columns, masterAxes, extremumAxes, translations, rotations};
    for (uint32_t offset : offsets)
        le16(table, offset / 4);
    REQUIRE(table.count() - start == 36);
    table.append(body.span());
}

// Returns an hvgl table of the parts that `write` appends, the first `glyphs` of which are glyphs.
template <typename Write>
Array<uint8_t> hvglOf(uint32_t parts, uint32_t glyphs, Write write) {
    Array<uint8_t> table;
    le16(table, 3);
    le16(table, 1);
    le32(table, 0);
    le32(table, parts);
    le32(table, 24);
    le32(table, glyphs);
    le32(table, 0);
    uint32_t index = table.count();
    for (uint32_t i = 0; i <= parts; ++i)
        le32(table, 0);
    for (uint32_t i = 0; i < parts; ++i) {
        pad(table, 8);
        uint32_t offset = table.count() - index;
        for (uint32_t k = 0; k < 4; ++k)
            table[index + 4 * i + k] = uint8_t(offset >> (8 * k));
        write(table, i);
    }
    pad(table, 8);
    uint32_t end = table.count() - index;
    for (uint32_t k = 0; k < 4; ++k)
        table[index + 4 * parts + k] = uint8_t(end >> (8 * k));
    return table;
}

// Returns a font file of the tables, in the order given, each on a boundary of 8 bytes.
Array<uint8_t> fileOf(const Array<uint32_t>& tags, const Array<Array<uint8_t>>& tables) {
    Array<uint8_t> file;
    be32(file, 0x00010000);
    be16(file, tags.count());
    be16(file, 0);
    be16(file, 0);
    be16(file, 0);
    uint32_t offset = 12 + 16 * tags.count();
    for (uint32_t i = 0; i < tags.count(); ++i) {
        offset = alignUp(offset, 8u);
        be32(file, tags[i]);
        be32(file, 0);
        be32(file, offset);
        be32(file, tables[i].count());
        offset += tables[i].count();
    }
    for (const Array<uint8_t>& table : tables) {
        pad(file, 8);
        file.append(table.span());
    }
    return file;
}

// The shape of the tests: a corner at the origin, a curve halfway between its control points, and a tangent off the
// line of its control points, whose first control point moves right by 2 at its axis's positive extremum.
Array<Segment> testShape() {
    return Array<Segment>({
        Segment{1, {0, 0, 10, 0}, {0, 0, 2, 0}},
        Segment{0, {0.5, 0, 10, 10}, {0, 0, 0, 0}},
        Segment{2, {6, 9, 0, 10}, {0, 0, 0, 0}},
    });
}

// The composite of the tests, glyph 0. It sets the shape at its axis value of 0.5, less half its own axis value below
// 0, turned a quarter turn about its origin, and then moved 100 right, and 10 left at its own axis's positive extremum
// as the turn carries the extremum's translation up.
Composite placing(uint32_t subpart) {
    Composite c{};
    c.subpart = subpart;
    c.treeParts = 2;
    c.treeAxes = 2;
    c.masterAxes.push(Delta{0, 0, 0.5f, 0});
    c.extremumAxes.push(Delta{0, 0, -0.5f, 0});
    c.masterTranslations.push(Delta{0, 0, 100, 0});
    c.extremumTranslations.push(Delta{0, 1, 0, 10});
    c.masterRotations.push(Delta{0, 0, float(kQuarterTurn), 0});
    return c;
}

// The parts of the tests: glyph 0, `placing`; glyph 1, which places glyph 0 at its own axis's positive extremum, moves
// it 1000 right, adds 0.25 to the shape's axis value, and moves the shape 5 right before glyph 0 places it; the shape;
// and a second shape, which an hvpm may put in the first's place.
Array<uint8_t> testTable() {
    return hvglOf(4, 2, [](Array<uint8_t>& table, uint32_t part) {
        if (part == 0) {
            composite(table, placing(2));
        } else if (part == 1) {
            Composite c{};
            c.subpart = 0;
            c.treeParts = 3;
            c.treeAxes = 3;
            c.masterAxes.push(Delta{0, 0, 1, 0});
            c.masterAxes.push(Delta{1, 0, 0.25f, 0});
            c.masterTranslations.push(Delta{0, 0, 1000, 0});
            c.masterTranslations.push(Delta{1, 0, 5, 0});
            composite(table, c);
        } else if (part == 2) {
            shape(table, testShape());
        } else {
            shape(table, Array<Segment>({
                Segment{1, {0, 0, 1, 0}, {}},
                Segment{1, {1, 1, 0, 1}, {}},
                Segment{1, {0, 2, 0, 2}, {}},
            }));
        }
    });
}

// Returns the points of the glyph of the face at the coordinate of its one axis.
Array<PrVec2> pointsOf(const HvglFace& face, uint32_t glyph, float coordinate) {
    FontGlyphOutline outline{};
    SfntCoords coords{&coordinate, 1};
    REQUIRE(hvglLoad(face, coords, glyph, &outline));
    Array<PrVec2> points;
    for (uint32_t i = 0; i < outline.points.count; ++i)
        points.push(outline.points.data[i]);
    fontOutlineReset(outline);
    return points;
}

void checkPoints(const Array<PrVec2>& points, const Array<PrVec2>& expected) {
    REQUIRE(points.count() == expected.count());
    for (uint32_t i = 0; i < points.count(); ++i) {
        CAPTURE(i);
        CHECK(points[i].x == doctest::Approx(expected[i].x).epsilon(1e-5));
        CHECK(points[i].y == doctest::Approx(expected[i].y).epsilon(1e-5));
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("hvgl shapes place curve and tangent points from their control points, as their composites place them") {
    Array<uint8_t> file =
        fileOf(Array<uint32_t>({PR_FONT_TAG('h', 'v', 'g', 'l')}), Array<Array<uint8_t>>({testTable()}));
    Sfnt sfnt{};
    REQUIRE(sfntOpen(file.data(), file.count(), 0, &sfnt));
    HvglFace face{};
    REQUIRE(hvglOpen(sfnt, &face));
    CHECK(face.glyphCount == 2);
    // Each segment is its on-curve point, then its control point. The quarter turn maps (x, y) to (-y, x).
    checkPoints(pointsOf(face, 0, 0), Array<PrVec2>({{100, 0}, {100, 11}, {95, 10.5f}, {90, 10}, {90, 6}, {90, 0}}));
    checkPoints(pointsOf(face, 0, 1), Array<PrVec2>({{90, 0}, {90, 11}, {85, 10.5f}, {80, 10}, {80, 6}, {80, 0}}));
    checkPoints(pointsOf(face, 0, -1), Array<PrVec2>({{100, 0}, {100, 10}, {95, 10}, {90, 10}, {90, 6}, {90, 0}}));
    // Glyph 1 moves the shape 5 right in the frame of glyph 0, before glyph 0 turns it.
    checkPoints(pointsOf(face, 1, 0), Array<PrVec2>({
        {1090, 5}, {1090, 16.5f}, {1085, 15.75f}, {1080, 15}, {1080, 11}, {1080, 5},
    }));
    FontGlyphOutline outline{};
    CHECK_FALSE(hvglLoad(face, SfntCoords{}, 2, &outline));
    fontOutlineReset(outline);
}

TEST_CASE("an hvpm map puts parts in the place of others") {
    Array<uint8_t> map;
    le16(map, 1);
    le16(map, 0);
    le32(map, 8);
    le32(map, 1);
    le32(map, 2);
    le32(map, 2);
    le32(map, 3);
    Array<uint8_t> file = fileOf(Array<uint32_t>({PR_FONT_TAG('h', 'v', 'g', 'l'), PR_FONT_TAG('h', 'v', 'p', 'm')}),
        Array<Array<uint8_t>>({testTable(), map}));
    Sfnt sfnt{};
    REQUIRE(sfntOpen(file.data(), file.count(), 0, &sfnt));
    HvglFace face{};
    REQUIRE(hvglOpen(sfnt, &face));
    checkPoints(pointsOf(face, 0, 0), Array<PrVec2>({{100, 0}, {100, 1}, {99, 1}, {99, 0}, {98, 0}, {98, 0}}));
}

TEST_CASE("an hvgl table whose parts do not fit draws nothing of them") {
    // A path of two segments is not drawn, and a composite whose subpart is past the end draws nothing.
    Array<uint8_t> table = hvglOf(2, 2, [](Array<uint8_t>& t, uint32_t part) {
        if (part == 0) {
            composite(t, placing(7));
        } else {
            Array<Segment> segments = testShape();
            segments.pop();
            shape(t, segments);
        }
    });
    Array<uint8_t> file = fileOf(Array<uint32_t>({PR_FONT_TAG('h', 'v', 'g', 'l')}), Array<Array<uint8_t>>({table}));
    Sfnt sfnt{};
    REQUIRE(sfntOpen(file.data(), file.count(), 0, &sfnt));
    HvglFace face{};
    REQUIRE(hvglOpen(sfnt, &face));
    CHECK(pointsOf(face, 0, 0).count() == 0);
    CHECK(pointsOf(face, 1, 0).count() == 0);
    // A part index that runs past the table does not open.
    Array<uint8_t> cut = table;
    cut.resize(30);
    file = fileOf(Array<uint32_t>({PR_FONT_TAG('h', 'v', 'g', 'l')}), Array<Array<uint8_t>>({cut}));
    REQUIRE(sfntOpen(file.data(), file.count(), 0, &sfnt));
    CHECK_FALSE(hvglOpen(sfnt, &face));
}

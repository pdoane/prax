// Path data: paths written in the SVG path syntax.

#include "pr/path.h"
#include "pr/path_data.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

// Returns the verbs of the path, one letter each, as path data spells them.
std::string verbs(const PrPath* path) {
    std::string out;
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i)
        out += "MLQCZ"[prPathVerbs(path)[i]];
    return out;
}

PrVec2 lastPoint(const PrPath* path) {
    return prPathPoints(path)[prPathPointCount(path) - 1];
}

bool near(PrVec2 a, PrVec2 b) {
    return a.x - b.x < 1e-3f && b.x - a.x < 1e-3f && a.y - b.y < 1e-3f && b.y - a.y < 1e-3f;
}

// Replaces `path` with the path that `data` describes, and returns true if `data` has no error.
bool parse(PrStringView data, PrPath* path) {
    prPathClear(path);
    return prPathAppendData(path, data) == PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("path data draws its commands, relative and implicit ones included") {
    PrPath* path = prPathCreate();
    CHECK(parse("M10 20 30 40 h5 v-5 z m1,1 l2 2", path));
    CHECK(verbs(path) == "MLLLZML");
    CHECK(near(lastPoint(path), PrVec2{13, 23}));

    // Numbers need no separators when a sign or a second decimal point starts the next one.
    CHECK(parse("M.5.5L-1-2", path));
    CHECK(near(prPathPoints(path)[0], PrVec2{0.5f, 0.5f}));
    CHECK(near(lastPoint(path), PrVec2{-1, -2}));

    // A smooth curve reflects the previous control point.
    CHECK(parse("M0 0 C0 10 10 10 10 0 S20 -10 20 0", path));
    CHECK(verbs(path) == "MCC");
    CHECK(near(prPathPoints(path)[4], PrVec2{10, -10}));
    prPathDestroy(path);
}

TEST_CASE("path data in error draws what came before the error") {
    PrPath* path = prPathCreate();
    CHECK_FALSE(parse("M0 0 L10 10 L20", path));
    CHECK(verbs(path) == "ML");
    CHECK_FALSE(parse("L10 10", path));
    CHECK(prPathVerbCount(path) == 0);
    prPathDestroy(path);
}

TEST_CASE("an arc's flags need no separators, and zero radii draw a line") {
    PrPath* path = prPathCreate();
    CHECK(parse("M0 0 a10 10 0 1110 0", path));
    CHECK(near(lastPoint(path), PrVec2{10, 0}));
    CHECK(parse("M0 0 A0 5 0 0 1 10 0", path));
    CHECK(verbs(path) == "ML");
    prPathDestroy(path);
}

TEST_CASE("an arc that ends where it starts is a segment of no length") {
    PrPath* path = prPathCreate();
    CHECK(parse("M80 30 A20 20 0 0 1 80 30", path));
    CHECK(verbs(path) == "ML");
    CHECK(near(lastPoint(path), PrVec2{80, 30}));
    prPathDestroy(path);
}

TEST_CASE("path data parses into its commands, with implicit commands made explicit") {
    PrPathDataCommand commands[8];
    uint32_t count = prPathDataParse("M10 20 30 40 h5 a1 2 3 1 0 4 5 z", commands, 8);
    REQUIRE(count == 5);
    CHECK(commands[0].letter == 'M');
    CHECK(commands[1].letter == 'L');
    CHECK(commands[1].arguments[0] == 30.0f);
    CHECK(commands[1].arguments[1] == 40.0f);
    CHECK(commands[2].letter == 'h');
    CHECK(commands[3].letter == 'a');
    CHECK(commands[3].arguments[3] == 1.0f);
    CHECK(commands[3].arguments[4] == 0.0f);
    CHECK(commands[3].arguments[6] == 5.0f);
    CHECK(commands[4].letter == 'z');

    // The count covers every command, beyond the capacity too, and stops at the first error.
    CHECK(prPathDataParse("M0 0 L10 10 L20", nullptr, 0) == 2);
    CHECK(prPathDataParse("L10 10", commands, 8) == 0);
    CHECK(prPathDataArgumentCount('a') == 7);
    CHECK(prPathDataArgumentCount('Z') == 0);
}

TEST_CASE("commands append the path that their data draws") {
    PrPathDataCommand commands[8];
    uint32_t count = prPathDataParse("M10 20 30 40 h5 a1 2 3 1 0 4 5 z", commands, 8);
    PrPath* fromData = prPathCreate();
    PrPath* fromCommands = prPathCreate();
    CHECK(parse("M10 20 30 40 h5 a1 2 3 1 0 4 5 z", fromData));
    prPathAppendCommands(fromCommands, commands, count);
    CHECK(verbs(fromCommands) == verbs(fromData));
    CHECK(near(lastPoint(fromCommands), lastPoint(fromData)));
    prPathDestroy(fromData);
    prPathDestroy(fromCommands);
}

TEST_CASE("each command's vertex has the path's directions through its segment") {
    PrPathDataCommand commands[8];
    PrPathDataVertex vertices[8];
    // A segment of no length starts in the direction that the segment before it ends in, and ends in the direction that
    // the one after it starts in. A moveto has no length, and the first has no segment before it.
    uint32_t count = prPathDataParse("M 0 0 L 10 0 L 10 0 L 10 10 Z", commands, 8);
    prPathDataVertices(commands, count, vertices);
    CHECK(near(vertices[0].point, PrVec2{0, 0}));
    CHECK(near(vertices[0].startDirection, PrVec2{1, 0}));
    CHECK(near(vertices[2].startDirection, PrVec2{1, 0}));
    CHECK(near(vertices[2].endDirection, PrVec2{0, 1}));
    // A closepath's segment runs back to the start of its subpath, where its vertex is.
    CHECK(near(vertices[4].point, PrVec2{0, 0}));
    CHECK(near(vertices[4].startDirection, PrVec2{-0.7071f, -0.7071f}));

    // An arc leaves and arrives along its tangents: this one bulges up, clockwise where y points down.
    count = prPathDataParse("M 0 0 A 10 10 0 0 1 20 0", commands, 8);
    prPathDataVertices(commands, count, vertices);
    CHECK(near(vertices[1].startDirection, PrVec2{0, -1}));
    CHECK(near(vertices[1].endDirection, PrVec2{0, 1}));

    // Every direction of a path of no length is the positive x-axis.
    count = prPathDataParse("M 5 5 L 5 5 Z", commands, 8);
    prPathDataVertices(commands, count, vertices);
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(near(vertices[i].startDirection, PrVec2{1, 0}));
        CHECK(near(vertices[i].endDirection, PrVec2{1, 0}));
    }
}

TEST_CASE("path data written from commands reads back as the same commands") {
    PrPathDataCommand commands[8];
    uint32_t count = prPathDataParse("m 10 20 l 40 50 z l 40 60 z t 70 130", commands, 8);
    PrRawString text = prRawStringNull();
    prPathDataWrite(commands, count, &text);
    CHECK(prRawStringView(&text) == "m 10 20 l 40 50 Z l 40 60 Z t 70 130");

    // In absolute coordinates, a closepath returns to the start of its subpath.
    prPathDataMakeAbsolute(commands, count);
    prRawStringReplace(&text, "");
    prPathDataWrite(commands, count, &text);
    CHECK(prRawStringView(&text) == "M 10 20 L 50 70 Z L 50 80 Z T 80 150");

    // Numbers take the fewest digits that read back to them, and a flag above zero is 1.
    count = prPathDataParse("M 0.1 -2.5e-3 A 1 2 3 1 0 4 5", commands, 8);
    commands[1].arguments[3] = 0.25f;
    commands[1].arguments[4] = -1;
    prRawStringReplace(&text, "");
    prPathDataWrite(commands, count, &text);
    CHECK(prRawStringView(&text) == "M 0.1 -0.0025 A 1 2 3 1 0 4 5");
    prRawStringReset(&text);
}

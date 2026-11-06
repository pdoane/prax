// Drawing a terminal's screen.

#include "studio/terminal/src/terminal_draw.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// The code point of the graphics protocol's image placeholder (pr/vterm.h). It draws no glyph.
constexpr uint32_t kImagePlaceholder = 0x10eeee;

// The weight of one arm of a box-drawing character. An arm runs from the cell's center to one edge.
enum {
    Arm_None,
    Arm_Light,
    Arm_Heavy,
    Arm_Double,
};
typedef uint8_t Arm;

// The direction of an arm, which is also the arm's 2-bit field index in an arms value.
enum {
    Side_Up,
    Side_Right,
    Side_Down,
    Side_Left,
};
typedef uint8_t Side;

// Packs the arms of a box-drawing character, 2 bits each by Side. Zero means the font draws the character.
constexpr uint8_t arms(uint8_t up, uint8_t right, uint8_t down, uint8_t left) {
    return uint8_t(up | right << 2 | down << 4 | left << 6);
}

constexpr uint8_t kL = Arm_Light;
constexpr uint8_t kH = Arm_Heavy;
constexpr uint8_t kD = Arm_Double;

// The arms of U+2500 to U+257F, from their Unicode names. drawArc draws the arcs. The font draws the dashed lines and
// the diagonals.
constexpr uint8_t kBoxArms[128] = {
    arms(0, kL, 0, kL), arms(0, kH, 0, kH), arms(kL, 0, kL, 0), arms(kH, 0, kH, 0),         // 2500
    0, 0, 0, 0, 0, 0, 0, 0,                                                                 // 2504
    arms(0, kL, kL, 0), arms(0, kH, kL, 0), arms(0, kL, kH, 0), arms(0, kH, kH, 0),         // 250C
    arms(0, 0, kL, kL), arms(0, 0, kL, kH), arms(0, 0, kH, kL), arms(0, 0, kH, kH),         // 2510
    arms(kL, kL, 0, 0), arms(kL, kH, 0, 0), arms(kH, kL, 0, 0), arms(kH, kH, 0, 0),         // 2514
    arms(kL, 0, 0, kL), arms(kL, 0, 0, kH), arms(kH, 0, 0, kL), arms(kH, 0, 0, kH),         // 2518
    arms(kL, kL, kL, 0), arms(kL, kH, kL, 0), arms(kH, kL, kL, 0), arms(kL, kL, kH, 0),     // 251C
    arms(kH, kL, kH, 0), arms(kH, kH, kL, 0), arms(kL, kH, kH, 0), arms(kH, kH, kH, 0),     // 2520
    arms(kL, 0, kL, kL), arms(kL, 0, kL, kH), arms(kH, 0, kL, kL), arms(kL, 0, kH, kL),     // 2524
    arms(kH, 0, kH, kL), arms(kH, 0, kL, kH), arms(kL, 0, kH, kH), arms(kH, 0, kH, kH),     // 2528
    arms(0, kL, kL, kL), arms(0, kL, kL, kH), arms(0, kH, kL, kL), arms(0, kH, kL, kH),     // 252C
    arms(0, kL, kH, kL), arms(0, kL, kH, kH), arms(0, kH, kH, kL), arms(0, kH, kH, kH),     // 2530
    arms(kL, kL, 0, kL), arms(kL, kL, 0, kH), arms(kL, kH, 0, kL), arms(kL, kH, 0, kH),     // 2534
    arms(kH, kL, 0, kL), arms(kH, kL, 0, kH), arms(kH, kH, 0, kL), arms(kH, kH, 0, kH),     // 2538
    arms(kL, kL, kL, kL), arms(kL, kL, kL, kH), arms(kL, kH, kL, kL), arms(kL, kH, kL, kH), // 253C
    arms(kH, kL, kL, kL), arms(kL, kL, kH, kL), arms(kH, kL, kH, kL), arms(kH, kL, kL, kH), // 2540
    arms(kH, kH, kL, kL), arms(kL, kL, kH, kH), arms(kL, kH, kH, kL), arms(kH, kH, kL, kH), // 2544
    arms(kL, kH, kH, kH), arms(kH, kL, kH, kH), arms(kH, kH, kH, kL), arms(kH, kH, kH, kH), // 2548
    0, 0, 0, 0,                                                                             // 254C
    arms(0, kD, 0, kD), arms(kD, 0, kD, 0), arms(0, kD, kL, 0), arms(0, kL, kD, 0),         // 2550
    arms(0, kD, kD, 0), arms(0, 0, kL, kD), arms(0, 0, kD, kL), arms(0, 0, kD, kD),         // 2554
    arms(kL, kD, 0, 0), arms(kD, kL, 0, 0), arms(kD, kD, 0, 0), arms(kL, 0, 0, kD),         // 2558
    arms(kD, 0, 0, kL), arms(kD, 0, 0, kD), arms(kL, kD, kL, 0), arms(kD, kL, kD, 0),       // 255C
    arms(kD, kD, kD, 0), arms(kL, 0, kL, kD), arms(kD, 0, kD, kL), arms(kD, 0, kD, kD),     // 2560
    arms(0, kD, kL, kD), arms(0, kL, kD, kL), arms(0, kD, kD, kD), arms(kL, kD, 0, kD),     // 2564
    arms(kD, kL, 0, kL), arms(kD, kD, 0, kD), arms(kL, kD, kL, kD), arms(kD, kL, kD, kL),   // 2568
    arms(kD, kD, kD, kD), 0, 0, 0,                                                          // 256C
    0, 0, 0, 0,                                                                             // 2570
    arms(0, 0, 0, kL), arms(kL, 0, 0, 0), arms(0, kL, 0, 0), arms(0, 0, kL, 0),             // 2574
    arms(0, 0, 0, kH), arms(kH, 0, 0, 0), arms(0, kH, 0, 0), arms(0, 0, kH, 0),             // 2578
    arms(0, kH, 0, kL), arms(kL, 0, kH, 0), arms(0, kL, 0, kH), arms(kH, 0, kL, 0),         // 257C
};

uint8_t armOf(uint8_t value, Side side) {
    return uint8_t(value >> (2 * side) & 3);
}

// Resolves `color`, using the color of `role` for a default color, as prVtermColorResolve does.
PrColor colorOf(const TerminalColors& colors, PrVtermColor color, PrVtermColorRole role) {
    switch (color.kind) {
        case PrVtermColorKind_Palette:
            return prColorFromColor8(colors.palette[prVtermColorIndex(color)]);
        case PrVtermColorKind_Rgb:
            return prColorFromColor8(PrColor8{color.red, color.green, color.blue, 0xff});
        default:
            return prColorFromColor8(colors.roles[role]);
    }
}

bool sameColor(PrColor a, PrColor b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

PrColor mixed(PrColor a, PrColor b, float t) {
    return PrColor{prF32Lerp(a.r, b.r, t), prF32Lerp(a.g, b.g, t), prF32Lerp(a.b, b.b, t), prF32Lerp(a.a, b.a, t)};
}

PrDrawPaint paintOf(PrColor color) {
    PrDrawPaint paint{};
    paint.color = prColor16FromColor(color);
    return paint;
}

void fill(PrDrawList* list, float left, float top, float right, float bottom, PrColor color) {
    if (right > left && bottom > top)
        prDrawListRect(list, PrRect{left, top, right, bottom}, paintOf(color));
}

// Rounds lengths in px to whole target pixels.
struct PixelGrid {
    float ratio;

    float snap(float value) const { return prF32Round(value * ratio) / ratio; }
    float floor(float value) const { return prF32Floor(value * ratio) / ratio; }
    float atLeastOne(float value) const {
        float snapped = snap(value);
        return snapped > 0 ? snapped : 1 / ratio;
    }
};

// One axis of a box-drawing cell. Computes where a line starts across the axis, so that the line is centered in the
// cell on whole target pixels. `single` places a line of `thickness`, and `doubled` places the first of the two lines
// of a double line.
struct BoxAxis {
    float start;        // the cell's starting edge on this axis
    float extent;

    float single(const PixelGrid& p, float thickness) const { return start + p.floor((extent - thickness) / 2); }
    float doubled(const PixelGrid& p, float light) const { return start + p.floor((extent - 3 * light) / 2); }
};

// Records the arms of a box-drawing character, packed as `arms` does.
void drawBox(const TerminalDrawer* drawer, PrDrawList* list, uint8_t value, PrRect rect, PrColor color) {
    PixelGrid p{drawer->pixelRatio};
    float light = p.atLeastOne(drawer->metrics.underlineThickness);
    float heavy = 2 * light;
    BoxAxis across{rect.left, rect.right - rect.left};    // the x of vertical lines
    BoxAxis down{rect.top, rect.bottom - rect.top};       // the y of horizontal lines
    uint8_t up = armOf(value, Side_Up);
    uint8_t right = armOf(value, Side_Right);
    uint8_t below = armOf(value, Side_Down);
    uint8_t left = armOf(value, Side_Left);

    // The thickest single line on each axis sets the size of the joint, so that the arms of both axes meet in it.
    auto thickness = [&](uint8_t arm) { return arm == Arm_Heavy ? heavy : light; };
    const uint8_t verticals[2] = {up, below};
    const uint8_t horizontals[2] = {right, left};
    float vertical = 0;
    for (uint8_t arm : verticals) {
        if (arm == Arm_Light || arm == Arm_Heavy)
            vertical = prF32Max(vertical, thickness(arm));
    }
    float horizontal = 0;
    for (uint8_t arm : horizontals) {
        if (arm == Arm_Light || arm == Arm_Heavy)
            horizontal = prF32Max(horizontal, thickness(arm));
    }
    float verticalX = across.single(p, vertical > 0 ? vertical : light);
    float horizontalY = down.single(p, horizontal > 0 ? horizontal : light);
    float doubleX = across.doubled(p, light);
    float doubleY = down.doubled(p, light);
    bool anyDouble = up == Arm_Double || right == Arm_Double || below == Arm_Double || left == Arm_Double;
    // A single arm that meets a double line reaches across the joint to the far line.
    float jointLeft = anyDouble ? doubleX : verticalX;
    float jointRight = anyDouble ? doubleX + 3 * light : verticalX + (vertical > 0 ? vertical : light);
    float jointTop = anyDouble ? doubleY : horizontalY;
    float jointBottom = anyDouble ? doubleY + 3 * light : horizontalY + (horizontal > 0 ? horizontal : light);
    float centerX = doubleX + 1.5f * light;
    float centerY = doubleY + 1.5f * light;

    // Where one of the two lines of a double arm starts, as an offset from the center toward the arm's edge. The line
    // stops short of a perpendicular arm on its own side. It starts at the center if the arm continues straight across.
    // At a corner, it starts at the far edge of the joint.
    auto doubleStart = [&](bool sideArm, bool opposite, bool otherSide) {
        if (sideArm)
            return 0.5f * light;
        if (opposite)
            return 0.0f;
        return otherSide ? -1.5f * light : 0.0f;
    };

    // The horizontal arms: right, then left.
    for (uint32_t i = 0; i < 2; ++i) {
        uint8_t arm = horizontals[i];
        bool toRight = i == 0;
        uint8_t opposite = toRight ? left : right;
        if (arm == Arm_Light || arm == Arm_Heavy) {
            float t = thickness(arm);
            float y = down.single(p, t);
            if (toRight)
                fill(list, jointLeft, y, rect.right, y + t, color);
            else
                fill(list, rect.left, y, jointRight, y + t, color);
        } else if (arm == Arm_Double) {
            float lines[2] = {doubleY, doubleY + 2 * light};
            bool sideArms[2] = {up != Arm_None, below != Arm_None};
            for (uint32_t k = 0; k < 2; ++k) {
                float offset = doubleStart(sideArms[k], opposite != Arm_None, sideArms[1 - k]);
                if (toRight)
                    fill(list, centerX + offset, lines[k], rect.right, lines[k] + light, color);
                else
                    fill(list, rect.left, lines[k], centerX - offset, lines[k] + light, color);
            }
        }
    }

    // The vertical arms: down, then up.
    for (uint32_t i = 0; i < 2; ++i) {
        uint8_t arm = verticals[1 - i];
        bool toBottom = i == 0;
        uint8_t opposite = toBottom ? up : below;
        if (arm == Arm_Light || arm == Arm_Heavy) {
            float t = thickness(arm);
            float x = across.single(p, t);
            if (toBottom)
                fill(list, x, jointTop, x + t, rect.bottom, color);
            else
                fill(list, x, rect.top, x + t, jointBottom, color);
        } else if (arm == Arm_Double) {
            float lines[2] = {doubleX, doubleX + 2 * light};
            bool sideArms[2] = {left != Arm_None, right != Arm_None};
            for (uint32_t k = 0; k < 2; ++k) {
                float offset = doubleStart(sideArms[k], opposite != Arm_None, sideArms[1 - k]);
                if (toBottom)
                    fill(list, lines[k], centerY + offset, lines[k] + light, rect.bottom, color);
                else
                    fill(list, lines[k], rect.top, lines[k] + light, centerY - offset, color);
            }
        }
    }
}

// Records a block element of U+2580 to U+259F, which fills part of the cell or shades all of it.
bool drawBlock(const TerminalDrawer* drawer, PrDrawList* list, uint32_t codePoint, PrRect rect, PrColor color) {
    PixelGrid p{drawer->pixelRatio};
    float width = rect.right - rect.left;
    float height = rect.bottom - rect.top;
    auto x = [&](float fraction) { return rect.left + p.snap(width * fraction); };
    auto y = [&](float fraction) { return rect.top + p.snap(height * fraction); };
    if (codePoint == 0x2580) {
        fill(list, rect.left, rect.top, rect.right, y(0.5f), color);
    } else if (codePoint >= 0x2581 && codePoint <= 0x2588) {
        // Lower blocks, from one eighth of the cell to all of it.
        fill(list, rect.left, y(1 - float(codePoint - 0x2580) / 8), rect.right, rect.bottom, color);
    } else if (codePoint >= 0x2589 && codePoint <= 0x258f) {
        // Left blocks, from seven eighths of the cell down to one eighth.
        fill(list, rect.left, rect.top, x(float(0x2590 - codePoint) / 8), rect.bottom, color);
    } else if (codePoint == 0x2590) {
        fill(list, x(0.5f), rect.top, rect.right, rect.bottom, color);
    } else if (codePoint >= 0x2591 && codePoint <= 0x2593) {
        PrColor shade = color;
        shade.a *= float(codePoint - 0x2590) / 4;
        fill(list, rect.left, rect.top, rect.right, rect.bottom, shade);
    } else if (codePoint == 0x2594) {
        fill(list, rect.left, rect.top, rect.right, y(0.125f), color);
    } else if (codePoint == 0x2595) {
        fill(list, x(0.875f), rect.top, rect.right, rect.bottom, color);
    } else if (codePoint >= 0x2596 && codePoint <= 0x259f) {
        // Quadrants. Bits 0 to 3 are the upper left, upper right, lower left and lower right.
        constexpr uint8_t kQuadrants[10] = {0x4, 0x8, 0x1, 0xd, 0x9, 0x7, 0xb, 0x2, 0x6, 0xe};
        uint8_t quadrants = kQuadrants[codePoint - 0x2596];
        float midX = x(0.5f);
        float midY = y(0.5f);
        if ((quadrants & 1) != 0)
            fill(list, rect.left, rect.top, midX, midY, color);
        if ((quadrants & 2) != 0)
            fill(list, midX, rect.top, rect.right, midY, color);
        if ((quadrants & 4) != 0)
            fill(list, rect.left, midY, midX, rect.bottom, color);
        if ((quadrants & 8) != 0)
            fill(list, midX, midY, rect.right, rect.bottom, color);
    } else {
        return false;
    }
    return true;
}

// Records an arc of U+256D to U+2570. The arc is a light line that turns a quarter circle between its two arms. It is
// centered on the same lines as drawBox, so that it meets the light lines of neighboring cells.
void drawArc(const TerminalDrawer* drawer, PrDrawList* list, uint32_t codePoint, PrRect rect, PrColor color) {
    PixelGrid p{drawer->pixelRatio};
    float light = p.atLeastOne(drawer->metrics.underlineThickness);
    BoxAxis across{rect.left, rect.right - rect.left};
    BoxAxis down{rect.top, rect.bottom - rect.top};
    float lineX = across.single(p, light);
    float lineY = down.single(p, light);
    float centerX = lineX + light / 2;
    float centerY = lineY + light / 2;
    // Whether the horizontal arm goes right or left, and whether the vertical arm goes down or up.
    bool toRight = codePoint == 0x256d || codePoint == 0x2570;
    bool toBottom = codePoint == 0x256d || codePoint == 0x256e;
    float reachX = toRight ? rect.right - centerX : centerX - rect.left;
    float reachY = toBottom ? rect.bottom - centerY : centerY - rect.top;
    float radius = prF32Min(reachX, reachY);
    float dx = toRight ? radius : -radius;
    float dy = toBottom ? radius : -radius;
    // The arc runs a quarter turn clockwise from `start`, in radians clockwise from +x. Its center lies diagonally
    // beyond the corner. For an arc with arms right and down, the turn therefore starts pointing left, and the other
    // arcs rotate from there.
    constexpr float kPi = 3.14159265f;
    float start = 0;
    if (toRight)
        start = toBottom ? kPi : 0.5f * kPi;
    else
        start = toBottom ? 1.5f * kPi : 0;
    // The arc's circle reaches past the cell. The cell clips it, so that the list's bounds stay within the cell.
    prDrawListPushClipRect(list, rect, PrDrawEdges_Antialiased);
    prDrawListSector(list, PrVec2{centerX + dx, centerY + dy}, radius - light / 2, radius + light / 2, start, 0.5f * kPi,
        paintOf(color));
    prDrawListPopClip(list);
    // The arms run straight from the ends of the arc to the cell's edges.
    if (toRight)
        fill(list, centerX + radius, lineY, rect.right, lineY + light, color);
    else
        fill(list, rect.left, lineY, centerX - radius, lineY + light, color);
    if (toBottom)
        fill(list, lineX, centerY + radius, lineX + light, rect.bottom, color);
    else
        fill(list, lineX, rect.top, lineX + light, centerY - radius, color);
}

// Records a Powerline separator of U+E0B0 to U+E0BF. These private-use characters separate the segments of status lines
// and prompts. Each spans the whole cell, so that it meets the backgrounds on either side. Returns false for any other
// code point.
bool drawPowerline(const TerminalDrawer* drawer, PrDrawList* list, uint32_t codePoint, PrRect rect, PrColor color) {
    PixelGrid p{drawer->pixelRatio};
    float middle = (rect.top + rect.bottom) / 2;
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = p.atLeastOne(drawer->metrics.underlineThickness);
    PrDrawPaint fillPaint = paintOf(color);
    PrDrawPaint strokePaint = fillPaint;
    strokePaint.stroke = &stroke;
    // The corners of the cell, and the midpoints of its left and right edges.
    PrVec2 topLeft{rect.left, rect.top};
    PrVec2 topRight{rect.right, rect.top};
    PrVec2 bottomLeft{rect.left, rect.bottom};
    PrVec2 bottomRight{rect.right, rect.bottom};
    PrVec2 leftMiddle{rect.left, middle};
    PrVec2 rightMiddle{rect.right, middle};
    // Triangles and lines by code point from U+E0B0, filled or stroked. An empty entry is a half ellipse.
    struct Figure {
        PrVec2 points[3];
        uint32_t pointCount;
        bool filled;
    };
    const Figure figures[] = {
        {{topLeft, rightMiddle, bottomLeft}, 3, true},
        {{topLeft, rightMiddle, bottomLeft}, 3, false},
        {{topRight, leftMiddle, bottomRight}, 3, true},
        {{topRight, leftMiddle, bottomRight}, 3, false},
        {},
        {},
        {},
        {},
        {{topLeft, bottomLeft, bottomRight}, 3, true},
        {{topLeft, bottomRight}, 2, false},
        {{topRight, bottomRight, bottomLeft}, 3, true},
        {{topRight, bottomLeft}, 2, false},
        {{topLeft, topRight, bottomLeft}, 3, true},
        {{topRight, bottomLeft}, 2, false},
        {{topLeft, topRight, bottomRight}, 3, true},
        {{topLeft, bottomRight}, 2, false},
    };
    if (codePoint < 0xe0b0 || codePoint > 0xe0bf)
        return false;
    const Figure& figure = figures[codePoint - 0xe0b0];
    // A stroke's joins and caps would spill into neighboring cells, so the cell clips every figure.
    prDrawListPushClipRect(list, rect, PrDrawEdges_Antialiased);
    if (figure.pointCount != 0) {
        PrDrawContour contour = figure.filled ? PrDrawContour_Closed : PrDrawContour_Open;
        prDrawListPolylineContour(list, figure.points, figure.pointCount, contour, PrPathFillRule_NonZero,
            figure.filled ? fillPaint : strokePaint);
    } else {
        // Half ellipses, filled or outlined. They bulge right from the left edge, or left from the right edge.
        bool right = codePoint == 0xe0b4 || codePoint == 0xe0b5;
        bool filled = codePoint == 0xe0b4 || codePoint == 0xe0b6;
        float inset = filled ? 0 : stroke.shape.width / 2;
        PrVec2 center{right ? rect.left : rect.right, middle};
        PrVec2 radii{rect.right - rect.left - inset, (rect.bottom - rect.top) / 2 - inset};
        prDrawListEllipse(list, center, radii, 0, filled ? fillPaint : strokePaint);
    }
    prDrawListPopClip(list);
    return true;
}

// Records a braille pattern of U+2800 to U+28FF: up to eight dots in two columns of four rows. Bits 0 to 2 are the top
// three dots of the left column, bits 3 to 5 those of the right column, and bits 6 and 7 the bottom dots of the left
// and the right column.
void drawBraille(const TerminalDrawer* drawer, PrDrawList* list, uint32_t codePoint, PrRect rect, PrColor color) {
    PixelGrid p{drawer->pixelRatio};
    float width = rect.right - rect.left;
    float height = rect.bottom - rect.top;
    float pitch = width / 2 < height / 4 ? width / 2 : height / 4;
    float radius = pitch * 0.3f;
    // The column and row of the dot of each bit.
    constexpr uint8_t kColumns[8] = {0, 0, 0, 1, 1, 1, 0, 1};
    constexpr uint8_t kRows[8] = {0, 1, 2, 0, 1, 2, 3, 3};
    uint32_t bits = codePoint - 0x2800;
    for (uint32_t bit = 0; bit < 8; ++bit) {
        if ((bits & (1u << bit)) == 0)
            continue;
        PrVec2 center{
            rect.left + p.snap(width * (float(kColumns[bit]) * 2 + 1) / 4),
            rect.top + p.snap(height * (float(kRows[bit]) * 2 + 1) / 8),
        };
        prDrawListEllipse(list, center, PrVec2{radius, radius}, 0, paintOf(color));
    }
}

// The index of the drawer's font for the attributes of `cell` (kTerminalFontCount).
uint32_t fontOf(const PrVtermCell& cell) {
    uint32_t index = 0;
    if ((cell.flags & PrVtermCellFlag_Bold) != 0)
        index |= 1;
    if ((cell.flags & PrVtermCellFlag_Italic) != 0)
        index |= 2;
    return index;
}

// Appends the grapheme cluster of `cell` to the drawer's text, as UTF-8.
void appendCluster(TerminalDrawer* drawer, const PrVtermLine& line, const PrVtermCell& cell) {
    char bytes[kPrUtf8MaxBytes];
    uint32_t size = prUtf8Encode(cell.codePoint, bytes);
    drawer->text.append(bytes, size);
    for (uint32_t i = 0; i < cell.clusterCount; ++i) {
        size = prUtf8Encode(line.codePoints[cell.clusterStart + i], bytes);
        drawer->text.append(bytes, size);
    }
}

// Returns the index of the drawer's glyph group of `color`, and adds the group if it is new.
uint32_t groupOf(TerminalDrawer* drawer, PrColor color) {
    for (uint32_t i = 0; i < drawer->groups.count; ++i) {
        if (sameColor(drawer->groups.data[i], color))
            return i;
    }
    drawer->groups.push(color);
    return drawer->groups.count - 1;
}

// Returns true if the cell draws no glyph: it is empty, an image placeholder, the tail of a wide cluster, or a spacer.
bool isEmpty(const PrVtermCell& cell) {
    return cell.codePoint == 0 || cell.codePoint == kImagePlaceholder ||
        (cell.flags & (PrVtermCellFlag_WideTail | PrVtermCellFlag_WideSpacer)) != 0;
}

// Returns true if the glyph and decorations of `cell` are hidden, because it blinks and the blink is in its hidden
// phase.
bool isBlinkHidden(const PrVtermCell& cell, bool blinkHidden) {
    return blinkHidden && (cell.flags & PrVtermCellFlag_Blink) != 0;
}

// Returns true if `cell` blinks and draws something visible: a glyph other than a space, or a decoration.
bool blinks(const PrVtermCell& cell) {
    if ((cell.flags & PrVtermCellFlag_Blink) == 0 || (cell.flags & PrVtermCellFlag_Invisible) != 0)
        return false;
    bool glyph = !isEmpty(cell) && cell.codePoint != ' ';
    bool decorated = cell.underline != PrVtermUnderline_None || cell.link != kPrVtermLinkNone ||
        (cell.flags & (PrVtermCellFlag_Strikethrough | PrVtermCellFlag_Overline)) != 0;
    return glyph || decorated;
}

PrDrawDecorationStyle decorationStyleOf(PrVtermUnderline underline) {
    switch (underline) {
        case PrVtermUnderline_Double:
            return PrDrawDecorationStyle_Double;
        case PrVtermUnderline_Curly:
            return PrDrawDecorationStyle_Wavy;
        case PrVtermUnderline_Dotted:
            return PrDrawDecorationStyle_Dotted;
        case PrVtermUnderline_Dashed:
            return PrDrawDecorationStyle_Dashed;
        default:
            return PrDrawDecorationStyle_Solid;
    }
}

// Records a decoration line across `columns` cells from `column`. `top` is the line's top edge and `thickness` its
// thickness, in px.
void decorate(const TerminalDrawer* drawer, PrDrawList* list, uint32_t column, uint32_t columns, float top,
        float thickness, PrDrawDecorationStyle style, PrColor color) {
    float cell = drawer->metrics.cell.x;
    PrDrawDecoration decoration{
        .origin = PrVec2{float(column) * cell, top + thickness / 2},
        .length = float(columns) * cell,
        .thickness = thickness,
        .style = style,
        .wavelength = cell,
        .amplitude = thickness,
        .phase = 0,
    };
    prDrawListDecoration(list, &decoration, paintOf(color));
}

} // namespace

TerminalColors terminalColors(const PrVterm* vterm) {
    TerminalColors colors{.reverse = (prVtermModes(vterm) & PrVtermModeFlag_ReverseVideo) != 0};
    for (uint32_t i = 0; i < kPrVtermPaletteCount; ++i)
        colors.palette[i] = prVtermPaletteColor(vterm, i);
    for (uint32_t role = 0; role <= PrVtermColorRole_Cursor; ++role)
        colors.roles[role] = prVtermRoleColor(vterm, PrVtermColorRole(role));
    return colors;
}

TerminalCellColors terminalCellColors(const TerminalDrawer* drawer, const TerminalColors& colors,
        const PrVtermCell& cell, bool selected) {
    PrVtermColorRole foregroundRole = PrVtermColorRole_Foreground;
    PrVtermColorRole backgroundRole = PrVtermColorRole_Background;
    if (colors.reverse) {
        foregroundRole = PrVtermColorRole_Background;
        backgroundRole = PrVtermColorRole_Foreground;
    }
    PrColor foreground = colorOf(colors, cell.foreground, foregroundRole);
    PrColor background = colorOf(colors, cell.background, backgroundRole);
    if ((cell.flags & PrVtermCellFlag_Inverse) != 0) {
        PrColor swapped = foreground;
        foreground = background;
        background = swapped;
    }
    if ((cell.flags & PrVtermCellFlag_Faint) != 0)
        foreground = mixed(foreground, background, 0.5f);
    if (selected)
        background = drawer->colors.selection;
    PrColor underline = foreground;
    if (cell.underlineColor.kind != PrVtermColorKind_Default)
        underline = colorOf(colors, cell.underlineColor, PrVtermColorRole_Foreground);
    if ((cell.flags & PrVtermCellFlag_Invisible) != 0)
        foreground = background;
    return TerminalCellColors{foreground, background, underline};
}

PrColor terminalBackground(const TerminalColors& colors) {
    return prColorFromColor8(colors.roles[colors.reverse ? PrVtermColorRole_Foreground : PrVtermColorRole_Background]);
}

bool terminalDrawRow(TerminalDrawer* drawer, PrDrawList* backgrounds, PrDrawList* list, const TerminalColors& colors,
        const PrVtermLine& line, uint32_t selectionStart, uint32_t selectionEnd, bool blinkHidden) {
    const PrTextGridMetrics& m = drawer->metrics;
    float cell = m.cell.x;
    PrColor screen = terminalBackground(colors);

    // The backgrounds that differ from the screen's, in runs of one color. Columns past the line's cells count as blank
    // cells, so that the selection shows there.
    uint32_t runStart = 0;
    PrColor runColor = screen;
    uint32_t shown = selectionEnd > line.cellCount ? selectionEnd : line.cellCount;
    for (uint32_t column = 0; column <= shown; ++column) {
        PrColor color = screen;
        if (column < shown) {
            bool selected = column >= selectionStart && column < selectionEnd;
            constexpr PrVtermCell kBlank{};
            const PrVtermCell& c = column < line.cellCount ? line.cells[column] : kBlank;
            color = terminalCellColors(drawer, colors, c, selected).background;
        }
        if (column != 0 && sameColor(color, runColor))
            continue;
        if (!sameColor(runColor, screen))
            fill(backgrounds, float(runStart) * cell, 0, float(column) * cell, m.cell.y, runColor);
        runStart = column;
        runColor = color;
    }

    // The glyphs. Those that terminalDrawShape handles draw as shapes, and every other cluster goes to the text grid. A
    // run of empty cells becomes one space in the grid's text, so that no ligature forms across it.
    drawer->text.clear();
    drawer->cells.clear();
    drawer->groups.clear();
    bool gap = false;
    bool blinking = false;
    for (uint32_t column = 0; column < line.cellCount; ++column) {
        const PrVtermCell& c = line.cells[column];
        bool selected = column >= selectionStart && column < selectionEnd;
        blinking = blinking || blinks(c);
        if (isEmpty(c) || (c.flags & PrVtermCellFlag_Invisible) != 0 || isBlinkHidden(c, blinkHidden)) {
            if (!gap && drawer->cells.count != 0 && (c.flags & PrVtermCellFlag_WideTail) == 0) {
                drawer->cells.push(PrTextGridCell{.start = drawer->text.count, .column = column, .columns = 1});
                drawer->text.push(' ');
                gap = true;
            }
            continue;
        }
        gap = false;
        TerminalCellColors cellColors = terminalCellColors(drawer, colors, c, selected);
        uint32_t columns = (c.flags & PrVtermCellFlag_Wide) != 0 ? 2 : 1;
        PrRect rect{float(column) * cell, 0, float(column + columns) * cell, m.cell.y};
        if (c.clusterCount == 0 && terminalDrawShape(drawer, list, c.codePoint, rect, cellColors.foreground))
            continue;
        drawer->cells.push(PrTextGridCell{
            .start = drawer->text.count,
            .column = column,
            .columns = columns,
            .font = fontOf(c),
            .group = groupOf(drawer, cellColors.foreground),
        });
        appendCluster(drawer, line, c);
    }
    if (drawer->cells.count != 0) {
        PrTextGridDesc desc{
            .text = PrStringView{drawer->text.data, drawer->text.count},
            .cells = drawer->cells.data,
            .cellCount = drawer->cells.count,
            .fonts = drawer->fonts,
            .fontCount = kTerminalFontCount,
            .shaper = drawer->shaper,
            .pixelRatio = drawer->pixelRatio,
            .cellWidth = cell,
            .baseline = m.baseline,
        };
        prTextGridSet(drawer->grid, &desc);
        for (uint32_t r = 0; r < prTextGridRunCount(drawer->grid); ++r) {
            PrFontGlyphRun run = prTextGridRun(drawer->grid, r);
            prDrawListGlyphs(list, &run, PrVec2{0, 0}, paintOf(drawer->groups.data[prTextGridRunGroup(drawer->grid, r)]));
        }
    }

    // Strikethrough and overline, in runs of cells with the same flag.
    struct Decoration {
        PrVtermCellFlags flag;
        float top;
        float thickness;
    };
    const Decoration kLines[] = {
        {PrVtermCellFlag_Strikethrough, m.strikeoutTop, m.strikeoutThickness},
        {PrVtermCellFlag_Overline, 0, m.underlineThickness},
    };
    for (const Decoration& decoration : kLines) {
        for (uint32_t column = 0; column < line.cellCount;) {
            const PrVtermCell& c = line.cells[column];
            if ((c.flags & decoration.flag) == 0 || isBlinkHidden(c, blinkHidden)) {
                ++column;
                continue;
            }
            bool selected = column >= selectionStart && column < selectionEnd;
            PrColor color = terminalCellColors(drawer, colors, c, selected).foreground;
            uint32_t end = column + 1;
            while (end < line.cellCount && (line.cells[end].flags & decoration.flag) != 0 &&
                    !isBlinkHidden(line.cells[end], blinkHidden))
                ++end;
            decorate(drawer, list, column, end - column, decoration.top, decoration.thickness,
                PrDrawDecorationStyle_Solid, color);
            column = end;
        }
    }
    // The underlines, in runs of the same style and color. A cell's own underline wins. Otherwise, a hyperlink gets a
    // fainter dotted underline, so that it reads as a link without looking like an underline that the program drew.
    auto underlineAt = [&](uint32_t column, PrColor* color) -> PrVtermUnderline {
        const PrVtermCell& c = line.cells[column];
        bool linked = c.underline == PrVtermUnderline_None && c.link != kPrVtermLinkNone;
        if ((c.underline == PrVtermUnderline_None && !linked) || isBlinkHidden(c, blinkHidden))
            return PrVtermUnderline_None;
        bool selected = column >= selectionStart && column < selectionEnd;
        TerminalCellColors cellColors = terminalCellColors(drawer, colors, c, selected);
        if (!linked) {
            *color = cellColors.underline;
            return PrVtermUnderline(c.underline);
        }
        *color = mixed(cellColors.underline, cellColors.background, 0.5f);
        return PrVtermUnderline_Dotted;
    };
    for (uint32_t column = 0; column < line.cellCount;) {
        PrColor color{};
        PrVtermUnderline underline = underlineAt(column, &color);
        if (underline == PrVtermUnderline_None) {
            ++column;
            continue;
        }
        uint32_t end = column + 1;
        PrColor next{};
        while (end < line.cellCount && underlineAt(end, &next) == underline && sameColor(next, color))
            ++end;
        decorate(drawer, list, column, end - column, m.underlineTop, m.underlineThickness, decorationStyleOf(underline), color);
        column = end;
    }
    return blinking;
}

void terminalDrawCell(TerminalDrawer* drawer, PrDrawList* list, const PrVtermLine& line, uint32_t column, PrVec2 origin,
        const TerminalCellColors& colors) {
    if (column >= line.cellCount)
        return;
    const PrVtermCell& c = line.cells[column];
    if (isEmpty(c) || (c.flags & PrVtermCellFlag_Invisible) != 0)
        return;
    const PrTextGridMetrics& m = drawer->metrics;
    uint32_t columns = (c.flags & PrVtermCellFlag_Wide) != 0 ? 2 : 1;
    PrRect rect{origin.x, origin.y, origin.x + float(columns) * m.cell.x, origin.y + m.cell.y};
    if (c.clusterCount == 0 && terminalDrawShape(drawer, list, c.codePoint, rect, colors.foreground))
        return;
    drawer->text.clear();
    appendCluster(drawer, line, c);
    PrTextGridCell cell{.columns = columns, .font = fontOf(c)};
    PrTextGridDesc desc{
        .text = PrStringView{drawer->text.data, drawer->text.count},
        .cells = &cell,
        .cellCount = 1,
        .fonts = drawer->fonts,
        .fontCount = kTerminalFontCount,
        .shaper = drawer->shaper,
        .pixelRatio = drawer->pixelRatio,
        .cellWidth = m.cell.x,
        .baseline = m.baseline,
    };
    prTextGridSet(drawer->grid, &desc);
    for (uint32_t r = 0; r < prTextGridRunCount(drawer->grid); ++r) {
        PrFontGlyphRun run = prTextGridRun(drawer->grid, r);
        prDrawListGlyphs(list, &run, origin, paintOf(colors.foreground));
    }
}

bool terminalDrawShape(const TerminalDrawer* drawer, PrDrawList* list, uint32_t codePoint, PrRect rect, PrColor color) {
    if (codePoint >= 0x256d && codePoint <= 0x2570) {
        drawArc(drawer, list, codePoint, rect, color);
        return true;
    }
    if (codePoint >= 0x2500 && codePoint <= 0x257f) {
        uint8_t value = kBoxArms[codePoint - 0x2500];
        if (value == 0)
            return false;
        drawBox(drawer, list, value, rect, color);
        return true;
    }
    if (codePoint >= 0x2580 && codePoint <= 0x259f)
        return drawBlock(drawer, list, codePoint, rect, color);
    if (codePoint >= 0x2800 && codePoint <= 0x28ff) {
        drawBraille(drawer, list, codePoint, rect, color);
        return true;
    }
    return drawPowerline(drawer, list, codePoint, rect, color);
}

} // namespace pr_impl

// Workloads: dense drawings that each stress one route through a renderer, for measuring it. The last one mixes them as
// a user interface does, interleaving work that batches separately.

#include "examples/draw_features/workloads.h"

#include "pr/string.h"

#include "examples/draw_features/drawing.h"

namespace drawings {

using namespace pr;

namespace {

// Returns a color from a small palette, by index.
PrColor paletteColor(uint32_t i) {
    static const PrColor colors[] = {
        {.95f, .45f, .35f, 1},
        {.30f, .62f, .92f, 1},
        {.40f, .78f, .48f, 1},
        {.96f, .76f, .25f, 1},
        {.66f, .48f, .90f, 1},
        {.25f, .75f, .78f, 1},
        {.92f, .48f, .70f, 1},
        {.55f, .58f, .62f, 1},
    };
    return colors[i % 8];
}

TextStyle uiText(float size, PrColor tint = kInk) {
    TextStyle style;
    style.font = fontNamed("NotoSans.ttf");
    style.size = size;
    style.paint.color = tint;
    return style;
}

// Calls `each` for the cells of `size` with `gap` between them, over the area, with each cell's rectangle, index,
// column and row.
template <typename Each>
void cells(PrRect area, PrVec2 size, float gap, Each each) {
    uint32_t columns = uint32_t((prRectWidth(area) + gap) / (size.x + gap));
    uint32_t rows = uint32_t((prRectHeight(area) + gap) / (size.y + gap));
    for (uint32_t r = 0; r < rows; ++r) {
        for (uint32_t c = 0; c < columns; ++c) {
            float x = area.left + float(c) * (size.x + gap);
            float y = area.top + float(r) * (size.y + gap);
            each(prRectAt(PrVec2{x, y}, size), r * columns + c, c, r);
        }
    }
}

constexpr PrRect kArea{20, 20, 1080, 780};

// ---------------------------------------------------------------------------------------------------------------------
// Workloads

void solidRects(PrDrawList* list, float, float, double) {
    cells(kArea, PrVec2{24, 16}, 2, [&](PrRect r, uint32_t i, uint32_t, uint32_t) {
        prDrawListRect(list, r, {.color = paletteColor(i)});
    });
}

void fractionalRects(PrDrawList* list, float, float, double) {
    cells(kArea, PrVec2{24, 16}, 2.3f, [&](PrRect r, uint32_t i, uint32_t, uint32_t) {
        float dx = 0.37f;
        float dy = 0.63f;
        prDrawListRect(list, PrRect{r.left + dx, r.top + dy, r.right + dx, r.bottom + dy}, {.color = paletteColor(i)});
    });
}

void roundedRects(PrDrawList* list, float, float, double) {
    const float radii[] = {2, 4, 6, 10, 14};
    cells(kArea, PrVec2{40, 26}, 4, [&](PrRect r, uint32_t i, uint32_t c, uint32_t) {
        PrCornerRect rr = prCornerRectRound(r, radii[c % 5]);
        if (i % 7 == 3) {
            // A tab: rounded on top only.
            rr.corners[2] = rr.corners[3] = PrCorner{};
        } else if (i % 7 == 5) {
            // Elliptical corners.
            for (PrCorner& corner : rr.corners)
                corner = prCornerRound(PrVec2{12, 5});
        }
        prDrawListCornerRect(list, rr, {.color = paletteColor(i)});
    });
}

void strokedRects(PrDrawList* list, float, float, double) {
    const float widths[] = {1, 1.5f, 2, 3, 4};
    cells(kArea, PrVec2{40, 26}, 6, [&](PrRect r, uint32_t i, uint32_t c, uint32_t row) {
        PrDrawStroke stroke = strokeOf(widths[c % 5]);
        if (row % 2 == 0)
            prDrawListRect(list, r, {.color = paletteColor(i), .stroke = &stroke});
        else
            prDrawListCornerRect(list, prCornerRectRound(r, 6), {.color = paletteColor(i), .stroke = &stroke});
    });
}

void borders(PrDrawList* list, float, float, double) {
    cells(kArea, PrVec2{64, 40}, 8, [&](PrRect r, uint32_t i, uint32_t c, uint32_t) {
        PrDrawBorder border{};
        float radius = c % 3 == 0 ? 0 : c % 3 == 1 ? 4 : 10;
        for (PrCorner& corner : border.shape.corners)
            corner = PrCorner{PrVec2{radius, radius}, 1};
        for (uint32_t side = 0; side < 4; ++side) {
            // Most borders are one width and color. Some differ by side, and a few are dashed.
            bool perSide = i % 5 == 4;
            PrDrawBorderStyle style = i % 11 == 10 ? PrDrawBorderStyle_Dashed : PrDrawBorderStyle_Solid;
            border.sides[side] = PrDrawBorderSide{
                perSide ? float(1 + side) : 1.5f,
                style,
                perSide ? paletteColor(i + side) : paletteColor(i), nullptr, 0,
            };
        }
        border.shape.rect = r;
        prDrawListBorder(list, &border, {});
    });
}

void hairlineGrid(PrDrawList* list, float, float, double) {
    PrDrawStroke hairline = strokeOf(1);
    const float spacing = 4;
    for (float x = kArea.left; x <= kArea.right; x += spacing)
        prDrawListLine(list, PrVec2{x + .5f, kArea.top}, PrVec2{x + .5f, kArea.bottom},
            {.color = withAlpha(kInk, .35f), .stroke = &hairline});
    for (float y = kArea.top; y <= kArea.bottom; y += spacing)
        prDrawListLine(list, PrVec2{kArea.left, y + .5f}, PrVec2{kArea.right, y + .5f},
            {.color = withAlpha(kInk, .35f), .stroke = &hairline});
}

void angledLines(PrDrawList* list, float, float, double time) {
    PrDrawStroke stroke = strokeOf(2.5f, PrPathJoin_Round, PrPathCap_Round);
    cells(kArea, PrVec2{26, 24}, 0, [&](PrRect r, uint32_t, uint32_t c, uint32_t row) {
        float a = float(time) * .6f + float(c) * .18f + float(row) * .12f;
        PrVec2 center = prRectCenter(r);
        PrVec2 d{prF32Cos(a) * 10, prF32Sin(a) * 10};
        PrColor color{(180 + float(row * 4 % 70)) / 255, 120.f / 255, (220 - float(c * 6 % 100)) / 255, 1};
        prDrawListLine(list, center - d, center + d, {.color = color, .stroke = &stroke});
    });
}

void capsules(PrDrawList* list, float, float, double time) {
    cells(kArea, PrVec2{26, 24}, 0, [&](PrRect r, uint32_t i, uint32_t c, uint32_t row) {
        float a = float(time) * .6f + float(c) * .18f + float(row) * .12f;
        PrVec2 center = prRectCenter(r);
        PrVec2 d{prF32Cos(a) * 8, prF32Sin(a) * 8};
        prDrawListCapsule(list, center - d, center + d, 1.5f + float(i % 4), {.color = paletteColor(i)});
    });
}

void polylines(PrDrawList* list, float, float, double time) {
    PrDrawStroke stroke = strokeOf(3, PrPathJoin_Round, PrPathCap_Round);
    Array<PrVec2> points;
    for (uint32_t k = 0; k < 24; ++k) {
        points.clear();
        float y = kArea.top + 16 + float(k) * 31;
        for (uint32_t i = 0; i <= 120; ++i) {
            float x = kArea.left + float(i) * 8.8f;
            points.push(PrVec2{x, y + 10 * prF32Sin(float(i) * .35f + float(k) + float(time))});
        }
        prDrawListPolylineContour(list, points.data(), uint32_t(points.count()), PrDrawContour_Open,
            PrPathFillRule_NonZero, {.color = paletteColor(k), .stroke = &stroke});
    }
}

// Draws code in a monospaced font, as an editor shows it.
void denseText(PrDrawList* list, float, float, double) {
    static const char* const lines[] = {
        "void layoutBlock(LayoutBox* box, const Constraints& constraints) {",
        "    float y = box->padding.top + box->border.top;",
        "    for (LayoutBox* child = box->first; child != nullptr; child = child->next) {",
        "        child->position.y = y + child->margin.top;",
        "        layoutBlock(child, constraintsFor(box, child));",
        "        y += child->size.height + child->margin.top + child->margin.bottom;",
        "    }",
        "    box->size.height = y + box->padding.bottom + box->border.bottom;",
        "}",
        "",
    };
    TextStyle style = uiText(12);
    style.font = fontNamed("Cousine.ttf");
    const PrColor colors[] = {kInk, kBlue, kPurple, kGreen, kGray};
    float y = kArea.top + 12;
    for (uint32_t i = 0; y < kArea.bottom; ++i, y += 15) {
        String number = format("{:4}", i + 1);
        TextStyle gutter = style;
        gutter.paint.color = kGray;
        drawText(list, gutter, number, PrVec2{kArea.left, y});
        style.paint.color = colors[i % 5];
        drawText(list, style, lines[i % 10], PrVec2{kArea.left + 44, y});
        drawText(list, style, lines[(i + 3) % 10], PrVec2{kArea.left + 560, y});
    }
}

// A run of cells in a terminal's row, in one pair of colors. A run whose background is the screen's draws no
// background.
struct TerminalRun {
    String text;
    PrColor foreground;
    PrColor background;
};

// Returns the runs of one row of a process monitor's frame, on a terminal `columns` cells wide. The frame has meters, a
// header, a list of processes with one selected, and a bar of function keys.
Array<TerminalRun> processMonitorRow(uint32_t row, uint32_t rows, uint32_t columns, PrColor screen) {
    const PrColor text{.85f, .85f, .82f, 1};
    const PrColor black{0, 0, 0, 1};
    const PrColor cyan{.30f, .75f, .80f, 1};
    const PrColor green{.40f, .78f, .40f, 1};
    const PrColor yellow{.90f, .78f, .30f, 1};
    const PrColor red{.85f, .35f, .30f, 1};
    static const char* const commands[] = {
        "/usr/libexec/trustd --agent",
        "/System/Library/CoreServices/Finder.app/Contents/MacOS/Finder",
        "node /usr/local/lib/node_modules/npm/bin/npm-cli.js run build",
        "/Applications/Xcode.app/Contents/MacOS/Xcode",
        "python3 -m http.server 8000",
        "/usr/sbin/cfprefsd agent",
        "zsh -l",
        "ninja -C build -j 16",
    };
    // Pads `s`, which starts at column `from`, with spaces to the end of the row.
    auto toEnd = [&](String s, uint32_t from) {
        while (from + s.count() < columns)
            s.append(" ");
        return s;
    };
    Array<TerminalRun> runs;
    if (row < 4) {
        uint32_t bars = 10 + row * 13 % 37;
        String meter;
        for (uint32_t i = 0; i < 40; ++i)
            meter.append(i < bars ? "|" : " ");
        runs.push({format("{:3}[", row), text, screen});
        runs.push({meter, green, screen});
        runs.push({"]   Tasks: 412, 1873 thr; 3 running   Load average: 2.31 1.94 1.70", text, screen});
    } else if (row == 5) {
        runs.push({toEnd("  PID USER      PRI  NI  VIRT   RES   SHR S CPU% MEM%   TIME+  Command", 0), black, green});
    } else if (row + 1 == rows) {
        const char* keys[] = {"Help", "Setup", "Search", "Filter", "Tree", "SortBy", "Nice -", "Nice +", "Kill", "Quit"};
        for (uint32_t i = 0; i < 10; ++i) {
            runs.push({format("F{}", i + 1), text, screen});
            runs.push({format("{:6}", keys[i]), black, cyan});
        }
    } else if (row > 5) {
        uint32_t process = row - 6;
        bool selected = process == 7;
        PrColor back = selected ? cyan : screen;
        runs.push({format("{:5} pdoane     17   0 ", 300 + process * 37), selected ? black : text, back});
        runs.push({format("{:4}G {:4}M ", 30 + process % 9, 120 + process * 11), selected ? black : yellow, back});
        runs.push({
            format("{:4}M S {:4.1f} {:4.1f}  {:2}:{:02}.{:02}  ", 40 + process * 3, float(process % 7) * 3.7f,
                float(process % 4) * 1.3f, process % 60, process * 7 % 60, process * 13 % 100),
            selected ? black : process % 5 == 0 ? red : text, back,
        });
        runs.push({commands[process % 8], selected ? black : process % 3 == 0 ? cyan : text, back});
        if (selected)
            runs.push({toEnd("", 0), black, back});
    }
    return runs;
}

// The background color of the terminal's screen. The window's pass clears to it.
constexpr PrColor kTerminalScreen{.11f, .11f, .13f, 1};

// Draws a process monitor's frame on a terminal of 120 by 40 cells, as a terminal emulator draws it. Each row embeds
// two lists at its position: first the backgrounds that differ from the screen's, then the glyphs, each in runs of one
// color. Nothing paints the screen's background, because the window's pass clears to it.
void terminal(PrDrawList* list, float, float, double) {
    constexpr uint32_t kColumns = 120;
    constexpr uint32_t kRows = 40;
    constexpr float kLine = 17;
    constexpr float kBaseline = 13;
    const PrColor screen = kTerminalScreen;
    TextStyle style = uiText(13);
    style.font = fontNamed("Cousine.ttf");
    // A terminal's cells are whole pixels wide.
    float cell = prF32Ceil(textAdvance(style, "M"));
    PrRect area{kArea.left, kArea.top, kArea.left + cell * kColumns, kArea.top + kLine * kRows};
    auto sameColor = [](PrColor a, PrColor b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; };
    for (uint32_t row = 0; row < kRows; ++row) {
        PrDrawList* backgrounds = prDrawListCreate();
        PrDrawList* glyphs = prDrawListCreate();
        uint32_t column = 0;
        // The background run in progress, from `start`. The next cells extend it if their background matches.
        uint32_t start = 0;
        PrColor background = screen;
        for (const TerminalRun& run : processMonitorRow(row, kRows, kColumns, screen)) {
            uint32_t end = column + run.text.count();
            end = end < kColumns ? end : kColumns;
            if (!sameColor(run.background, background)) {
                if (!sameColor(background, screen))
                    prDrawListRect(backgrounds, PrRect{float(start) * cell, 0, float(column) * cell, kLine},
                        {.color = background});
                start = column;
                background = run.background;
            }
            style.paint.color = run.foreground;
            PrStringView text = run.text.view();
            drawText(glyphs, style, PrStringView{text.data, end - column}, PrVec2{float(column) * cell, kBaseline});
            column = end;
        }
        if (!sameColor(background, screen))
            prDrawListRect(backgrounds, PrRect{float(start) * cell, 0, float(column) * cell, kLine},
                {.color = background});
        PrDVec2 at{double(area.left), double(area.top) + double(row) * double(kLine)};
        prDrawListEmbed(list, backgrounds, at);
        prDrawListEmbed(list, glyphs, at);
        prDrawListRelease(backgrounds);
        prDrawListRelease(glyphs);
    }
}

// Draws rows of a list in a scroll view: each row clipped, with a label and a value.
void clippedCells(PrDrawList* list, float, float, double time) {
    float scroll = prF32Fmod(float(time) * 20, 26.0f);
    for (uint32_t column = 0; column < 4; ++column) {
        PrRect view{kArea.left + float(column) * 266, kArea.top, kArea.left + float(column) * 266 + 256, kArea.bottom};
        prDrawListRect(list, view, {.color = kWhite});
        prDrawListPushClipRect(list, view, PrDrawEdges_Antialiased);
        for (uint32_t row = 0; row < 31; ++row) {
            float y = view.top + float(row) * 26 - scroll;
            PrRect cell{view.left, y, view.right, y + 25};
            prDrawListPushClipRect(list, cell, PrDrawEdges_Antialiased);
            prDrawListRect(list, cell, {.color = row % 2 ? PrColor{.97f, .97f, .98f, 1} : kWhite});
            drawText(list, uiText(13), "Setting with a long descriptive name", PrVec2{cell.left + 28, y + 17});
            PrRect box{cell.left + 8, y + 7, cell.left + 20, y + 19};
            prDrawListCornerRect(list, prCornerRectRound(box, 3), {.color = paletteColor(row + column)});
            prDrawListPopClip(list);
        }
        prDrawListPopClip(list);
    }
}

// Draws cards clipped to their rounded corners, with contents that reach past them.
void roundedClips(PrDrawList* list, float, float, double) {
    cells(kArea, PrVec2{120, 80}, 12, [&](PrRect r, uint32_t i, uint32_t, uint32_t) {
        prDrawListPushClipCornerRect(list, prCornerRectRound(r, 12), PrDrawEdges_Antialiased);
        prDrawListRect(list, r, {.color = kWhite});
        prDrawListRect(list, PrRect{r.left, r.top, r.right, r.top + 36}, {.color = paletteColor(i)});
        drawText(list, uiText(12), "Card title", PrVec2{r.left + 10, r.top + 58});
        prDrawListPopClip(list);
    });
}

void shadows(PrDrawList* list, float, float, double) {
    cells(PrRect{40, 40, 1060, 760}, PrVec2{96, 64}, 28, [&](PrRect r, uint32_t i, uint32_t, uint32_t) {
        PrDrawShadow shadow{};
        shadow.shape.rect = r;
        for (PrCorner& corner : shadow.shape.corners)
            corner = PrCorner{PrVec2{8, 8}, 1};
        shadow.offset = PrVec2{0, 4};
        shadow.sigma = 3 + float(i % 4) * 2;
        prDrawListDropShadow(list, &shadow, {.color = withAlpha(kInk, .3f)});
        prDrawListCornerRect(list, prCornerRectRound(r, 8), {.color = kWhite});
        if (i % 3 == 0) {
            shadow.offset = PrVec2{0, 1};
            shadow.sigma = 1.5f;
            prDrawListInnerShadow(list, &shadow, {.color = withAlpha(kInk, .2f)});
        }
    });
}

// Returns a small icon as a path in a 16 by 16 box.
PrPath* iconPath(uint32_t which) {
    PrPath* path = prPathCreate();
    switch (which % 3) {
        case 0: {
            // A gear-like star.
            PrPath* shape = star(PrVec2{8, 8}, 7, 8, 3);
            prPathAddPath(path, shape, prAffine2Identity());
            prPathDestroy(shape);
            break;
        }
        case 1:
            // A document with a folded corner.
            prPathMoveTo(path, PrVec2{3, 1});
            prPathLineTo(path, PrVec2{10, 1});
            prPathLineTo(path, PrVec2{13, 4});
            prPathLineTo(path, PrVec2{13, 15});
            prPathLineTo(path, PrVec2{3, 15});
            prPathClose(path);
            break;
        default:
            // A speech bubble.
            prPathMoveTo(path, PrVec2{2, 3});
            prPathQuadTo(path, PrVec2{2, 1}, PrVec2{4, 1});
            prPathLineTo(path, PrVec2{12, 1});
            prPathQuadTo(path, PrVec2{14, 1}, PrVec2{14, 3});
            prPathLineTo(path, PrVec2{14, 9});
            prPathQuadTo(path, PrVec2{14, 11}, PrVec2{12, 11});
            prPathLineTo(path, PrVec2{7, 11});
            prPathLineTo(path, PrVec2{4, 14});
            prPathLineTo(path, PrVec2{4, 11});
            prPathQuadTo(path, PrVec2{2, 11}, PrVec2{2, 9});
            prPathClose(path);
            break;
    }
    return path;
}

// Draws icons: small images and small paths, as a toolbar or a file list shows them.
void icons(PrDrawList* list, float, float, double) {
    PrPath* paths[3] = {iconPath(0), iconPath(1), iconPath(2)};
    cells(kArea, PrVec2{20, 20}, 6, [&](PrRect r, uint32_t i, uint32_t, uint32_t) {
        if (i % 2 == 0) {
            drawImage(list, testCard(), PrRect{0, 0, 64, 64}, r, PrDrawSampling_Mipmap, 1);
            return;
        }
        prDrawListPushTransformAffine2(list, PrAffine2{1.25f, 0, 0, 1.25f, r.left, r.top});
        prDrawListPath(list, paths[i / 2 % 3], PrPathFillRule_NonZero, {.color = paletteColor(i)});
        prDrawListPopTransform(list);
    });
    for (PrPath* path : paths)
        prPathDestroy(path);
}

// Draws elements faded by opacity groups, as a transition shows them.
void opacityGroups(PrDrawList* list, float, float, double time) {
    cells(kArea, PrVec2{80, 40}, 8, [&](PrRect r, uint32_t i, uint32_t, uint32_t) {
        PrDrawGroup group = prDrawGroupDefault();
        group.bounds = r;
        group.opacity = .5f + .45f * prF32Sin(float(time) + float(i) * .3f);
        prDrawListPushGroup(list, &group);
        prDrawListCornerRect(list, prCornerRectRound(r, 6), {.color = paletteColor(i)});
        drawText(list, uiText(12, kWhite), "Button", PrVec2{r.left + 20, r.top + 25});
        prDrawListPopGroup(list);
    });
}

void gradients(PrDrawList* list, float, float, double) {
    cells(kArea, PrVec2{80, 32}, 8, [&](PrRect r, uint32_t i, uint32_t, uint32_t) {
        PrDrawColorStop stops[2] = {
            {.offset = 0, .color = paletteColor(i)},
            {.offset = 1, .color = paletteColor(i + 3)},
        };
        PrDrawBrush brush = prDrawBrushLinear(PrVec2{r.left, r.top}, PrVec2{r.left, r.bottom}, stops, 2);
        if (i % 4 == 3) {
            brush = prDrawBrushRadial(prRectCenter(r), 0, prRectCenter(r), 40, stops, 2);
        }
        prDrawListCornerRect(list, prCornerRectRound(r, 8), {.brush = &brush});
    });
}

// Draws a settings window: a sidebar, a toolbar, cards with shadows and borders, labels, toggles and icons, in the
// order that a tree of views paints them.
void interface(PrDrawList* list, float, float, double time) {
    PrPath* glyphs[3] = {iconPath(0), iconPath(1), iconPath(2)};
    PrDrawStroke rule = strokeOf(1);
    prDrawListRect(list, PrRect{0, 0, 1100, 800}, {.color = PrColor{.95f, .95f, .96f, 1}});
    // Sidebar.
    prDrawListRect(list, PrRect{0, 0, 220, 800}, {.color = PrColor{.90f, .91f, .93f, 1}});
    prDrawListLine(list, PrVec2{220.5f, 0}, PrVec2{220.5f, 800}, {.color = withAlpha(kInk, .15f), .stroke = &rule});
    for (uint32_t i = 0; i < 22; ++i) {
        float y = 60 + float(i) * 30;
        if (i == 4) {
            PrRect highlight{8, y - 20, 212, y + 8};
            prDrawListCornerRect(list, prCornerRectRound(highlight, 6), {.color = withAlpha(kBlue, .2f)});
        }
        prDrawListPushTransformAffine2(list, PrAffine2{1, 0, 0, 1, 18, y - 14});
        prDrawListPath(list, glyphs[i % 3], PrPathFillRule_NonZero, {.color = paletteColor(i)});
        prDrawListPopTransform(list);
        drawText(list, uiText(13), i % 2 ? "Appearance" : "Notifications", PrVec2{44, y});
    }
    // Toolbar.
    prDrawListRect(list, PrRect{220, 0, 1100, 48}, {.color = kWhite});
    prDrawListLine(list, PrVec2{220, 48.5f}, PrVec2{1100, 48.5f}, {.color = withAlpha(kInk, .15f), .stroke = &rule});
    drawText(list, uiText(17), "Settings", PrVec2{244, 31});
    PrDrawBorder field{};
    for (PrCorner& corner : field.shape.corners)
        corner = PrCorner{PrVec2{6, 6}, 1};
    for (PrDrawBorderSide& side : field.sides)
        side = PrDrawBorderSide{1, PrDrawBorderStyle_Solid, withAlpha(kInk, .25f), nullptr, 0};
    prDrawListCornerRect(list, prCornerRectRound(PrRect{820, 10, 1080, 38}, 6), {.color = kWhite});
    field.shape.rect = PrRect{820, 10, 1080, 38};
    prDrawListBorder(list, &field, {});
    drawText(list, uiText(13, kGray), "Search", PrVec2{836, 29});
    // Cards in a scrolling pane.
    float scroll = prF32Fmod(float(time) * 30, 120.0f);
    prDrawListPushClipRect(list, PrRect{220, 49, 1100, 800}, PrDrawEdges_Antialiased);
    for (uint32_t card = 0; card < 8; ++card) {
        float x = card % 2 == 0 ? 244 : 668;
        float y = 72 + float(card / 2) * 190 - scroll;
        PrRect r{x, y, x + 408, y + 170};
        PrDrawShadow shadow{};
        shadow.shape.rect = r;
        for (PrCorner& corner : shadow.shape.corners)
            corner = PrCorner{PrVec2{10, 10}, 1};
        shadow.offset = PrVec2{0, 2};
        shadow.sigma = 4;
        prDrawListDropShadow(list, &shadow, {.color = withAlpha(kInk, .18f)});
        prDrawListCornerRect(list, prCornerRectRound(r, 10), {.color = kWhite});
        drawText(list, uiText(15), "Section heading", PrVec2{r.left + 16, r.top + 28});
        for (uint32_t row = 0; row < 4; ++row) {
            float ry = r.top + 44 + float(row) * 30;
            prDrawListLine(list, PrVec2{r.left + 16, ry + .5f}, PrVec2{r.right - 16, ry + .5f},
                {.color = withAlpha(kInk, .08f), .stroke = &rule});
            drawText(list, uiText(13), "Option label", PrVec2{r.left + 16, ry + 20});
            drawText(list, uiText(12, kGray), "Detail", PrVec2{r.left + 200, ry + 20});
            // A toggle.
            bool on = (card + row) % 3 != 0;
            PrRect track{r.right - 56, ry + 6, r.right - 16, ry + 26};
            prDrawListCornerRect(list, prCornerRectRound(track, 10), {.color = on ? kGreen : PrColor{.8f, .8f, .82f, 1}});
            float knob = on ? track.right - 10 : track.left + 10;
            prDrawListEllipse(list, PrVec2{knob, ry + 16}, PrVec2{8, 8}, 0, {.color = kWhite});
        }
    }
    prDrawListPopClip(list);
    for (PrPath* path : glyphs)
        prPathDestroy(path);
}

} // namespace

const Array<Drawing>& workloadDrawings() {
    static const Array<Drawing> workloads({
        {"Solid rects", "Pixel-aligned rectangles, as panels and backgrounds.", solidRects},
        {"Fractional rects", "Rectangles off the pixel grid on every edge.", fractionalRects},
        {"Rounded rects", "Rounded rectangles of every radius, tabs and elliptical corners.", roundedRects},
        {"Stroked rects", "Rectangles and rounded rectangles stroked at several widths.", strokedRects},
        {"Borders", "Borders of one color, of a color per side, and dashed.", borders},
        {"Hairline grid", "One-pixel lines four pixels apart, as a table's rules.", hairlineGrid},
        {"Angled lines", "Short round-capped lines at every angle.", angledLines},
        {"Capsules", "Capsules at every angle and a few radii.", capsules},
        {"Polylines", "Long polylines with round joins.", polylines},
        {"Dense text", "Code in a monospaced font, a screen of it.", denseText},
        {"Clipped cells", "Rows of scroll views, each row clipped.", clippedCells},
        {"Rounded clips", "Cards clipped to their rounded corners.", roundedClips},
        {"Shadows", "Cards with drop shadows, some with inner shadows.", shadows},
        {"Icons", "Small images and small paths, alternating.", icons},
        {"Opacity groups", "Buttons faded by opacity groups.", opacityGroups},
        {"Gradients", "Buttons filled with linear and radial gradients.", gradients},
        {"Interface", "A settings window, drawn as a tree of views paints it.", interface},
        {
            "Terminal", "A process monitor's frame on a terminal of 120 by 40 cells, its rows embedded.", terminal,
            kTerminalScreen,
        },
    });
    return workloads;
}

} // namespace drawings

// Sixel images: the decoder of the data, which draws into a canvas that grows as the sixels need, and the image's
// placement.

#include "vterm/src/vterm_sixel.h"

#include "pr/color.h"
#include "pr/color_space.h"
#include "pr/float.h"
#include "pr/mem.h"

#include "vterm/src/vterm_graphics.h"
#include "vterm/src/vterm_parser.h"
#include "vterm/src/vterm_screen.h"

namespace pr_impl {

namespace {

// The largest image, in pixels along either axis, as kitty's protocol allows.
constexpr uint32_t kMaxSide = 10000;

// The VT340's colors for the first 16 registers, as percentages of red, green and blue.
constexpr uint8_t kDefaultColors[16][3] = {
    {0, 0, 0}, {20, 20, 80}, {80, 13, 13}, {20, 80, 20}, {80, 20, 80}, {20, 80, 80}, {80, 80, 20}, {53, 53, 53},
    {26, 26, 26}, {33, 33, 60}, {60, 26, 26}, {33, 60, 33}, {60, 33, 60}, {33, 60, 60}, {60, 60, 33}, {80, 80, 80},
};

uint8_t percentChannel(uint32_t percent) {
    return uint8_t((percent > 100 ? 100 : percent) * 255 / 100);
}

// Returns the color of hue `h` in degrees, lightness `l` and saturation `s` in percentages. The sixel hue puts blue at
// 0 degrees, red at 120 and green at 240, where HSL puts red at 0.
PrColor8 hlsColor(uint32_t h, uint32_t l, uint32_t s) {
    double lightness = l > 100 ? 100 : l;
    double saturation = s > 100 ? 100 : s;
    return prColor8FromColor(prColorFromHsl(double((h + 240) % 360), saturation, lightness, 1));
}

// The pixels that the sixels draw, Rgba8 with straight alpha. A pixel that no sixel sets has zero alpha. The canvas
// grows as the sixels need, up to kMaxSide pixels along either axis and the terminal's budget.
struct SixelCanvas {
    RawArray<uint8_t> pixels;
    uint32_t capacityWidth;     // the pixels' width and height
    uint32_t capacityHeight;
    uint32_t width;             // the extent that the sixels set
    uint32_t height;
    uint64_t budget;
    bool full;                  // the canvas could not grow, and the sixels past it are dropped
};

// Grows the canvas to hold at least `width` by `height` pixels. Returns false if it cannot.
bool canvasReserve(SixelCanvas* canvas, uint32_t width, uint32_t height) {
    if (width <= canvas->capacityWidth && height <= canvas->capacityHeight)
        return true;
    if (canvas->full || width > kMaxSide || height > kMaxSide) {
        canvas->full = true;
        return false;
    }
    uint32_t newWidth = canvas->capacityWidth;
    uint32_t newHeight = canvas->capacityHeight;
    while (newWidth < width)
        newWidth = newWidth == 0 ? 64 : newWidth * 2;
    while (newHeight < height)
        newHeight = newHeight == 0 ? 64 : newHeight * 2;
    newWidth = newWidth > kMaxSide ? kMaxSide : newWidth;
    newHeight = newHeight > kMaxSide ? kMaxSide : newHeight;
    if (uint64_t(newWidth) * newHeight * 4 > canvas->budget) {
        canvas->full = true;
        return false;
    }
    RawArray<uint8_t> grown;
    grown.resize(newWidth * newHeight * 4);
    prMemZero(grown.data, grown.count);
    for (uint32_t y = 0; y < canvas->capacityHeight; ++y) {
        prMemCopy(grown.data + size_t(y) * newWidth * 4, canvas->pixels.data + size_t(y) * canvas->capacityWidth * 4,
            size_t(canvas->capacityWidth) * 4);
    }
    canvas->pixels.reset();
    canvas->pixels = grown;
    canvas->capacityWidth = newWidth;
    canvas->capacityHeight = newHeight;
    return true;
}

// Draws the sixel `bits` in `color` at column `x` of the band whose top is `y`, `count` times across.
void canvasDraw(SixelCanvas* canvas, uint32_t x, uint32_t y, uint32_t bits, uint32_t count, PrColor8 color) {
    if (bits == 0 || count == 0)
        return;
    uint32_t rows = 6;
    while ((bits & (1u << (rows - 1))) == 0)
        --rows;
    uint64_t right = uint64_t(x) + count;
    if (right > kMaxSide)
        right = kMaxSide;
    if (x >= right || !canvasReserve(canvas, uint32_t(right), y + rows))
        return;
    for (uint32_t row = 0; row < rows; ++row) {
        if ((bits & (1u << row)) == 0)
            continue;
        uint8_t* pixel = canvas->pixels.data + (size_t(y + row) * canvas->capacityWidth + x) * 4;
        for (uint32_t i = x; i < right; ++i, pixel += 4) {
            pixel[0] = color.r;
            pixel[1] = color.g;
            pixel[2] = color.b;
            pixel[3] = 0xff;
        }
    }
    if (right > canvas->width)
        canvas->width = uint32_t(right);
    if (y + rows > canvas->height)
        canvas->height = y + rows;
}

// Reads the decimal number at `*at` of `text`, clamped to 65535, and advances past it. Returns false if there is none.
bool readNumber(PrStringView text, size_t* at, uint32_t* out) {
    size_t i = *at;
    uint32_t value = 0;
    while (i < text.size && text.data[i] >= '0' && text.data[i] <= '9') {
        value = value * 10 + uint32_t(text.data[i] - '0');
        if (value > 65535)
            value = 65535;
        ++i;
    }
    if (i == *at)
        return false;
    *at = i;
    *out = value;
    return true;
}

// Reads up to `capacity` numbers separated by `;` at `*at` of `text` into `out`, and advances past them. A number that
// is left out reads as zero. Returns the count of numbers, including those left out.
uint32_t readNumbers(PrStringView text, size_t* at, uint32_t* out, uint32_t capacity) {
    uint32_t count = 0;
    for (;;) {
        uint32_t value = 0;
        bool read = readNumber(text, at, &value);
        if (count < capacity)
            out[count] = value;
        if (read || (*at < text.size && text.data[*at] == ';'))
            ++count;
        if (*at >= text.size || text.data[*at] != ';')
            return count;
        ++*at;
    }
}

} // namespace

void vtermSixelCommand(Vterm* vterm, const VtermAction* action) {
    VtermGraphics& graphics = vterm->graphics;
    if (graphics.budget == 0)
        return;
    bool transparent = vtermParam(action, 1, 0) == 1;
    PrColor8 palette[kVtermSixelColors] = {};
    for (uint32_t i = 0; i < kVtermSixelColors; ++i) {
        palette[i] = PrColor8{0, 0, 0, 0xff};
        if (i < 16) {
            palette[i] = PrColor8{
                percentChannel(kDefaultColors[i][0]), percentChannel(kDefaultColors[i][1]),
                percentChannel(kDefaultColors[i][2]), 0xff,
            };
        }
    }

    SixelCanvas canvas{.budget = graphics.budget};
    PrUVec2 raster{};
    PrColor8 color = palette[0];
    uint32_t x = 0;
    uint32_t y = 0;
    PrStringView text = action->text;
    size_t at = 0;
    while (at < text.size) {
        char c = text.data[at];
        if (c >= '?' && c <= '~') {
            canvasDraw(&canvas, x, y, uint32_t(c - '?'), 1, color);
            ++x;
            ++at;
        } else if (c == '!') {
            // A repeat: a count, then the sixel that it repeats.
            ++at;
            uint32_t count = 0;
            readNumber(text, &at, &count);
            if (at < text.size && text.data[at] >= '?' && text.data[at] <= '~') {
                canvasDraw(&canvas, x, y, uint32_t(text.data[at] - '?'), count, color);
                x = x + count < kMaxSide ? x + count : kMaxSide;
                ++at;
            }
        } else if (c == '"') {
            // The raster attributes: the pixel aspect ratio, which is ignored, then the width and height.
            ++at;
            uint32_t values[4] = {};
            uint32_t count = readNumbers(text, &at, values, 4);
            if (count >= 4) {
                raster.x = values[2] < kMaxSide ? values[2] : kMaxSide;
                raster.y = values[3] < kMaxSide ? values[3] : kMaxSide;
            }
        } else if (c == '#') {
            // Selects a register, and defines its color if a coordinate system and three values follow.
            ++at;
            uint32_t values[5] = {};
            uint32_t count = readNumbers(text, &at, values, 5);
            uint32_t index = values[0] % kVtermSixelColors;
            if (count >= 5 && values[1] == 1)
                palette[index] = hlsColor(values[2], values[3], values[4]);
            else if (count >= 5 && values[1] == 2)
                palette[index] = PrColor8{
                    percentChannel(values[2]), percentChannel(values[3]), percentChannel(values[4]), 0xff,
                };
            if (count >= 1)
                color = palette[index];
        } else if (c == '$') {
            x = 0;
            ++at;
        } else if (c == '-') {
            x = 0;
            y = y + 6 < kMaxSide ? y + 6 : kMaxSide;
            ++at;
        } else {
            ++at;
        }
    }

    // The image is the raster's size, grown to the sixels that pass it.
    uint32_t width = raster.x > canvas.width ? raster.x : canvas.width;
    uint32_t height = raster.y > canvas.height ? raster.y : canvas.height;
    if (width == 0 || height == 0 || !canvasReserve(&canvas, width, height)) {
        canvas.pixels.reset();
        return;
    }
    VtermCellImage image{.extent = PrUVec2{width, height}};
    image.pixels.resize(width * height * 4);
    PrColor8 background = vterm->roles[PrVtermColorRole_Background];
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* from = canvas.pixels.data + size_t(row) * canvas.capacityWidth * 4;
        uint8_t* to = image.pixels.data + size_t(row) * width * 4;
        prMemCopy(to, from, size_t(width) * 4);
        if (transparent)
            continue;
        for (uint32_t i = 0; i < width; ++i) {
            uint8_t* pixel = to + size_t(i) * 4;
            if (pixel[3] == 0) {
                pixel[0] = background.r;
                pixel[1] = background.g;
                pixel[2] = background.b;
                pixel[3] = 0xff;
            }
        }
    }
    canvas.pixels.reset();

    PrUVec2 cell = graphics.cellPixels;
    image.size = PrVec2{float(width) / float(cell.x), float(height) / float(cell.y)};
    VtermShowCursor cursor = VtermShowCursor_Below;
    if (vterm->modes.sixelDisplay) {
        cursor = VtermShowCursor_Stay;
    } else {
        image.column = vterm->cursor.column;
        image.row = vterm->cursor.row;
        if (vterm->modes.sixelCursorRight)
            cursor = VtermShowCursor_After;
    }
    vtermGraphicsShow(vterm, &image, cursor);
}

void vtermSixelGraphicsAttributes(Vterm* vterm, const VtermAction* action) {
    // The status of a reply: success, an unknown attribute, or an unknown action.
    constexpr uint32_t kSuccess = 0;
    constexpr uint32_t kBadAttribute = 1;
    constexpr uint32_t kBadAction = 2;
    uint32_t attribute = vtermParam(action, 0, 0);
    uint32_t request = vtermParam(action, 1, 0);
    bool knownAction = request >= 1 && request <= 4;
    auto reply = [&](uint32_t status, uint32_t first, uint32_t second, bool pair) {
        vtermPut(vterm, "\x1b[?");
        vtermPutNumber(vterm, attribute);
        vtermPutByte(vterm, ';');
        vtermPutNumber(vterm, status);
        vtermPutByte(vterm, ';');
        vtermPutNumber(vterm, first);
        if (pair) {
            vtermPutByte(vterm, ';');
            vtermPutNumber(vterm, second);
        }
        vtermPutByte(vterm, 'S');
    };
    if (attribute == 1) {
        reply(knownAction ? kSuccess : kBadAction, knownAction ? kVtermSixelColors : 0, 0, false);
    } else if (attribute == 2) {
        PrUVec2 cell = vterm->graphics.cellPixels;
        uint64_t width = uint64_t(vterm->columns) * cell.x;
        uint64_t height = uint64_t(vterm->rows) * cell.y;
        if (!knownAction) {
            reply(kBadAction, 0, 0, false);
            return;
        }
        reply(kSuccess, uint32_t(width < kMaxSide ? width : kMaxSide), uint32_t(height < kMaxSide ? height : kMaxSide), true);
    } else {
        reply(kBadAttribute, 0, 0, false);
    }
}

} // namespace pr_impl

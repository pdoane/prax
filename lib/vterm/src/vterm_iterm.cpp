// iTerm2's inline images: the arguments of a file, its size in cells, and the file in parts.

#include "vterm/src/vterm_iterm.h"

#include "pr/base64.h"
#include "pr/float.h"
#include "pr/string_ops.h"

#include "vterm/src/vterm_graphics.h"
#include "vterm/src/vterm_screen.h"

namespace pr_impl {

namespace {

// The units of a width or a height.
enum {
    ItermUnit_Auto,     // the image's own size
    ItermUnit_Cells,
    ItermUnit_Pixels,
    ItermUnit_Percent,  // of the screen
};
typedef uint8_t ItermUnit;

struct ItermSize {
    ItermUnit unit;
    uint32_t value;
};

// The arguments of a file that the terminal reads (vtermItermCommand).
struct ItermArgs {
    bool shows;
    bool keepsAspect;
    bool staysCursor;
    uint64_t size;
    ItermSize width;
    ItermSize height;
};

// Reads the decimal digits of `text` into `*out`, clamped to 1e9. Returns false if `text` is empty or not all digits.
bool parseUnsigned(PrStringView text, uint64_t* out) {
    if (text.size == 0)
        return false;
    uint64_t value = 0;
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + uint64_t(c - '0');
        if (value > 1000000000)
            value = 1000000000;
    }
    *out = value;
    return true;
}

// Reads a width or a height. A value that does not parse is `auto`.
ItermSize parseSize(PrStringView text) {
    ItermSize size{.unit = ItermUnit_Cells};
    if (prStringEndsWith(text, "px")) {
        size.unit = ItermUnit_Pixels;
        text = prStringHead(text, text.size - 2);
    } else if (prStringEndsWith(text, "%")) {
        size.unit = ItermUnit_Percent;
        text = prStringHead(text, text.size - 1);
    }
    uint64_t value = 0;
    if (!parseUnsigned(text, &value) || value == 0)
        return ItermSize{.unit = ItermUnit_Auto};
    size.value = uint32_t(value);
    return size;
}

// Splits the next `;`-separated field off `*rest`.
PrStringView splitField(PrStringView* rest) {
    size_t end = prStringFindByte(*rest, ';');
    PrStringView field = prStringHead(*rest, end);
    *rest = end < rest->size ? prStringFrom(*rest, end + 1) : PrStringView{rest->data + rest->size, 0};
    return field;
}

ItermArgs parseArgs(PrStringView text) {
    ItermArgs args{.keepsAspect = true};
    while (text.size != 0) {
        PrStringView field = splitField(&text);
        size_t equals = prStringFindByte(field, '=');
        if (equals == field.size)
            continue;
        PrStringView key = prStringHead(field, equals);
        PrStringView value = prStringFrom(field, equals + 1);
        uint64_t number = 0;
        if (key == "inline") {
            args.shows = value == "1";
        } else if (key == "preserveAspectRatio") {
            args.keepsAspect = value != "0";
        } else if (key == "doNotMoveCursor") {
            args.staysCursor = value == "1";
        } else if (key == "size") {
            if (parseUnsigned(value, &number))
                args.size = number;
        } else if (key == "width") {
            args.width = parseSize(value);
        } else if (key == "height") {
            args.height = parseSize(value);
        }
    }
    return args;
}

// Returns a width or a height in cells. `cellPixels` is the cell's size along the axis, and `screen` the screen's.
double sizeCells(ItermSize size, double cellPixels, uint32_t screen) {
    switch (size.unit) {
        case ItermUnit_Cells:
            return size.value;
        case ItermUnit_Pixels:
            return size.value / cellPixels;
        case ItermUnit_Percent:
            return double(size.value) * screen / 100;
        default:
            return 0;
    }
}

// Decodes the file `bytes` and shows it at the cursor as `args` say.
void showFile(Vterm* vterm, const ItermArgs& args, PrByteView bytes) {
    if (!args.shows)
        return;
    VtermCellImage image{};
    if (!vtermGraphicsDecode(vterm, bytes, true, &image.extent, &image.pixels))
        return;
    PrUVec2 cell = vterm->graphics.cellPixels;
    double naturalWidth = double(image.extent.x) / cell.x;
    double naturalHeight = double(image.extent.y) / cell.y;
    double width = sizeCells(args.width, cell.x, vterm->columns);
    double height = sizeCells(args.height, cell.y, vterm->rows);
    bool autoWidth = args.width.unit == ItermUnit_Auto;
    bool autoHeight = args.height.unit == ItermUnit_Auto;
    if (autoWidth && autoHeight) {
        width = naturalWidth;
        height = naturalHeight;
    } else if (autoWidth) {
        width = args.keepsAspect ? height * naturalWidth / naturalHeight : naturalWidth;
    } else if (autoHeight) {
        height = args.keepsAspect ? width * naturalHeight / naturalWidth : naturalHeight;
    } else if (args.keepsAspect) {
        double scale = prF64Min(width / naturalWidth, height / naturalHeight);
        width = naturalWidth * scale;
        height = naturalHeight * scale;
    }
    VtermCursorState& cursor = vterm->cursor;
    double room = vterm->columns - cursor.column;
    if (width > room) {
        if (args.keepsAspect)
            height *= room / width;
        width = room;
    }
    image.column = cursor.column;
    image.row = cursor.row;
    image.size = PrVec2{float(width), float(height)};
    vtermGraphicsShow(vterm, &image, args.staysCursor ? VtermShowCursor_Stay : VtermShowCursor_After);
}

// Shows a file sent whole: its arguments, a colon, and its base64.
void fileCommand(Vterm* vterm, PrStringView text) {
    size_t colon = prStringFindByte(text, ':');
    if (colon == text.size)
        return;
    ItermArgs args = parseArgs(prStringHead(text, colon));
    PrStringView payload = prStringFrom(text, colon + 1);
    uint64_t budget = vterm->graphics.budget;
    if (!args.shows || args.size > budget || prBase64DecodedCapacity(payload.size) > budget)
        return;
    VtermGraphicsLoad load{};
    if (vtermGraphicsLoadAppend(&load, payload, budget) && vtermGraphicsLoadFinish(&load))
        showFile(vterm, args, PrByteView{load.data.data, load.data.count});
    load.data.reset();
    load.control.reset();
}

} // namespace

void vtermItermCommand(Vterm* vterm, PrStringView text) {
    VtermGraphics& graphics = vterm->graphics;
    VtermGraphicsLoad& load = graphics.itermLoad;
    if (graphics.budget == 0)
        return;
    if (prStringStartsWith(text, "File=")) {
        fileCommand(vterm, prStringFrom(text, 5));
    } else if (prStringStartsWith(text, "MultipartFile=")) {
        PrStringView arguments = prStringFrom(text, 14);
        ItermArgs args = parseArgs(arguments);
        vtermGraphicsLoadDrop(&load);
        load.active = true;
        load.failed = !args.shows || args.size > graphics.budget;
        load.control.append(arguments.data, uint32_t(arguments.size));
    } else if (prStringStartsWith(text, "FilePart=")) {
        if (load.active && !load.failed && !vtermGraphicsLoadAppend(&load, prStringFrom(text, 9), graphics.budget))
            load.failed = true;
    } else if (text == "FileEnd") {
        if (load.active && !load.failed && vtermGraphicsLoadFinish(&load)) {
            ItermArgs args = parseArgs(PrStringView{load.control.data, load.control.count});
            showFile(vterm, args, PrByteView{load.data.data, load.data.count});
        }
        vtermGraphicsLoadDrop(&load);
    }
}

} // namespace pr_impl

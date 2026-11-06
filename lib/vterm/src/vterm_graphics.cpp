// kitty's terminal graphics protocol. This file runs the protocol's commands, keeps the images and placements of each
// screen, and reports the placements that the caller draws, including those that placeholders show.

#include "pr/vterm_graphics.h"
#include "vterm/src/vterm_graphics.h"

#include "pr/assert.h"
#include "pr/base64.h"
#include "pr/compress.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/image.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/raw_string.h"
#include "pr/sort.h"
#include "pr/utf8.h"

#include "vterm/src/vterm_screen.h"

namespace pr_impl {

namespace {

// The largest image, in pixels along either axis, and the largest placement, in cells along either axis.
constexpr uint32_t kMaxImageSide = 10000;
constexpr uint32_t kMaxPlacementCells = 10000;

// The most images and placements on one screen. Past either limit, the oldest one is removed.
constexpr uint32_t kMaxImages = 4096;
constexpr uint32_t kMaxPlacements = 4096;

// The first id that the terminal assigns to an image. The ids below it are left for programs to choose.
constexpr uint32_t kFirstGivenId = 0x80000000u;

// The code point of a placeholder of a virtual placement.
constexpr uint32_t kPlaceholder = 0x10eeee;

// The cell's pixel extent until the caller sets one.
constexpr PrUVec2 kDefaultCellPixels = {8, 16};

// The answer to every failure to read a file. One answer for all failures tells a program nothing about files that it
// cannot read.
constexpr PrStringView kFileError = "EBADF:Failed to read image file";

// The diacritics that encode a placeholder's numbers: its row, its column, and the most significant byte of its image
// id. A diacritic stands for its index in the list. The list is the protocol's rowcolumn-diacritics.txt, in ascending
// order.
constexpr uint32_t kDiacritics[] = {
    0x0305, 0x030d, 0x030e, 0x0310, 0x0312, 0x033d, 0x033e, 0x033f, 0x0346, 0x034a, 0x034b, 0x034c, 0x0350, 0x0351,
    0x0352, 0x0357, 0x035b, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369, 0x036a, 0x036b, 0x036c, 0x036d,
    0x036e, 0x036f, 0x0483, 0x0484, 0x0485, 0x0486, 0x0487, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597, 0x0598, 0x0599,
    0x059c, 0x059d, 0x059e, 0x059f, 0x05a0, 0x05a1, 0x05a8, 0x05a9, 0x05ab, 0x05ac, 0x05af, 0x05c4, 0x0610, 0x0611,
    0x0612, 0x0613, 0x0614, 0x0615, 0x0616, 0x0617, 0x0657, 0x0658, 0x0659, 0x065a, 0x065b, 0x065d, 0x065e, 0x06d6,
    0x06d7, 0x06d8, 0x06d9, 0x06da, 0x06db, 0x06dc, 0x06df, 0x06e0, 0x06e1, 0x06e2, 0x06e4, 0x06e7, 0x06e8, 0x06eb,
    0x06ec, 0x0730, 0x0732, 0x0733, 0x0735, 0x0736, 0x073a, 0x073d, 0x073f, 0x0740, 0x0741, 0x0743, 0x0745, 0x0747,
    0x0749, 0x074a, 0x07eb, 0x07ec, 0x07ed, 0x07ee, 0x07ef, 0x07f0, 0x07f1, 0x07f3, 0x0816, 0x0817, 0x0818, 0x0819,
    0x081b, 0x081c, 0x081d, 0x081e, 0x081f, 0x0820, 0x0821, 0x0822, 0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082a,
    0x082b, 0x082c, 0x082d, 0x0951, 0x0953, 0x0954, 0x0f82, 0x0f83, 0x0f86, 0x0f87, 0x135d, 0x135e, 0x135f, 0x17dd,
    0x193a, 0x1a17, 0x1a75, 0x1a76, 0x1a77, 0x1a78, 0x1a79, 0x1a7a, 0x1a7b, 0x1a7c, 0x1b6b, 0x1b6d, 0x1b6e, 0x1b6f,
    0x1b70, 0x1b71, 0x1b72, 0x1b73, 0x1cd0, 0x1cd1, 0x1cd2, 0x1cda, 0x1cdb, 0x1ce0, 0x1dc0, 0x1dc1, 0x1dc3, 0x1dc4,
    0x1dc5, 0x1dc6, 0x1dc7, 0x1dc8, 0x1dc9, 0x1dcb, 0x1dcc, 0x1dd1, 0x1dd2, 0x1dd3, 0x1dd4, 0x1dd5, 0x1dd6, 0x1dd7,
    0x1dd8, 0x1dd9, 0x1dda, 0x1ddb, 0x1ddc, 0x1ddd, 0x1dde, 0x1ddf, 0x1de0, 0x1de1, 0x1de2, 0x1de3, 0x1de4, 0x1de5,
    0x1de6, 0x1dfe, 0x20d0, 0x20d1, 0x20d4, 0x20d5, 0x20d6, 0x20d7, 0x20db, 0x20dc, 0x20e1, 0x20e7, 0x20e9, 0x20f0,
    0x2cef, 0x2cf0, 0x2cf1, 0x2de0, 0x2de1, 0x2de2, 0x2de3, 0x2de4, 0x2de5, 0x2de6, 0x2de7, 0x2de8, 0x2de9, 0x2dea,
    0x2deb, 0x2dec, 0x2ded, 0x2dee, 0x2def, 0x2df0, 0x2df1, 0x2df2, 0x2df3, 0x2df4, 0x2df5, 0x2df6, 0x2df7, 0x2df8,
    0x2df9, 0x2dfa, 0x2dfb, 0x2dfc, 0x2dfd, 0x2dfe, 0x2dff, 0xa66f, 0xa67c, 0xa67d, 0xa6f0, 0xa6f1, 0xa8e0, 0xa8e1,
    0xa8e2, 0xa8e3, 0xa8e4, 0xa8e5, 0xa8e6, 0xa8e7, 0xa8e8, 0xa8e9, 0xa8ea, 0xa8eb, 0xa8ec, 0xa8ed, 0xa8ee, 0xa8ef,
    0xa8f0, 0xa8f1, 0xaab0, 0xaab2, 0xaab3, 0xaab7, 0xaab8, 0xaabe, 0xaabf, 0xaac1, 0xfe20, 0xfe21, 0xfe22, 0xfe23,
    0xfe24, 0xfe25, 0xfe26, 0x10a0f, 0x10a38, 0x1d185, 0x1d186, 0x1d187, 0x1d188, 0x1d189, 0x1d1aa, 0x1d1ab, 0x1d1ac,
    0x1d1ad, 0x1d242, 0x1d243, 0x1d244,
};

// The index of no diacritic.
constexpr uint32_t kDiacriticMissing = 0xffffffffu;

// Returns the number that `codePoint` encodes as a diacritic, or kDiacriticMissing.
uint32_t diacriticIndex(uint32_t codePoint) {
    uint32_t low = 0;
    uint32_t high = countOf(kDiacritics);
    while (low < high) {
        uint32_t middle = (low + high) / 2;
        if (kDiacritics[middle] < codePoint)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < countOf(kDiacritics) && kDiacritics[low] == codePoint)
        return low;
    return kDiacriticMissing;
}

// A graphics command's keys. A key that is absent has its default.
struct GraphicsCommand {
    char action;                // a: t, T, q, p, d, or another that the terminal refuses
    char medium;                // t
    char compression;           // o, or zero for none
    char deletion;              // d
    char feature;               // e, or zero
    uint32_t quiet;             // q
    uint32_t format;            // f
    uint32_t width;             // s
    uint32_t height;            // v
    uint32_t size;              // S
    uint32_t offset;            // O
    uint32_t id;                // i
    uint32_t number;            // I
    uint32_t placement;         // p
    uint32_t more;              // m

    // The source rectangle (x, y, w, h), the offset within the cell, and the size in columns and rows.
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t cellX;             // X
    uint32_t cellY;             // Y
    uint32_t columns;           // c
    uint32_t rows;              // r

    uint32_t cursorPolicy;      // C
    uint32_t placeholder;       // U
    uint32_t parent;            // P
    int32_t z;

    bool hasId;
    bool hasNumber;
    bool invalid;               // a key's value did not parse
    bool continues;             // only m and q are present, the keys that a later chunk may carry
    PrStringView control;       // the keys as the program wrote them
};

// A command with every key at its default.
GraphicsCommand commandDefault() {
    return GraphicsCommand{.action = 't', .medium = 'd', .deletion = 'a', .format = 32, .continues = true};
}

bool parseUnsigned(PrStringView text, uint32_t* out) {
    uint64_t value = 0;
    if (!prU64Parse(text.data, text.size, &value, 10) || value > UINT32_MAX)
        return false;
    *out = uint32_t(value);
    return true;
}

bool parseSigned(PrStringView text, int32_t* out) {
    bool negative = text.size > 1 && text.data[0] == '-';
    PrStringView digits = negative ? PrStringView{text.data + 1, text.size - 1} : text;
    uint64_t value = 0;
    if (!prU64Parse(digits.data, digits.size, &value, 10) || value > (negative ? 0x80000000ull : 0x7fffffffull))
        return false;
    *out = negative ? int32_t(-int64_t(value)) : int32_t(value);
    return true;
}

// Splits `*text` at its first `separator`: returns what comes before it, and leaves what follows in `*text`.
PrStringView splitAt(PrStringView* text, char separator) {
    size_t end = 0;
    while (end < text->size && text->data[end] != separator)
        ++end;
    PrStringView field{text->data, end};
    size_t next = end < text->size ? end + 1 : end;
    *text = PrStringView{text->data + next, text->size - next};
    return field;
}

// Reads the keys of `control` into `*command`. A key that the terminal does not know is ignored, and a value that does
// not parse marks the command invalid.
void commandParse(PrStringView control, GraphicsCommand* command) {
    command->control = control;
    while (control.size != 0) {
        PrStringView field = splitAt(&control, ',');
        if (field.size == 0)
            continue;
        if (field.size < 3 || field.data[1] != '=') {
            command->invalid = true;
            continue;
        }
        char key = field.data[0];
        PrStringView value{field.data + 2, field.size - 2};
        if (key != 'm' && key != 'q')
            command->continues = false;
        uint32_t* number = nullptr;
        char* letter = nullptr;
        switch (key) {
            case 'a':
                letter = &command->action;
                break;
            case 't':
                letter = &command->medium;
                break;
            case 'o':
                letter = &command->compression;
                break;
            case 'd':
                letter = &command->deletion;
                break;
            case 'e':
                letter = &command->feature;
                break;
            case 'q':
                number = &command->quiet;
                break;
            case 'f':
                number = &command->format;
                break;
            case 's':
                number = &command->width;
                break;
            case 'v':
                number = &command->height;
                break;
            case 'S':
                number = &command->size;
                break;
            case 'O':
                number = &command->offset;
                break;
            case 'i':
                number = &command->id;
                command->hasId = true;
                break;
            case 'I':
                number = &command->number;
                command->hasNumber = true;
                break;
            case 'p':
                number = &command->placement;
                break;
            case 'm':
                number = &command->more;
                break;
            case 'x':
                number = &command->x;
                break;
            case 'y':
                number = &command->y;
                break;
            case 'w':
                number = &command->w;
                break;
            case 'h':
                number = &command->h;
                break;
            case 'X':
                number = &command->cellX;
                break;
            case 'Y':
                number = &command->cellY;
                break;
            case 'c':
                number = &command->columns;
                break;
            case 'r':
                number = &command->rows;
                break;
            case 'C':
                number = &command->cursorPolicy;
                break;
            case 'U':
                number = &command->placeholder;
                break;
            case 'P':
                number = &command->parent;
                break;
            case 'z':
                if (!parseSigned(value, &command->z))
                    command->invalid = true;
                continue;
            default:
                continue;
        }
        if (letter != nullptr) {
            if (value.size != 1)
                command->invalid = true;
            else
                *letter = value.data[0];
        } else if (!parseUnsigned(value, number)) {
            command->invalid = true;
        }
    }
    // An id or a number of zero is none.
    command->hasId = command->hasId && command->id != 0;
    command->hasNumber = command->hasNumber && command->number != 0;
}

// Answers `command` with `message`, which is OK or an error. Sends nothing if the command's `q` key suppresses this
// kind of answer, or if the command has neither an id nor a number. `id` is the image's id. For a command with an image
// number, it is the id that the terminal gave the image.
void answer(Vterm* vterm, const GraphicsCommand& command, uint32_t id, PrStringView message) {
    bool ok = message == "OK";
    if (command.quiet >= (ok ? 1u : 2u) || (id == 0 && !command.hasNumber))
        return;
    vtermPut(vterm, "\x1b_G");
    bool first = true;
    auto key = [&](PrStringView name, uint32_t value) {
        if (!first)
            vtermPutByte(vterm, ',');
        first = false;
        vtermPut(vterm, name);
        vtermPutNumber(vterm, value);
    };
    if (id != 0)
        key("i=", id);
    if (command.hasNumber)
        key("I=", command.number);
    if (command.placement != 0)
        key("p=", command.placement);
    vtermPutByte(vterm, ';');
    vtermPut(vterm, message);
    vtermPut(vterm, "\x1b\\");
}

// Stores

VtermGraphicsStore& activeStore(Vterm* vterm) {
    return vterm->graphics.stores[vterm->active];
}

// The number of lines in the primary screen's scrollback.
uint32_t scrollbackCount(const Vterm* vterm) {
    return vterm->scrollback.count;
}

// The line number of the top row of the screen of `kind`.
int64_t screenTop(const Vterm* vterm, VtermScreenKind kind) {
    if (kind == VtermScreenKind_Alternate)
        return 0;
    return int64_t(vterm->scrollbackDropped + vterm->scrollback.count);
}

VtermStoredImage* imageFromKey(VtermGraphicsStore* store, uint64_t key) {
    for (VtermStoredImage& image : store->images.span()) {
        if (image.key == key)
            return &image;
    }
    return nullptr;
}

VtermStoredImage* imageFromId(VtermGraphicsStore* store, uint32_t id) {
    for (VtermStoredImage& image : store->images.span()) {
        if (image.id == id)
            return &image;
    }
    return nullptr;
}

// The newest image with the number `number`.
VtermStoredImage* imageFromNumber(VtermGraphicsStore* store, uint32_t number) {
    VtermStoredImage* newest = nullptr;
    for (VtermStoredImage& image : store->images.span()) {
        if (image.number == number && (newest == nullptr || image.key > newest->key))
            newest = &image;
    }
    return newest;
}

// The image that a command names by its id or its number.
VtermStoredImage* imageOf(VtermGraphicsStore* store, const GraphicsCommand& command) {
    if (command.hasId)
        return imageFromId(store, command.id);
    if (command.hasNumber)
        return imageFromNumber(store, command.number);
    return nullptr;
}

bool hasPlacements(const VtermGraphicsStore* store, uint64_t key) {
    for (const VtermStoredPlacement& placement : store->placements.span()) {
        if (placement.image == key)
            return true;
    }
    return false;
}

// Removes the image at `index` of the store and its placements.
void imageRemove(Vterm* vterm, VtermGraphicsStore* store, uint32_t index) {
    VtermStoredImage& image = store->images.data[index];
    uint64_t key = image.key;
    vterm->graphics.bytes -= image.pixels.count;
    image.pixels.reset();
    store->images.erase(index);
    for (uint32_t i = store->placements.count; i-- > 0;) {
        if (store->placements.data[i].image == key)
            store->placements.erase(i);
    }
}

void storeReset(Vterm* vterm, VtermGraphicsStore* store) {
    for (VtermStoredImage& image : store->images.span()) {
        vterm->graphics.bytes -= image.pixels.count;
        image.pixels.reset();
    }
    store->images.clear();
    store->placements.clear();
}

// Frees the store's images that have no id and that no placement shows. Nothing can name such an image again.
void freeUnnamed(Vterm* vterm, VtermGraphicsStore* store) {
    for (uint32_t i = store->images.count; i-- > 0;) {
        const VtermStoredImage& image = store->images.data[i];
        if (image.id == 0 && !hasPlacements(store, image.key))
            imageRemove(vterm, store, i);
    }
}

// Frees images until the images of both stores fit the budget, sparing the image whose key is `kept`. It frees the
// least recently used of the images that no placement shows first, and then the least recently used of the rest.
void applyBudget(Vterm* vterm, uint64_t kept) {
    VtermGraphics& graphics = vterm->graphics;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        bool placed = pass == 1;
        while (graphics.bytes > graphics.budget) {
            VtermGraphicsStore* oldestStore = nullptr;
            uint32_t oldest = 0;
            for (VtermGraphicsStore& store : graphics.stores) {
                for (uint32_t i = 0; i < store.images.count; ++i) {
                    const VtermStoredImage& image = store.images.data[i];
                    if (image.key == kept || (!placed && hasPlacements(&store, image.key)))
                        continue;
                    if (oldestStore == nullptr || image.used < oldestStore->images.data[oldest].used) {
                        oldestStore = &store;
                        oldest = i;
                    }
                }
            }
            if (oldestStore == nullptr)
                break;
            imageRemove(vterm, oldestStore, oldest);
        }
    }
}

// Returns an id for the terminal to give an image. No image of the store has the id.
uint32_t givenId(VtermGraphics* graphics, VtermGraphicsStore* store) {
    for (;;) {
        uint32_t id = graphics->nextId++;
        if (graphics->nextId == 0)
            graphics->nextId = kFirstGivenId;
        if (id != 0 && imageFromId(store, id) == nullptr)
            return id;
    }
}

// Clips the shown rows of the store's placements to the lines from `first` to `end`, excluded, and removes the
// placements that keep no rows.
void storeClip(VtermGraphicsStore* store, int64_t first, int64_t end) {
    for (uint32_t i = store->placements.count; i-- > 0;) {
        VtermStoredPlacement& placement = store->placements.data[i];
        if (placement.isVirtual)
            continue;
        int64_t top = placement.line + placement.firstRow;
        int64_t bottom = placement.line + placement.endRow;
        if (top < first)
            top = first;
        if (bottom > end)
            bottom = end;
        if (top >= bottom) {
            store->placements.erase(i);
            continue;
        }
        placement.firstRow = uint32_t(top - placement.line);
        placement.endRow = uint32_t(bottom - placement.line);
    }
}

// Transmission

// Appends the bytes of a chunk's base64 `payload` to the load. Characters past the last whole group of four wait for
// the next chunk. Returns false if the payload is not base64 or the load passes `limit` bytes.
bool loadAppend(VtermGraphicsLoad* load, PrStringView payload, uint64_t limit) {
    size_t at = 0;
    if (load->carryCount != 0) {
        while (load->carryCount < 4 && at < payload.size)
            load->carry[load->carryCount++] = payload.data[at++];
        if (load->carryCount < 4)
            return true;
        char decoded[3];
        size_t size = 0;
        if (!prBase64Decode(PrStringView{load->carry, 4}, decoded, sizeof(decoded), &size))
            return false;
        load->data.append(reinterpret_cast<const uint8_t*>(decoded), uint32_t(size));
        load->carryCount = 0;
    }
    PrStringView rest{payload.data + at, payload.size - at};
    size_t whole = rest.size / 4 * 4;
    if (whole != 0) {
        size_t capacity = prBase64DecodedCapacity(whole);
        if (load->data.count + capacity > limit) {
            load->tooLarge = true;
            return false;
        }
        uint32_t start = load->data.count;
        load->data.resize(uint32_t(start + capacity));
        size_t size = 0;
        if (!prBase64Decode(PrStringView{rest.data, whole}, reinterpret_cast<char*>(load->data.data + start), capacity, &size))
            return false;
        load->data.count = uint32_t(start + size);
    }
    for (size_t i = whole; i < rest.size; ++i)
        load->carry[load->carryCount++] = rest.data[i];
    load->tooLarge = load->data.count > limit;
    return !load->tooLarge;
}

// Ends the load by decoding the characters that still wait. Returns false if they are not base64.
bool loadFinish(VtermGraphicsLoad* load) {
    if (load->carryCount == 0)
        return true;
    char decoded[3];
    size_t size = 0;
    bool decodedAll = prBase64Decode(PrStringView{load->carry, load->carryCount}, decoded, sizeof(decoded), &size);
    load->carryCount = 0;
    if (!decodedAll)
        return false;
    load->data.append(reinterpret_cast<const uint8_t*>(decoded), uint32_t(size));
    return true;
}

// The error that answers a load that failed.
PrStringView loadError(const VtermGraphicsLoad& load) {
    if (load.tooLarge)
        return "EFBIG:the data is larger than the storage quota";
    return "EINVAL:the data is not base64";
}

void loadDrop(VtermGraphicsLoad* load) {
    load->active = false;
    load->failed = false;
    load->tooLarge = false;
    load->control.clear();
    load->data.clear();
    load->carryCount = 0;
}

// Returns true if `path` is `directory` or is in it.
bool isUnder(PrStringView path, PrStringView directory) {
    if (directory.size == 0 || path.size < directory.size || prMemCompare(path.data, directory.data, directory.size) != 0)
        return false;
    return path.size == directory.size || directory.data[directory.size - 1] == '/' || path.data[directory.size] == '/';
}

// Returns true if the normalized absolute `path` is in a temporary directory: /tmp, /var/tmp, /dev/shm, the user's
// temporary directory, or their real paths on Apple's platforms.
bool isTemporary(PrStringView path) {
    static constexpr PrStringView kDirectories[] = {"/tmp", "/var/tmp", "/dev/shm", "/private/tmp", "/private/var/tmp"};
    for (PrStringView directory : kDirectories) {
        if (isUnder(path, directory))
            return true;
    }
    PrRawString temporary = prFileUserDirectory(PrFileUserDirectory_Temporary);
    bool under = false;
    if (prRawStringView(&temporary).size != 0) {
        PrRawString directory = prFilePathAbsolute(prRawStringView(&temporary), "");
        under = isUnder(path, prRawStringView(&directory));
        prRawStringReset(&directory);
    }
    prRawStringReset(&temporary);
    return under;
}

// Returns true if `text` contains `part`.
bool contains(PrStringView text, PrStringView part) {
    for (size_t i = 0; i + part.size <= text.size; ++i) {
        if (prMemCompare(text.data + i, part.data, part.size) == 0)
            return true;
    }
    return false;
}

// Reads the file that a command's payload names into `*out`, from the command's offset, for its size or to the end of
// the file. The path must be absolute and must name a regular file of at most `limit` bytes. It must be outside the
// file system's views of devices and processes. On Windows, it must be on a drive, because opening a UNC path sends the
// user's credentials to its host. A temporary file that passes these checks is deleted, whether or not the read
// succeeds. Returns false for any failure, and the program learns only that the read failed.
bool readFile(Vterm* vterm, const GraphicsCommand& command, PrStringView payload, uint64_t limit, RawArray<uint8_t>* out) {
    if ((vterm->graphics.flags & PrVtermImageFlag_Files) == 0)
        return false;
    RawArray<char> decoded;
    decoded.resize(uint32_t(prBase64DecodedCapacity(payload.size)));
    size_t size = 0;
    bool valid = prBase64Decode(payload, decoded.data, decoded.count, &size);
    PrStringView named{decoded.data, size};
    valid = valid && prFilePathIsAbsolute(named) && prUtf8Validate(named) == size;
    for (size_t i = 0; valid && i < size; ++i)
        valid = named.data[i] != 0;
    PrRawString path = valid ? prFilePathAbsolute(named, "") : prRawStringNull();
    decoded.reset();
    PrStringView absolute = prRawStringView(&path);
    bool temporary = command.medium == 't';
    // Only a Windows path normalizes to two leading slashes: a UNC or device path.
    bool sensitive = isUnder(absolute, "//") || isUnder(absolute, "/proc") || isUnder(absolute, "/sys") ||
        (isUnder(absolute, "/dev") && !(temporary && isUnder(absolute, "/dev/shm")));
    bool allowed = absolute.size != 0 && !sensitive;
    if (temporary)
        allowed = allowed && isTemporary(absolute) && contains(absolute, "tty-graphics-protocol");
    PrFileInfo info{};
    bool read = false;
    if (allowed && prFileInfo(absolute, &info) && info.kind == PrFileKind_Regular &&
            info.size <= limit + command.offset) {
        uint64_t end = command.size != 0 ? uint64_t(command.offset) + command.size : info.size;
        PrByteView mapped{};
        if (end <= info.size && end > command.offset && end - command.offset <= limit && prFileMap(absolute, &mapped)) {
            if (mapped.size >= end) {
                out->append(mapped.data + command.offset, uint32_t(end - command.offset));
                read = true;
            }
            prFileUnmap(&mapped);
        }
    }
    if (allowed && temporary)
        prFileRemove(absolute);
    prRawStringReset(&path);
    return read;
}

// Decompresses zlib `input` into `*out`, up to `limit` bytes. Returns false for a stream that is corrupt or that does
// not fit, and sets `*tooLarge` in the latter case. With `truncate`, a stream that does not fit is cut at the limit.
bool inflate(PrCompressCodec* codec, PrByteView input, uint64_t limit, bool truncate, RawArray<uint8_t>* out,
        bool* tooLarge) {
    PrCompressDecoder* decoder = nullptr;
    if (prCompressDecoderCreate(codec, PrCompressFormat_Zlib, &decoder) != PrStatus_Ok)
        return false;
    bool ok = false;
    size_t consumed = 0;
    for (;;) {
        if (out->count == out->capacity) {
            if (out->count >= limit) {
                *tooLarge = !truncate;
                ok = truncate;
                break;
            }
            uint64_t grown = out->capacity < 65536 ? 65536 : uint64_t(out->capacity) * 2;
            out->reserve(uint32_t(grown < limit ? grown : limit));
        }
        PrCompressRead read{};
        size_t room = out->capacity - out->count;
        PrByteView rest{input.data + consumed, input.size - consumed};
        PrStatus status = prCompressDecoderRead(decoder, rest, true, out->data + out->count, room, &read);
        consumed += read.consumed;
        out->count += uint32_t(read.written);
        if (status != PrStatus_Ok)
            break;
        if (read.progress == PrCompressProgress_Done) {
            ok = true;
            break;
        }
        if (read.progress == PrCompressProgress_Input)
            break;
    }
    prCompressDecoderDestroy(decoder);
    return ok;
}

// Decodes a PNG into `*image`. Returns false if it is not a PNG that decodes within the limits.
bool decodePng(Vterm* vterm, PrByteView bytes, VtermStoredImage* image) {
    static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (bytes.size < 8 || prMemCompare(bytes.data, kSignature, 8) != 0)
        return false;
    PrImageDecoder* decoder = prImageDecoderCreate(vterm->graphics.imageCodec);
    PrImageLimits limits = prImageLimitsDefault();
    uint64_t pixels = uint64_t(kMaxImageSide) * kMaxImageSide;
    if (limits.pixels > pixels)
        limits.pixels = pixels;
    PrImageSource source{};
    source.memory = bytes;
    bool ok = false;
    if (prImageDecoderOpen(decoder, source, &limits) == PrStatus_Ok) {
        PrUVec2 extent = prImageDecoderInfo(decoder)->extent;
        if (extent.x != 0 && extent.y != 0 && extent.x <= kMaxImageSide && extent.y <= kMaxImageSide) {
            image->extent = extent;
            image->pixels.resize(extent.x * extent.y * 4);
            PrPixels output{image->pixels.data, size_t(extent.x) * 4, extent};
            ok = prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Identity, output) ==
                PrStatus_Ok;
        }
    }
    prImageDecoderDestroy(decoder);
    return ok;
}

// Decodes the bytes of a transmission into `*image`. Returns OK or the error to answer.
PrStringView decodeImage(Vterm* vterm, const GraphicsCommand& command, RawArray<uint8_t>* data, VtermStoredImage* image) {
    VtermGraphics& graphics = vterm->graphics;
    bool raw = command.format == 24 || command.format == 32;
    if (!raw && command.format != 100)
        return "EINVAL:unsupported format";
    if (command.format == 100 && graphics.imageCodec == nullptr)
        return "EINVAL:PNG is not supported";
    uint32_t channels = command.format == 24 ? 3 : 4;
    if (raw) {
        if (command.width == 0 || command.height == 0)
            return "EINVAL:the width and height are required";
        if (command.width > kMaxImageSide || command.height > kMaxImageSide)
            return "EINVAL:the image is too large";
    }
    uint64_t needed = uint64_t(command.width) * command.height * channels;
    if (raw && uint64_t(command.width) * command.height * 4 > graphics.budget)
        return "EFBIG:the image is larger than the storage quota";
    if (command.compression == 'z') {
        if (graphics.compressCodec == nullptr)
            return "EINVAL:compression is not supported";
        RawArray<uint8_t> inflated;
        bool tooLarge = false;
        uint64_t limit = raw ? needed : (command.size != 0 ? command.size : graphics.budget);
        bool ok =
            inflate(graphics.compressCodec, PrByteView{data->data, data->count}, limit, raw, &inflated, &tooLarge);
        if (!ok) {
            inflated.reset();
            if (tooLarge)
                return "EFBIG:the decompressed data is too large";
            return "EINVAL:the zlib data does not decompress";
        }
        data->reset();
        *data = inflated;
    } else if (command.compression != 0) {
        return "EINVAL:unsupported compression";
    }
    if (raw) {
        if (data->count < needed)
            return "ENODATA:insufficient image data";
        image->extent = PrUVec2{command.width, command.height};
        image->pixels.resize(uint32_t(command.width * command.height * 4));
        for (uint32_t i = 0; i < command.width * command.height; ++i) {
            const uint8_t* from = data->data + size_t(i) * channels;
            uint8_t* to = image->pixels.data + size_t(i) * 4;
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
            to[3] = channels == 4 ? from[3] : 0xff;
        }
        return "OK";
    }
    if (!decodePng(vterm, PrByteView{data->data, data->count}, image)) {
        image->pixels.reset();
        return "EBADPNG:the PNG does not decode";
    }
    if (image->pixels.count > graphics.budget) {
        image->pixels.reset();
        return "EFBIG:the image is larger than the storage quota";
    }
    return "OK";
}

// Placement

// Computes the placement's source rectangle and destination from the command's source rectangle, offset within the
// cell, and size in columns and rows. The cell's pixel extent converts pixels to cells. Returns false if the source
// rectangle is empty.
bool placementGeometry(const Vterm* vterm, const GraphicsCommand& command, const VtermStoredImage& image,
        VtermStoredPlacement* placement) {
    PrUVec2 extent = image.extent;
    uint32_t x = command.x < extent.x ? command.x : extent.x;
    uint32_t y = command.y < extent.y ? command.y : extent.y;
    uint32_t w = extent.x - x;
    uint32_t h = extent.y - y;
    if (command.w != 0 && command.w < w)
        w = command.w;
    if (command.h != 0 && command.h < h)
        h = command.h;
    if (w == 0 || h == 0)
        return false;
    placement->sourceOrigin = PrUVec2{x, y};
    placement->sourceExtent = PrUVec2{w, h};

    double cellWidth = vterm->graphics.cellPixels.x;
    double cellHeight = vterm->graphics.cellPixels.y;
    double offsetX = command.cellX < cellWidth ? command.cellX : cellWidth - 1;
    double offsetY = command.cellY < cellHeight ? command.cellY : cellHeight - 1;
    if (placement->isVirtual) {
        offsetX = 0;
        offsetY = 0;
    }
    double width = w;
    double height = h;
    uint32_t columns = command.columns;
    uint32_t rows = command.rows;
    if (columns != 0 && rows != 0) {
        // Scale the image to fit its cells, centered, keeping its aspect ratio.
        double boxWidth = columns * cellWidth - offsetX;
        double boxHeight = rows * cellHeight - offsetY;
        double scale = prF64Min(boxWidth / w, boxHeight / h);
        width = w * scale;
        height = h * scale;
        offsetX += (boxWidth - width) / 2;
        offsetY += (boxHeight - height) / 2;
    } else if (columns != 0) {
        width = columns * cellWidth - offsetX;
        height = h * width / w;
    } else if (rows != 0) {
        height = rows * cellHeight - offsetY;
        width = w * height / h;
    }
    if (columns == 0)
        columns = uint32_t(prF64Ceil((width + offsetX) / cellWidth - 1e-9));
    if (rows == 0)
        rows = uint32_t(prF64Ceil((height + offsetY) / cellHeight - 1e-9));
    placement->columns = columns != 0 ? columns : 1;
    placement->rows = rows != 0 ? rows : 1;
    placement->firstRow = 0;
    placement->endRow = placement->rows;
    placement->offset = PrVec2{float(offsetX / cellWidth), float(offsetY / cellHeight)};
    placement->extent = PrVec2{float(width / cellWidth), float(height / cellHeight)};
    return true;
}

// Moves the cursor past a placement of `columns` by `rows` at the cursor. The cursor moves down by one less than
// `rows`, and the region scrolls if the cursor passes its bottom margin. The cursor then moves right by `columns`, or
// to the next line if that passes the last column.
void cursorPast(Vterm* vterm, uint32_t columns, uint32_t rows) {
    VtermCursorState& cursor = vterm->cursor;
    uint32_t down = rows - 1;
    if (cursor.row < vterm->bottom && cursor.row + uint64_t(down) >= vterm->bottom) {
        uint64_t past = cursor.row + uint64_t(down) - (vterm->bottom - 1);
        vtermScrollUp(vterm, past < vterm->rows ? uint32_t(past) : vterm->rows);
        cursor.row = vterm->bottom - 1;
    } else {
        uint64_t row = cursor.row + uint64_t(down);
        cursor.row = row < vterm->rows ? uint32_t(row) : vterm->rows - 1;
    }
    uint64_t column = cursor.column + uint64_t(columns);
    cursor.wrapPending = false;
    if (column >= vterm->columns) {
        cursor.column = 0;
        vtermIndex(vterm);
    } else {
        cursor.column = uint32_t(column);
    }
    vterm->hasLastPrinted = false;
}

// Places `image` as the command says. Returns OK or the error to answer.
PrStringView put(Vterm* vterm, const GraphicsCommand& command, VtermStoredImage* image) {
    VtermGraphics& graphics = vterm->graphics;
    VtermGraphicsStore& store = activeStore(vterm);
    if (command.parent != 0)
        return "EINVAL:relative placements are not supported";
    if (command.columns > kMaxPlacementCells || command.rows > kMaxPlacementCells)
        return "EINVAL:too many columns or rows";
    VtermStoredPlacement placement{.image = image->key, .isVirtual = command.placeholder == 1, .z = command.z};
    if (!placementGeometry(vterm, command, *image, &placement))
        return "EINVAL:the source rectangle is empty";
    placement.id = image->id != 0 ? command.placement : 0;
    placement.order = ++graphics.clock;
    image->used = graphics.clock;
    if (!placement.isVirtual) {
        placement.line = screenTop(vterm, vterm->active) + vterm->cursor.row;
        placement.column = vterm->cursor.column;
        storeClip(&store, vterm->active == VtermScreenKind_Primary ? int64_t(vterm->scrollbackDropped) : 0, INT64_MAX);
    }
    for (uint32_t i = store.placements.count; i-- > 0;) {
        const VtermStoredPlacement& old = store.placements.data[i];
        if (placement.id != 0 && old.image == placement.image && old.id == placement.id)
            store.placements.erase(i);
    }
    if (store.placements.count >= kMaxPlacements)
        store.placements.erase(0);
    store.placements.push(placement);
    freeUnnamed(vterm, &store);
    vtermRevise(vterm);
    if (!placement.isVirtual && command.cursorPolicy != 1)
        cursorPast(vterm, placement.columns, placement.rows);
    return "OK";
}

// Runs a transmission and answers it. `data` holds the bytes of a direct transmission. For a file, `payload` holds the
// path in base64.
void transmit(Vterm* vterm, const GraphicsCommand& command, RawArray<uint8_t>* data, PrStringView payload) {
    VtermGraphics& graphics = vterm->graphics;
    VtermGraphicsStore& store = activeStore(vterm);
    uint32_t id = command.id;
    PrStringView error{};
    if (command.hasId && command.hasNumber)
        error = "EINVAL:both an id and a number";
    else if (command.medium == 's')
        error = "EINVAL:shared memory is not supported";
    else if (command.medium != 'd' && command.medium != 'f' && command.medium != 't')
        error = "EINVAL:unsupported medium";
    if (error.size == 0 && command.medium != 'd') {
        data->clear();
        if (!readFile(vterm, command, payload, graphics.budget, data))
            error = kFileError;
    }
    VtermStoredImage image{};
    if (error.size == 0) {
        PrStringView decoded = decodeImage(vterm, command, data, &image);
        if (decoded != "OK")
            error = decoded;
    }
    if (error.size != 0 || command.action == 'q') {
        image.pixels.reset();
        answer(vterm, command, id, error.size != 0 ? error : PrStringView("OK"));
        return;
    }
    if (!command.hasId && !command.hasNumber && command.action != 'T') {
        // Nothing could name the image.
        image.pixels.reset();
        return;
    }
    if (command.hasId) {
        for (uint32_t i = 0; i < store.images.count; ++i) {
            if (store.images.data[i].id == id) {
                imageRemove(vterm, &store, i);
                break;
            }
        }
    } else if (command.hasNumber) {
        id = givenId(&graphics, &store);
    }
    if (store.images.count >= kMaxImages)
        imageRemove(vterm, &store, 0);
    image.id = id;
    image.number = command.number;
    image.key = ++graphics.clock;
    image.used = image.key;
    store.images.push(image);
    graphics.bytes += image.pixels.count;
    applyBudget(vterm, image.key);
    vtermRevise(vterm);
    PrStringView result = "OK";
    if (command.action == 'T') {
        VtermStoredImage* stored = imageFromKey(&store, image.key);
        PR_ASSERT(stored != nullptr);
        result = put(vterm, command, stored);
        freeUnnamed(vterm, &store);
    }
    answer(vterm, command, id, result);
}

// Deletion

// Stands for any column or any row in covers().
constexpr uint32_t kNoCell = 0xffffffffu;

// Returns true if the placement on the screen of `kind` covers the cell at `column` and `row` of the screen. If either
// is kNoCell, the placement need only cover the other: a whole column or a whole row.
bool covers(const Vterm* vterm, VtermScreenKind kind, const VtermStoredPlacement& placement, uint32_t column, uint32_t row) {
    if (placement.isVirtual)
        return false;
    int64_t top = placement.line + placement.firstRow - screenTop(vterm, kind);
    int64_t bottom = placement.line + placement.endRow - screenTop(vterm, kind);
    bool inColumn = column == kNoCell || (column >= placement.column && column - placement.column < placement.columns);
    bool inRow = row == kNoCell || (int64_t(row) >= top && int64_t(row) < bottom);
    return inColumn && inRow;
}

void deleteCommand(Vterm* vterm, const GraphicsCommand& command) {
    VtermGraphicsStore& store = activeStore(vterm);
    VtermScreenKind kind = vterm->active;
    char what = command.deletion;
    bool freeing = what >= 'A' && what <= 'Z';
    char lower = freeing ? char(what - 'A' + 'a') : what;
    // The images whose placements the deletion removes. An upper-case deletion frees those that no placement still
    // shows.
    RawArray<uint64_t> touched;
    auto removeWhere = [&](auto matches) {
        for (uint32_t i = store.placements.count; i-- > 0;) {
            const VtermStoredPlacement& placement = store.placements.data[i];
            if (!matches(placement))
                continue;
            touched.push(placement.image);
            store.placements.erase(i);
        }
    };
    switch (lower) {
        case 'a':
            removeWhere([&](const VtermStoredPlacement& p) {
                int64_t top = p.line + p.firstRow - screenTop(vterm, kind);
                int64_t bottom = p.line + p.endRow - screenTop(vterm, kind);
                return !p.isVirtual && bottom > 0 && top < int64_t(vterm->rows);
            });
            break;
        case 'i':
        case 'n': {
            VtermStoredImage* image = nullptr;
            if (lower == 'i' && command.hasId)
                image = imageFromId(&store, command.id);
            else if (lower == 'n' && command.hasNumber)
                image = imageFromNumber(&store, command.number);
            if (image == nullptr)
                break;
            uint64_t key = image->key;
            removeWhere([&](const VtermStoredPlacement& p) {
                return p.image == key && (command.placement == 0 || p.id == command.placement);
            });
            touched.push(key);
            break;
        }
        case 'c':
            removeWhere([&](const VtermStoredPlacement& p) {
                return covers(vterm, kind, p, vterm->cursor.column, vterm->cursor.row);
            });
            break;
        case 'p':
        case 'q':
            if (command.x == 0 || command.y == 0)
                break;
            removeWhere([&](const VtermStoredPlacement& p) {
                return covers(vterm, kind, p, command.x - 1, command.y - 1) && (lower == 'p' || p.z == command.z);
            });
            break;
        case 'x':
            if (command.x != 0)
                removeWhere([&](const VtermStoredPlacement& p) { return covers(vterm, kind, p, command.x - 1, kNoCell); });
            break;
        case 'y':
            if (command.y != 0)
                removeWhere([&](const VtermStoredPlacement& p) { return covers(vterm, kind, p, kNoCell, command.y - 1); });
            break;
        case 'z':
            removeWhere([&](const VtermStoredPlacement& p) { return !p.isVirtual && p.z == command.z; });
            break;
        case 'r':
            for (const VtermStoredImage& image : store.images.span()) {
                if (image.id != 0 && image.id >= command.x && image.id <= command.y)
                    touched.push(image.key);
            }
            removeWhere([&](const VtermStoredPlacement& p) { return touched.find(p.image) != kArrayMissing; });
            break;
        default:
            break;
    }
    if (freeing) {
        for (uint64_t key : touched.span()) {
            for (uint32_t i = 0; i < store.images.count; ++i) {
                if (store.images.data[i].key == key && !hasPlacements(&store, key)) {
                    imageRemove(vterm, &store, i);
                    break;
                }
            }
        }
    }
    touched.reset();
    freeUnnamed(vterm, &store);
    vtermRevise(vterm);
}

// Feature queries

// Returns true if the terminal supports `value` of `key` for the action `action`.
bool supportsKey(const Vterm* vterm, char action, char key, PrStringView value) {
    const VtermGraphics& graphics = vterm->graphics;
    bool transmits = action == 't' || action == 'T' || action == 'q';
    bool places = action == 'T' || action == 'p';
    switch (key) {
        case 'f':
            return transmits && (value == "24" || value == "32" || (value == "100" && graphics.imageCodec != nullptr));
        case 't':
            return transmits && (value == "f" || value == "t") && (graphics.flags & PrVtermImageFlag_Files) != 0;
        case 'o':
            return transmits && value == "z" && graphics.compressCodec != nullptr;
        case 's':
        case 'v':
        case 'S':
        case 'O':
            return transmits;
        case 'U':
        case 'C':
            return places && value == "1";
        case 'c':
        case 'r':
        case 'X':
        case 'Y':
        case 'p':
            return places;
        case 'x':
        case 'y':
        case 'z':
            return places || action == 'd';
        case 'w':
        case 'h':
            return places;
        case 'd':
            return action == 'd' && value.size == 1 && contains("aAiInNcCpPqQrRxXyYzZ", value);
        default:
            return false;
    }
}

// Answers a feature query. The answer repeats `e`, and lists the keys of the query that the terminal supports for the
// action that `e` names.
void featureQuery(Vterm* vterm, const GraphicsCommand& command) {
    if (!command.hasId || command.quiet >= 1)
        return;
    char action = command.feature;
    bool known = action == 't' || action == 'T' || action == 'q' || action == 'p' || action == 'd';
    vtermPut(vterm, "\x1b_Gi=");
    vtermPutNumber(vterm, command.id);
    if (known) {
        vtermPut(vterm, ",e=");
        vtermPutByte(vterm, uint8_t(action));
        PrStringView control = command.control;
        while (control.size != 0) {
            PrStringView field = splitAt(&control, ',');
            if (field.size < 3 || field.data[1] != '=')
                continue;
            char key = field.data[0];
            PrStringView value{field.data + 2, field.size - 2};
            bool omitted = key == 'a' || key == 'i' || key == 'I' || key == 'm' || key == 'q' || key == 'e' ||
                value == "0" || (key == 'f' && value == "32") || (key == 't' && value == "d");
            if (omitted || !supportsKey(vterm, action, key, value))
                continue;
            vtermPutByte(vterm, ',');
            vtermPut(vterm, field);
        }
    }
    vtermPut(vterm, ";OK\x1b\\");
}

// Runs a command that is not a later chunk of a load.
void run(Vterm* vterm, const GraphicsCommand& command, PrStringView payload) {
    VtermGraphics& graphics = vterm->graphics;
    if (command.invalid) {
        answer(vterm, command, command.id, "EINVAL:a key's value does not parse");
        return;
    }
    switch (command.action) {
        case 't':
        case 'T':
        case 'q': {
            if (command.action == 'q' && command.feature != 0) {
                featureQuery(vterm, command);
                return;
            }
            loadDrop(&graphics.load);
            if (command.medium == 'd') {
                graphics.load.active = true;
                graphics.load.control.append(command.control.data, uint32_t(command.control.size));
                if (!loadAppend(&graphics.load, payload, graphics.budget))
                    graphics.load.failed = true;
                if (command.more == 1)
                    return;
                bool decoded = loadFinish(&graphics.load) && !graphics.load.failed;
                if (!decoded) {
                    answer(vterm, command, command.id, loadError(graphics.load));
                    loadDrop(&graphics.load);
                    return;
                }
                RawArray<uint8_t> data = graphics.load.data;
                graphics.load.data = RawArray<uint8_t>{};
                loadDrop(&graphics.load);
                transmit(vterm, command, &data, payload);
                data.reset();
                return;
            }
            RawArray<uint8_t> data;
            transmit(vterm, command, &data, payload);
            data.reset();
            return;
        }
        case 'p': {
            loadDrop(&graphics.load);
            VtermStoredImage* image = imageOf(&activeStore(vterm), command);
            uint32_t id = image != nullptr ? image->id : command.id;
            PrStringView result = "ENOENT:no such image";
            if (command.hasId && command.hasNumber)
                result = "EINVAL:both an id and a number";
            else if (image != nullptr)
                result = put(vterm, command, image);
            answer(vterm, command, id, result);
            return;
        }
        case 'd':
            loadDrop(&graphics.load);
            deleteCommand(vterm, command);
            return;
        default:
            loadDrop(&graphics.load);
            answer(vterm, command, command.id, "EINVAL:the action is not supported");
            return;
    }
}

// Converts a line number on the screen of `kind` to an index among the lines that prVtermLine reads.
int64_t lineIndex(const Vterm* vterm, VtermScreenKind kind, int64_t line) {
    if (kind == VtermScreenKind_Alternate)
        return int64_t(scrollbackCount(vterm)) + line;
    return line - int64_t(vterm->scrollbackDropped);
}

// Returns the number that a color encodes in a placeholder cell: its palette index or its 24-bit RGB value, or zero for
// the default color.
uint32_t colorId(PrVtermColor color) {
    switch (color.kind) {
        case PrVtermColorKind_Palette:
            return prVtermColorIndex(color);
        case PrVtermColorKind_Rgb:
            return uint32_t(color.red) << 16 | uint32_t(color.green) << 8 | color.blue;
        default:
            return 0;
    }
}

bool isSameColor(PrVtermColor a, PrVtermColor b) {
    return a.kind == b.kind && colorId(a) == colorId(b);
}

// The numbers of a placeholder cell, including those that it inherits from the cell before it.
struct PlaceholderCell {
    bool valid;
    PrVtermColor foreground;
    PrVtermColor underline;
    uint32_t row;
    uint32_t column;
    uint32_t high;              // the most significant byte of the image's id
};

// Reads the placeholder at `column` of `line`. The numbers that its diacritics leave out come from `left`, the
// placeholder before it, if the two continue one run.
PlaceholderCell placeholderRead(const PrVtermLine& line, uint32_t column, const PlaceholderCell& left) {
    const PrVtermCell& cell = line.cells[column];
    uint32_t numbers[3] = {kDiacriticMissing, kDiacriticMissing, kDiacriticMissing};
    uint32_t count = 0;
    while (count < 3 && count < cell.clusterCount) {
        uint32_t number = diacriticIndex(line.codePoints[cell.clusterStart + count]);
        if (number == kDiacriticMissing)
            break;
        numbers[count++] = number;
    }
    PlaceholderCell read{.valid = true, .foreground = cell.foreground, .underline = cell.underlineColor};
    bool sameColors = left.valid && isSameColor(left.foreground, read.foreground) &&
        isSameColor(left.underline, read.underline);
    if (count == 0) {
        if (sameColors) {
            read.row = left.row;
            read.column = left.column + 1;
            read.high = left.high;
        }
        return read;
    }
    read.row = numbers[0];
    bool sameRow = sameColors && left.row == read.row;
    if (count == 1) {
        if (sameRow) {
            read.column = left.column + 1;
            read.high = left.high;
        }
        return read;
    }
    read.column = numbers[1];
    if (count == 2) {
        if (sameRow && left.column + 1 == read.column)
            read.high = left.high;
        return read;
    }
    read.high = numbers[2];
    return read;
}

// Returns the virtual placement with placement id `placement` of the image with id `id`, or any virtual placement of
// the image if `placement` is zero. Writes the image to `*image` if the image exists.
const VtermStoredPlacement* virtualPlacement(const VtermGraphicsStore* store, uint32_t id, uint32_t placement,
        const VtermStoredImage** image) {
    const VtermStoredImage* found = nullptr;
    for (const VtermStoredImage& candidate : store->images.span()) {
        if (candidate.id == id)
            found = &candidate;
    }
    if (found == nullptr)
        return nullptr;
    const VtermStoredPlacement* chosen = nullptr;
    for (const VtermStoredPlacement& candidate : store->placements.span()) {
        if (candidate.isVirtual && candidate.image == found->key && (placement == 0 || candidate.id == placement))
            chosen = &candidate;
    }
    *image = found;
    return chosen;
}

// A placement as the caller reads it, with the image id and the order that sort it among the others.
struct Shown {
    PrVtermPlacement placement;
    uint32_t imageId;
    uint64_t order;
};

// Appends a placement for each run of placeholder cells on line `index`, a line of the scrollback or of the screen of
// `kind`.
void appendPlaceholders(const Vterm* vterm, VtermScreenKind kind, uint32_t index, RawArray<Shown>* out) {
    uint32_t scrollback = scrollbackCount(vterm);
    const VtermGraphicsStore* store = &vterm->graphics.stores[index < scrollback ? VtermScreenKind_Primary : kind];
    if (store->placements.count == 0)
        return;
    PrVtermLine line{};
    if (index < scrollback) {
        line = vtermLineRead(vterm, index);
    } else {
        const VtermStoredLine& row = vtermRowsAt(&vterm->screens[kind].rows, index - scrollback);
        line = PrVtermLine{.cells = row.cells.data, .cellCount = row.cells.count, .codePoints = row.codePoints.data};
    }
    PlaceholderCell left{};
    Shown* run = nullptr;
    PlaceholderCell runLast{};
    uint32_t runId = 0;
    for (uint32_t column = 0; column < line.cellCount; ++column) {
        if (line.cells[column].codePoint != kPlaceholder) {
            left = PlaceholderCell{};
            run = nullptr;
            continue;
        }
        PlaceholderCell cell = placeholderRead(line, column, left);
        left = cell;
        uint32_t id = colorId(cell.foreground) | cell.high << 24;
        bool continues = run != nullptr && runId == id && isSameColor(runLast.underline, cell.underline) &&
            runLast.row == cell.row && runLast.column + 1 == cell.column;
        runLast = cell;
        if (continues) {
            ++run->placement.columns;
            continue;
        }
        run = nullptr;
        const VtermStoredImage* image = nullptr;
        const VtermStoredPlacement* prototype = virtualPlacement(store, id, colorId(cell.underline), &image);
        if (prototype == nullptr)
            continue;
        PrVtermPlacement shown{
            .image = image->key,
            .sourceOrigin = prototype->sourceOrigin,
            .sourceExtent = prototype->sourceExtent,
            .line = index,
            .column = column,
            .offset = PrVec2{prototype->offset.x - float(cell.column), prototype->offset.y - float(cell.row)},
            .extent = prototype->extent,
            .columns = 1,
            .firstRow = 0,
            .endRow = 1,
            .z = prototype->z,
        };
        out->push(Shown{shown, image->id, prototype->order});
        run = &out->last();
        runId = id;
    }
}

} // namespace

void vtermGraphicsInit(VtermGraphics* graphics, const PrVtermDesc* desc) {
    graphics->budget = desc->imageBytes;
    graphics->flags = desc->imageFlags;
    graphics->compressCodec = desc->compressCodec;
    graphics->imageCodec = desc->imageCodec;
    graphics->cellPixels = kDefaultCellPixels;
    graphics->nextId = kFirstGivenId;
}

void vtermGraphicsReset(VtermGraphics* graphics) {
    for (VtermGraphicsStore& store : graphics->stores) {
        for (VtermStoredImage& image : store.images.span())
            image.pixels.reset();
        store.images.reset();
        store.placements.reset();
    }
    graphics->load.control.reset();
    graphics->load.data.reset();
    graphics->bytes = 0;
}

void vtermGraphicsClear(Vterm* vterm) {
    for (VtermGraphicsStore& store : vterm->graphics.stores)
        storeReset(vterm, &store);
    loadDrop(&vterm->graphics.load);
}

void vtermGraphicsCommand(Vterm* vterm, PrStringView text) {
    VtermGraphics& graphics = vterm->graphics;
    if (graphics.budget == 0)
        return;
    PrStringView payload = text;
    PrStringView control = splitAt(&payload, ';');
    GraphicsCommand command = commandDefault();
    commandParse(control, &command);
    if (!graphics.load.active || !command.continues) {
        run(vterm, command, payload);
        return;
    }
    // A later chunk of the load carries only `m` and `q`. The other keys come from the load's first chunk.
    if (command.invalid || !loadAppend(&graphics.load, payload, graphics.budget))
        graphics.load.failed = true;
    if (command.more == 1)
        return;
    GraphicsCommand first = commandDefault();
    PrStringView firstControl{graphics.load.control.data, graphics.load.control.count};
    commandParse(firstControl, &first);
    bool decoded = loadFinish(&graphics.load) && !graphics.load.failed;
    PrStringView error = loadError(graphics.load);
    RawArray<uint8_t> data = graphics.load.data;
    RawArray<char> keys = graphics.load.control;
    graphics.load.data = RawArray<uint8_t>{};
    graphics.load.control = RawArray<char>{};
    loadDrop(&graphics.load);
    first.control = PrStringView{keys.data, keys.count};
    if (decoded)
        transmit(vterm, first, &data, PrStringView{});
    else
        answer(vterm, first, first.id, error);
    data.reset();
    keys.reset();
}

void vtermGraphicsScroll(Vterm* vterm, uint32_t top, uint32_t bottom, int32_t delta, bool scrollback) {
    VtermGraphicsStore& store = activeStore(vterm);
    if (store.placements.count == 0 || delta == 0)
        return;
    int64_t base = screenTop(vterm, vterm->active);
    bool spansWidth = vterm->left == 0 && vterm->right == vterm->columns;
    for (uint32_t i = store.placements.count; i-- > 0;) {
        VtermStoredPlacement& placement = store.placements.data[i];
        if (placement.isVirtual)
            continue;
        int64_t first = placement.line + placement.firstRow - base;
        int64_t end = placement.line + placement.endRow - base;
        if (end <= 0)
            continue;
        bool inRows = (top == 0 || first >= int64_t(top)) && (bottom == vterm->rows || end <= int64_t(bottom));
        bool inColumns = spansWidth || (placement.column >= vterm->left && placement.column < vterm->right &&
            placement.columns <= vterm->right - placement.column);
        if (!inRows || !inColumns) {
            // The lines that enter the scrollback renumber the screen's rows. This placement stays at its row, so its
            // line number changes.
            if (scrollback)
                placement.line -= delta;
            continue;
        }
        if (scrollback)
            continue;
        placement.line += delta;
        first += delta;
        end += delta;
        if (delta < 0 && first < int64_t(top))
            first = top;
        if (delta > 0 && end > int64_t(bottom))
            end = bottom;
        if (first >= end) {
            store.placements.erase(i);
            continue;
        }
        placement.firstRow = uint32_t(first + base - placement.line);
        placement.endRow = uint32_t(end + base - placement.line);
    }
    freeUnnamed(vterm, &store);
}

void vtermGraphicsEraseScreen(Vterm* vterm) {
    VtermGraphicsStore& store = activeStore(vterm);
    int64_t base = screenTop(vterm, vterm->active);
    for (uint32_t i = store.placements.count; i-- > 0;) {
        const VtermStoredPlacement& placement = store.placements.data[i];
        if (!placement.isVirtual && placement.line + placement.endRow > base &&
                placement.line + placement.firstRow < base + vterm->rows)
            store.placements.erase(i);
    }
    freeUnnamed(vterm, &store);
}

void vtermGraphicsEraseScrollback(Vterm* vterm) {
    VtermGraphicsStore& store = vterm->graphics.stores[VtermScreenKind_Primary];
    storeClip(&store, screenTop(vterm, VtermScreenKind_Primary), INT64_MAX);
    freeUnnamed(vterm, &store);
}

void vtermGraphicsClearAlternate(Vterm* vterm) {
    storeReset(vterm, &vterm->graphics.stores[VtermScreenKind_Alternate]);
}

void vtermGraphicsRewrap(Vterm* vterm, uint64_t firstLine, const RawArray<uint32_t>& map, uint64_t newFirstLine) {
    if (map.count == 0)
        return;
    for (VtermStoredPlacement& placement : vterm->graphics.stores[VtermScreenKind_Primary].placements.span()) {
        if (placement.isVirtual)
            continue;
        int64_t index = placement.line - int64_t(firstLine);
        int64_t mapped = 0;
        if (index < 0)
            mapped = int64_t(map.data[0]) + index;
        else if (index >= int64_t(map.count))
            mapped = int64_t(map.last()) + (index - int64_t(map.count - 1));
        else
            mapped = map.data[index];
        placement.line = int64_t(newFirstLine) + mapped;
    }
}

void vtermGraphicsShiftAlternate(Vterm* vterm, uint32_t rows) {
    for (VtermStoredPlacement& placement : vterm->graphics.stores[VtermScreenKind_Alternate].placements.span()) {
        if (!placement.isVirtual)
            placement.line -= rows;
    }
}

void vtermGraphicsPrune(Vterm* vterm) {
    VtermGraphicsStore* stores = vterm->graphics.stores;
    storeClip(&stores[VtermScreenKind_Primary], int64_t(vterm->scrollbackDropped),
        int64_t(vterm->scrollbackDropped + vtermLineCount(vterm)));
    storeClip(&stores[VtermScreenKind_Alternate], 0, vterm->rows);
    for (uint32_t i = 0; i < 2; ++i)
        freeUnnamed(vterm, &stores[i]);
}

} // namespace pr_impl

using namespace pr_impl;

PrUVec2 prVtermCellPixelExtent(const PrVterm* vterm) {
    return vterm->graphics.cellPixels;
}

void prVtermSetCellPixelExtent(PrVterm* vterm, PrUVec2 extent) {
    PR_ASSERT(extent.x > 0 && extent.y > 0);
    vterm->graphics.cellPixels = extent;
}

bool prVtermImage(const PrVterm* vterm, uint64_t key, PrVtermImage* out) {
    for (const VtermGraphicsStore& store : vterm->graphics.stores) {
        for (const VtermStoredImage& image : store.images.span()) {
            if (image.key == key) {
                *out = PrVtermImage{.extent = image.extent, .pixels = image.pixels.data};
                return true;
            }
        }
    }
    return false;
}

uint32_t prVtermPlacements(const PrVterm* vterm, uint32_t first, uint32_t count, PrVtermPlacement* PR_NULLABLE out,
        uint32_t capacity) {
    uint32_t scrollback = scrollbackCount(vterm);
    uint32_t lines = scrollback + vterm->rows;
    PR_ASSERT(first <= lines && count <= lines - first);
    RawArray<Shown> shown;
    for (uint32_t kind = 0; kind < 2; ++kind) {
        const VtermGraphicsStore& store = vterm->graphics.stores[kind];
        if (kind == VtermScreenKind_Alternate && vterm->active != VtermScreenKind_Alternate)
            continue;
        // A screen shows its placements on its own lines: the primary screen's in the scrollback, and on the screen's
        // rows while it shows.
        int64_t allowedFirst = kind == VtermScreenKind_Primary ? 0 : scrollback;
        int64_t allowedEnd = lines;
        if (kind == VtermScreenKind_Primary && vterm->active != VtermScreenKind_Primary)
            allowedEnd = scrollback;
        for (const VtermStoredPlacement& placement : store.placements.span()) {
            if (placement.isVirtual)
                continue;
            int64_t index = lineIndex(vterm, VtermScreenKind(kind), placement.line);
            int64_t top = index + placement.firstRow;
            int64_t bottom = index + placement.endRow;
            if (top < allowedFirst)
                top = allowedFirst;
            if (bottom > allowedEnd)
                bottom = allowedEnd;
            if (top >= bottom || bottom <= int64_t(first) || top >= int64_t(first) + count)
                continue;
            const VtermStoredImage* image = nullptr;
            for (const VtermStoredImage& candidate : store.images.span()) {
                if (candidate.key == placement.image)
                    image = &candidate;
            }
            PR_ASSERT(image != nullptr);
            PrVtermPlacement entry{
                .image = placement.image,
                .sourceOrigin = placement.sourceOrigin,
                .sourceExtent = placement.sourceExtent,
                .line = index,
                .column = placement.column,
                .offset = placement.offset,
                .extent = placement.extent,
                .columns = placement.columns,
                .firstRow = uint32_t(top - index),
                .endRow = uint32_t(bottom - index),
                .z = placement.z,
            };
            shown.push(Shown{entry, image->id, placement.order});
        }
    }
    for (uint32_t index = first; index < first + count; ++index)
        appendPlaceholders(vterm, vterm->active, index, &shown);
    sortItems(shown.span(), [](const Shown& a, const Shown& b) {
        if (a.placement.z != b.placement.z)
            return a.placement.z < b.placement.z;
        if (a.imageId != b.imageId)
            return a.imageId < b.imageId;
        if (a.order != b.order)
            return a.order < b.order;
        if (a.placement.line != b.placement.line)
            return a.placement.line < b.placement.line;
        return a.placement.column < b.placement.column;
    });
    for (uint32_t i = 0; i < shown.count && i < capacity; ++i)
        out[i] = shown.data[i].placement;
    uint32_t total = shown.count;
    shown.reset();
    return total;
}

// The data's pages: the clipboard in each form and the selection, drags into the window, and file dialogs, the file
// manager and the trash.

#include "examples/app_features/pages_data.h"

#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/integer.h"
#include "pr/kit_box.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"
#include "pr/print.h"
#include "pr/raw_string.h"
#include "pr/ui.h"

#include "examples/app_features/features.h"

namespace features {

using namespace pr;

namespace {

// The type of the custom content that the Clipboard page writes and reads.
constexpr PrStringView kCustomType = "org.prax.app-features";

// The content of a file that a Save dialog chose.
constexpr PrStringView kSaved = "Saved by app_features\n";

PrByteView bytesOf(PrStringView text) {
    return PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size};
}

// Returns the names of `forms`, joined by spaces, or none.
String formNames(PrAppDataFormFlags forms) {
    String names;
    for (PrAppDataFormFlags rest = forms; rest != 0; rest = PrAppDataFormFlags(rest & (rest - 1))) {
        if (!names.isEmpty())
            names.append(" ");
        names.append(prAppDataFormName(PrAppDataFormFlags(1u << prU64LowestBit(rest))));
    }
    return names.isEmpty() ? String("none") : names;
}

void answered(Features& f, String text) {
    f.clipboardRead = text;
    logLine(f, text);
}

void readForms(void* context, PrStatus status, PrAppDataFormFlags forms) {
    answered(featuresOf(context), format("forms: {} {}", statusName(status), formNames(forms)));
}

void readText(void* context, PrStatus status, PrStringView text) {
    answered(featuresOf(context), format("text: {} \"{}\"", statusName(status), text));
}

void readHtml(void* context, PrStatus status, PrStringView html) {
    answered(featuresOf(context), format("html: {} {}", statusName(status), html));
}

void readImage(void* context, PrStatus status, PrPixelView pixels) {
    answered(featuresOf(context), format("image: {} {} x {}", statusName(status), pixels.extent.x, pixels.extent.y));
}

void readCustom(void* context, PrStatus status, PrByteView bytes) {
    String text(PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size});
    answered(featuresOf(context), format("custom: {} \"{}\"", statusName(status), text));
}

// Logs the first bytes of a file, which its source reads whether or not the file has a path.
void fileBytes(void* context, PrStatus status, PrByteView bytes) {
    String hex;
    for (size_t i = 0; i < bytes.size; ++i)
        hex.append(format("{:02x} ", bytes.data[i]));
    logLine(featuresOf(context), format("  first bytes: {} {}", statusName(status), hex));
}

// Logs the files of a source, and keeps the first path for the file manager.
void sourceFiles(void* context, PrStatus status, const PrAppDataFile* PR_NULLABLE files, uint32_t count) {
    Features& f = featuresOf(context);
    logLine(f, format("files: {} {}", statusName(status), count));
    for (uint32_t i = 0; i < count; ++i) {
        logLine(f, format("  {} ({} bytes{}) {}", files[i].name, files[i].size, files[i].directory ? ", directory" : "",
            files[i].path.isEmpty() ? PrStringView("without a path") : files[i].path));
    }
    if (count != 0 && !files[0].path.isEmpty())
        f.lastPath = String(files[0].path);
}

void written(void* context, PrStatus status) {
    logLine(featuresOf(context), format("written: {}", statusName(status)));
}

// Reads the files of a dialog's source, or writes the file that a Save dialog chose.
void dialogAnswered(void* context, PrStatus status, PrAppDataSource source) {
    Features& f = featuresOf(context);
    if (status != PrStatus_Ok || source == kPrAppDataSourceNone) {
        logLine(f, format("dialog: {}", status != PrStatus_Ok ? statusName(status) : PrStringView("canceled")));
        return;
    }
    PrApp* app = appOf(f);
    prAppDataReadFiles(app, source, sourceFiles, &f);
    if (f.saving)
        prAppDataWriteFile(app, source, 0, 0, bytesOf(kSaved), written, &f);
    else
        prAppDataReadFile(app, source, 0, 0, 16, fileBytes, &f);
}

void droppedText(void* context, PrStatus status, PrStringView text) {
    logLine(featuresOf(context), format("drop's text: {} \"{}\"", statusName(status), text));
}

// Writes a new file in the user's temporary directory, and returns its path, or empty if it cannot be written.
String temporaryFile(uint32_t number) {
    PrRawString directory = prFileUserDirectory(PrFileUserDirectory_Temporary);
    String path = format("{}/app_features_trash_{}.txt", prRawStringView(&directory), number);
    prRawStringReset(&directory);
    return prFileWrite(path, bytesOf("To be trashed by app_features\n")) ? path : String();
}

} // namespace

void showDialog(Features& f, PrAppFileDialogKind kind, PrAppFileDialogFlags flags) {
    static const PrAppFileType kImages[] = {{.name = "Images", .extensions = "png,jpg,jpeg,gif"}};
    static const PrAppFileType kText[] = {{.name = "Text", .extensions = "txt"}};
    f.saving = kind == PrAppFileDialogKind_Save;
    bool images = !f.saving && (flags & PrAppFileDialogFlag_Multiple) != 0;
    PrStringView title = "Open a file";
    if (f.saving)
        title = "Save a file";
    else if (images)
        title = "Open one or more images";
    else if ((flags & PrAppFileDialogFlag_Directories) != 0)
        title = "Open a folder";
    PrAppFileDialogDesc desc{
        .kind = kind,
        .flags = flags,
        .parent = controlOf(f),
        .title = title,
        .name = f.saving ? "untitled.txt" : "",
    };
    if (f.saving) {
        desc.types = kText;
        desc.typeCount = uint32_t(countOf(kText));
    } else if (images) {
        desc.types = kImages;
        desc.typeCount = uint32_t(countOf(kImages));
    }
    prAppFileDialog(appOf(f), &desc, dialogAnswered, &f);
}

void pageClipboard(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrApp* app = appOf(f);
    PrLayoutNode w = section(kit, parent, "WRITE");
    PrLayoutNode writes = row(kit, w);
    if (button(kit, writes, "Text"))
        logLine(f, format("write text: {}", statusName(prAppDataWriteText(app, kPrAppDataSourceClipboard,
            "Copied by app_features"))));
    if (button(kit, writes, "HTML and text")) {
        PrAppDataContent content{
            .forms = PrAppDataFormFlag_Text | PrAppDataFormFlag_Html,
            .text = "Bold and plain",
            .html = "<b>Bold</b> and plain",
        };
        logLine(f, format("write html: {}", statusName(prAppDataWrite(app, kPrAppDataSourceClipboard, &content))));
    }
    if (button(kit, writes, "URL")) {
        PrStringView urls[] = {"https://example.com/"};
        PrAppDataContent content{
            .forms = PrAppDataFormFlag_Text | PrAppDataFormFlag_Urls,
            .text = urls[0],
            .urls = urls,
            .urlCount = 1,
        };
        logLine(f, format("write url: {}", statusName(prAppDataWrite(app, kPrAppDataSourceClipboard, &content))));
    }
    if (button(kit, writes, "Image")) {
        Array<uint8_t> pixels;
        PrAppDataContent content{
            .forms = PrAppDataFormFlag_Image,
            .image = iconPixels(pixels, 64, PrColor{1, 0.5f, 0.2f, 1}),
        };
        logLine(f, format("write image: {}", statusName(prAppDataWrite(app, kPrAppDataSourceClipboard, &content))));
    }
    if (button(kit, writes, "Custom")) {
        PrAppDataCustom custom[] = {{.type = kCustomType, .bytes = bytesOf("custom bytes")}};
        PrAppDataContent content{
            .forms = PrAppDataFormFlag_Custom | PrAppDataFormFlag_Text,
            .text = "Custom content, with text for other applications",
            .custom = custom,
            .customCount = 1,
        };
        logLine(f, format("write custom: {}", statusName(prAppDataWrite(app, kPrAppDataSourceClipboard, &content))));
    }

    PrLayoutNode r = section(kit, parent, "READ");
    PrLayoutNode reads = row(kit, r);
    if (button(kit, reads, "Forms"))
        prAppDataReadForms(app, kPrAppDataSourceClipboard, readForms, &f);
    if (button(kit, reads, "Text"))
        prAppDataReadText(app, kPrAppDataSourceClipboard, readText, &f);
    if (button(kit, reads, "HTML"))
        prAppDataReadHtml(app, kPrAppDataSourceClipboard, readHtml, &f);
    if (button(kit, reads, "Image"))
        prAppDataReadImage(app, kPrAppDataSourceClipboard, readImage, &f);
    if (button(kit, reads, "Custom"))
        prAppDataReadCustom(app, kPrAppDataSourceClipboard, kCustomType, readCustom, &f);
    line(kit, r, f.clipboardRead.isEmpty() ? PrStringView("nothing read yet") : PrStringView(f.clipboardRead));
    note(kit, r, "The web reads and writes the clipboard only during a press or a key, and a browser may ask before "
        "a read. It holds text, HTML and PNG images, and custom content only in Chrome. It cannot tell when the "
        "clipboard changes.");

    line(kit, r, format("clipboard changes: {}", f.clipboardChanges));

    PrLayoutNode s = section(kit, parent, "PRIMARY SELECTION");
    PrLayoutNode selection = row(kit, s);
    if (button(kit, selection, "Write"))
        logLine(f, format("write selection: {}", statusName(prAppDataWriteText(app, kPrAppDataSourceSelection,
            "Selected by app_features"))));
    if (button(kit, selection, "Read"))
        prAppDataReadText(app, kPrAppDataSourceSelection, readText, &f);
    note(kit, s, "The selection is the text that a middle click pastes on X11 and Wayland. Elsewhere it reads as empty, "
        "and a write answers NotSupported.");
}

void pageDrags(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrUi* ui = prStudioWindowUi(window);
    PrApp* app = appOf(f);

    PrLayoutNode in = section(kit, parent, "DROP INTO THE WINDOW");
    const PrUiPayload* dragging = prUiDragging(ui);
    bool over = false;
    PrLayoutNode zone = prKitSurface(kit, in, {}, PrKitSurfaceStyle{
        .box = {.align = PrKitAlign_Center, .padding = prKitEdgesAll(kPrKitSpace5)},
        .background = PrKitBackground_Raised,
        .rules = PrKitRuleFlag_All,
    });
    if (dragging != nullptr && dragging->kind == kPrUiPayloadKindSystem)
        over = prUiIsDropTarget(ui, zone, kPrUiPayloadKindSystem);
    prKitLabel(kit, zone, {.text = over ? "Release to drop" : "Drop files or text here"}, {.tone = PrKitTone_Muted});
    if (const PrUiPayload* dropped = prUiDropped(ui, zone)) {
        if ((dropped->forms & PrAppDataFormFlag_Files) != 0) {
            prAppDataReadFiles(app, dropped->drop, sourceFiles, &f);
            prAppDataReadFile(app, dropped->drop, 0, 0, 16, fileBytes, &f);
        }
        if ((dropped->forms & PrAppDataFormFlag_Text) != 0)
            prAppDataReadText(app, dropped->drop, droppedText, &f);
    }
}

void pageFiles(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode s = section(kit, parent, "FILE DIALOGS");
    PrLayoutNode r = row(kit, s);
    if (button(kit, r, "Open a file"))
        showDialog(f, PrAppFileDialogKind_Open, 0);
    if (button(kit, r, "Open images"))
        showDialog(f, PrAppFileDialogKind_Open, PrAppFileDialogFlag_Multiple);
    if (button(kit, r, "Open folder"))
        showDialog(f, PrAppFileDialogKind_Open, PrAppFileDialogFlag_Directories);
    if (button(kit, r, "Save as"))
        showDialog(f, PrAppFileDialogKind_Save, 0);
    note(kit, s, "Open a file takes one file of any type. Open images takes several, of PNG, JPEG and GIF only. A "
        "dialog's answer is a source of files, which the program reads and writes through the source, as a sandboxed "
        "application and the web must. The log lists each file. Save as writes a line to the file that it chose. The "
        "web's files have no paths, and only Chrome saves to the file chosen: other browsers save a download.");

    PrApp* app = appOf(f);
    PrLayoutNode m = section(kit, parent, "FILE MANAGER AND TRASH");
    line(kit, m, f.lastPath.isEmpty() ? PrStringView("no path yet") : PrStringView(f.lastPath));
    PrLayoutNode manager = row(kit, m);
    if (button(kit, manager, "Reveal", f.lastPath.isEmpty()))
        logLine(f, format("reveal {}: {}", f.lastPath, statusName(prAppRevealPath(app, f.lastPath))));
    if (button(kit, manager, "Trash a new temporary file")) {
        String path = temporaryFile(++f.trashed);
        logLine(f, format("trash {}: {}", path, statusName(path.isEmpty() ? PrStatus_Io : prAppTrashPath(app, path))));
    }
    note(kit, m, "The temporary file lands in the Trash or the Recycle Bin, where it can be restored. The web cannot "
        "reveal a file or move it to the trash.");
}

bool dataAppEvent(Features& f, const PrAppEvent& e) {
    if (e.kind != PrAppEventKind_ClipboardChanged)
        return false;
    ++f.clipboardChanges;
    return true;
}

} // namespace features

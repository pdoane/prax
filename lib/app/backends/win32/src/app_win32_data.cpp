// The Win32 backend's data transfer: the clipboard, drags and drops, reads and writes of sources, and file dialogs.

#include "app/backends/win32/src/app_win32_data.h"

#include "app/backends/win32/src/app_win32_input.h"
#include "app/backends/win32/src/app_win32_window.h"

namespace pr_impl {

namespace {

// Opens the clipboard. Another process may hold it for a moment, so the open is tried a few times.
bool openClipboard(HWND window) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(window))
            return true;
        Sleep(1);
    }
    return false;
}

// Reads a device-independent bitmap of `size` bytes as straight-alpha Rgba8 rows into `out`. Returns its extent, or
// zero for a form that it does not read. It reads 24 bits, and 32 bits with no masks or with the masks of BGRA.
PrUVec2 dibPixels(const uint8_t* bytes, size_t size, RawArray<uint8_t>& out) {
    if (size < sizeof(BITMAPINFOHEADER))
        return PrUVec2{0, 0};
    BITMAPINFOHEADER header;
    prMemCopy(&header, bytes, sizeof(header));
    bool bitfields = header.biCompression == BI_BITFIELDS;
    bool readable = header.biPlanes == 1 && (header.biCompression == BI_RGB || bitfields) &&
        (header.biBitCount == 32 || (header.biBitCount == 24 && !bitfields));
    if (!readable || header.biWidth <= 0 || header.biHeight == 0 || header.biHeight == INT32_MIN)
        return PrUVec2{0, 0};
    size_t width = size_t(header.biWidth);
    size_t height = size_t(header.biHeight < 0 ? -header.biHeight : header.biHeight);
    size_t offset = header.biSize + header.biClrUsed * sizeof(RGBQUAD);
    // A header of the first version keeps its masks after it. The later versions hold them within.
    if (bitfields && header.biSize == sizeof(BITMAPINFOHEADER))
        offset += 3 * sizeof(DWORD);
    size_t stride = ((width * header.biBitCount + 31) / 32) * 4;
    if (offset > size || stride * height > size - offset)
        return PrUVec2{0, 0};
    out.resize(uint32_t(width * height * 4));
    // A 32-bit bitmap with no masks leaves its alpha undefined, and most programs write zero there.
    bool hasAlpha = false;
    if (header.biBitCount == 32) {
        for (size_t i = offset + 3; i < offset + stride * height && !hasAlpha; i += 4)
            hasAlpha = bytes[i] != 0;
    }
    for (size_t y = 0; y < height; ++y) {
        size_t sourceRow = header.biHeight < 0 ? y : height - 1 - y;
        const uint8_t* from = bytes + offset + sourceRow * stride;
        uint8_t* to = out.data + y * width * 4;
        for (size_t x = 0; x < width; ++x) {
            to[x * 4] = from[2];
            to[x * 4 + 1] = from[1];
            to[x * 4 + 2] = from[0];
            to[x * 4 + 3] = hasAlpha ? from[3] : 255;
            from += header.biBitCount / 8;
        }
    }
    return PrUVec2{uint32_t(width), uint32_t(height)};
}

// The custom type under which a drop's store keeps the bytes of its image's CF_DIBV5, which a read of the image
// decodes.
constexpr PrStringView kDropImageType = "org.prax.app.dib";

// Adds the file at the absolute path `path` to `out`, with its name, its size and whether it is a directory.
void addFile(Win32App* app, const wchar_t* path, AppDataStore& out) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    bool known = GetFileAttributesExW(path, GetFileExInfoStandard, &attributes) != FALSE;
    PrStringView narrowed = windowsNarrow(path, uint32_t(wcslen(path)), app->utf8);
    appDataStoreAddFile(out, PrAppDataFile{
        .name = appPathName(narrowed),
        .path = narrowed,
        .size = known ? (uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow : 0,
        .directory = known && (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
    });
}

// Adds the files that `drop` lists to `out`.
void addDroppedFiles(Win32App* app, HDROP drop, AppDataStore& out) {
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        UINT length = DragQueryFileW(drop, i, nullptr, 0);
        RawArray<wchar_t> path{};
        path.resize(length + 1);
        if (DragQueryFileW(drop, i, path.data, length + 1) == length)
            addFile(app, path.data, out);
        path.reset();
    }
}

// Decodes the bytes of a CF_DIBV5 into `out`'s image.
void setDibImage(const uint8_t* bytes, size_t size, AppDataStore& out) {
    RawArray<uint8_t> pixels{};
    PrUVec2 extent = dibPixels(bytes, size, pixels);
    if (extent.x != 0 && extent.y != 0)
        appDataStoreSetImage(out, PrPixelView{pixels.data, size_t(extent.x) * 4, extent});
    pixels.reset();
}

// Returns the number that follows `name` in a CF_HTML header, or zero.
uint32_t cfHtmlOffset(PrStringView html, PrStringView name) {
    size_t at = prStringFind(html, name);
    if (at == html.size)
        return 0;
    uint32_t value = 0;
    for (at += name.size; at < html.size && html.data[at] >= '0' && html.data[at] <= '9'; ++at)
        value = value * 10 + uint32_t(html.data[at] - '0');
    return value;
}

// Returns the fragment of CF_HTML's document, or the whole of it if its header names none.
PrStringView cfHtmlFragment(PrStringView html) {
    uint32_t start = cfHtmlOffset(html, "StartFragment:");
    uint32_t end = cfHtmlOffset(html, "EndFragment:");
    if (start == 0 || end <= start || end > html.size)
        return html;
    return PrStringView{html.data + start, end - start};
}

// Returns the forms that the clipboard holds.
PrAppDataFormFlags clipboardForms(const Win32App* app) {
    PrAppDataFormFlags forms = 0;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT))
        forms |= PrAppDataFormFlag_Text;
    if (app->htmlFormat != 0 && IsClipboardFormatAvailable(app->htmlFormat))
        forms |= PrAppDataFormFlag_Html;
    if (IsClipboardFormatAvailable(CF_HDROP))
        forms |= PrAppDataFormFlag_Files;
    if (IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB))
        forms |= PrAppDataFormFlag_Image;
    return forms;
}

// Reads the clipboard's content of the form that `answer` asks for into `out`. The clipboard holds no URLs apart from
// its text, and custom content by the clipboard format that its type registers.
void readClipboard(Win32App* app, AppDataAnswer answer, PrStringView type, AppDataStore& out) {
    if (!openClipboard(app->messageWindow))
        return;
    switch (answer) {
        case PrAppDataReadKind_Forms:
            out.forms = clipboardForms(app);
            break;
        case PrAppDataReadKind_Text: {
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            const wchar_t* units = data != nullptr ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
            if (units != nullptr) {
                // The text ends at its NUL, or at the end of its memory if another program wrote none.
                size_t capacity = GlobalSize(data) / sizeof(wchar_t);
                size_t count = 0;
                while (count < capacity && units[count] != 0)
                    ++count;
                int length = int(count < size_t(INT32_MAX) ? count : size_t(INT32_MAX));
                appDataStoreSetText(out, windowsNarrow(units, uint32_t(length), app->utf8));
                GlobalUnlock(data);
            }
            break;
        }
        case PrAppDataReadKind_Html: {
            HANDLE data = app->htmlFormat != 0 ? GetClipboardData(app->htmlFormat) : nullptr;
            const char* bytes = data != nullptr ? static_cast<const char*>(GlobalLock(data)) : nullptr;
            if (bytes != nullptr) {
                PrStringView html{bytes, strnlen(bytes, GlobalSize(data))};
                appDataStoreSetHtml(out, cfHtmlFragment(html));
                GlobalUnlock(data);
            }
            break;
        }
        case PrAppDataReadKind_Image: {
            // The system makes a CF_DIB of a CF_DIBV5 and the reverse, and either of a CF_BITMAP.
            HANDLE data = GetClipboardData(CF_DIBV5);
            const uint8_t* bytes = data != nullptr ? static_cast<const uint8_t*>(GlobalLock(data)) : nullptr;
            if (bytes != nullptr) {
                setDibImage(bytes, GlobalSize(data), out);
                GlobalUnlock(data);
            }
            break;
        }
        case PrAppDataReadKind_Files: {
            HANDLE data = GetClipboardData(CF_HDROP);
            if (data != nullptr)
                addDroppedFiles(app, static_cast<HDROP>(data), out);
            break;
        }
        case PrAppDataReadKind_Custom: {
            RawArray<wchar_t> name{};
            windowsWiden(type, name);
            UINT format = RegisterClipboardFormatW(name.data);
            name.reset();
            HANDLE data = format != 0 ? GetClipboardData(format) : nullptr;
            const uint8_t* bytes = data != nullptr ? static_cast<const uint8_t*>(GlobalLock(data)) : nullptr;
            if (bytes != nullptr) {
                appDataStoreAddCustom(out, type, PrByteView{bytes, GlobalSize(data)});
                GlobalUnlock(data);
            }
            break;
        }
        default:
            break;
    }
    CloseClipboard();
}

// Answers the request `id` of content, on the application's thread: from the clipboard, or from a source's store. A
// source's store does not change, and the request's reference keeps it, so its answer copies only a decoded image. A
// request that was cancelled answers nothing.
void answerReadFor(Win32App* app, uint32_t id, PrStringView type) {
    PrAppDataSource source = kPrAppDataSourceNone;
    AppDataAnswer kind = PrAppDataReadKind_Text;
    if (!appDataRequestsFind(app->requests, id, &source, &kind))
        return;
    AppAnswer* answer = appAnswerCreate(AppAnswerKind_Store, id, PrStatus_Ok);
    AppDataStore* store = appDataSourceStore(app->sources, source);
    if (source == kPrAppDataSourceClipboard) {
        readClipboard(app, kind, type, answer->store);
    } else if (source == kPrAppDataSourceSelection || source == kPrAppDataSourceNone) {
    } else if (store == nullptr) {
        answer->status = PrStatus_NotFound;
    } else if (kind == PrAppDataReadKind_Image) {
        PrByteView dib = appDataStoreCustom(*store, kDropImageType);
        if (dib.size != 0)
            setDibImage(dib.data, dib.size, answer->store);
    } else {
        answer->external = store;
    }
    win32AnswerDeliver(app, answer);
}

//
// One application runs at a time, and the system carries one drag at a time. So the drop target of the windows, the
// data object of a drag and its drop source are single objects of this file. They are not reference counted, because
// they outlive every reference that OLE takes.

// The name of the clipboard format of a drag's token (Win32Drag).
constexpr const wchar_t* kPayloadFormatName = L"org.prax.app.drag-payload";

// Returns the drag operations of an OLE drop effect.
PrAppDragOperationFlags operationsOfEffect(DWORD effect) {
    PrAppDragOperationFlags operations = 0;
    if ((effect & DROPEFFECT_COPY) != 0)
        operations |= PrAppDragOperationFlag_Copy;
    if ((effect & DROPEFFECT_MOVE) != 0)
        operations |= PrAppDragOperationFlag_Move;
    if ((effect & DROPEFFECT_LINK) != 0)
        operations |= PrAppDragOperationFlag_Link;
    return operations;
}

// Returns the OLE drop effect of a set of drag operations.
DWORD effectOf(PrAppDragOperationFlags operations) {
    DWORD effect = DROPEFFECT_NONE;
    if ((operations & PrAppDragOperationFlag_Copy) != 0)
        effect |= DROPEFFECT_COPY;
    if ((operations & PrAppDragOperationFlag_Move) != 0)
        effect |= DROPEFFECT_MOVE;
    if ((operations & PrAppDragOperationFlag_Link) != 0)
        effect |= DROPEFFECT_LINK;
    return effect;
}

// Returns the OLE drop effect of one operation.
DWORD effectOfOperation(PrAppDragOperation operation) {
    if (operation == PrAppDragOperation_None)
        return DROPEFFECT_NONE;
    return effectOf(PrAppDragOperationFlags(1u << operation));
}

// Returns the operation of the drop effect that a target chose. A target chooses one effect, and a move outranks the
// others if it reports several.
PrAppDragOperation operationOfEffect(DWORD effect) {
    if ((effect & DROPEFFECT_MOVE) != 0)
        return PrAppDragOperation_Move;
    if ((effect & DROPEFFECT_COPY) != 0)
        return PrAppDragOperation_Copy;
    if ((effect & DROPEFFECT_LINK) != 0)
        return PrAppDragOperation_Link;
    return PrAppDragOperation_None;
}

// Narrows the source's operations to the one that the modifier keys in `keys` ask for. As in the shell, Control copies,
// Shift moves, and both together link.
PrAppDragOperationFlags narrowedByKeys(PrAppDragOperationFlags operations, DWORD keys) {
    bool control = (keys & MK_CONTROL) != 0;
    bool shift = (keys & MK_SHIFT) != 0;
    if (control && shift)
        return operations & PrAppDragOperationFlag_Link;
    if (control)
        return operations & PrAppDragOperationFlag_Copy;
    if (shift)
        return operations & PrAppDragOperationFlag_Move;
    return operations;
}

// Returns a copy of `size` bytes in movable memory that the caller owns, or null.
HGLOBAL PR_NULLABLE globalOfBytes(const void* bytes, size_t size) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    void* at = memory != nullptr ? GlobalLock(memory) : nullptr;
    if (at == nullptr) {
        if (memory != nullptr)
            GlobalFree(memory);
        return nullptr;
    }
    prMemCopy(at, bytes, size);
    GlobalUnlock(memory);
    return memory;
}

// Returns a device-independent bitmap of straight-alpha sRGB Rgba8 `pixels`, in movable memory that the caller owns, or
// null.
HGLOBAL PR_NULLABLE dibOf(PrPixelView pixels) {
    size_t rowBytes = size_t(pixels.extent.x) * 4;
    size_t bytes = sizeof(BITMAPV5HEADER) + rowBytes * pixels.extent.y;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    uint8_t* at = memory != nullptr ? static_cast<uint8_t*>(GlobalLock(memory)) : nullptr;
    if (at == nullptr) {
        if (memory != nullptr)
            GlobalFree(memory);
        return nullptr;
    }
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = LONG(pixels.extent.x);
    header.bV5Height = LONG(pixels.extent.y);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = DWORD(rowBytes * pixels.extent.y);
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    header.bV5CSType = LCS_sRGB;
    header.bV5Intent = LCS_GM_IMAGES;
    prMemCopy(at, &header, sizeof(header));
    // A bitmap with a positive height holds its rows from the bottom up.
    for (uint32_t y = 0; y < pixels.extent.y; ++y) {
        const uint8_t* from = pixels.data + y * pixels.stride;
        uint8_t* to = at + sizeof(header) + size_t(pixels.extent.y - 1 - y) * rowBytes;
        for (uint32_t x = 0; x < pixels.extent.x; ++x) {
            to[x * 4] = from[x * 4 + 2];
            to[x * 4 + 1] = from[x * 4 + 1];
            to[x * 4 + 2] = from[x * 4];
            to[x * 4 + 3] = from[x * 4 + 3];
        }
    }
    GlobalUnlock(memory);
    return memory;
}

// Returns the paths of `content` as a CF_HDROP list in movable memory that the caller owns, or null.
HGLOBAL PR_NULLABLE fileListOf(Win32App* app, const PrAppDataContent& content) {
    // The list is the header, then each path with its NUL, then one more NUL.
    RawArray<wchar_t> units{};
    for (uint32_t i = 0; i < content.pathCount; ++i) {
        windowsWiden(content.paths[i], app->utf16);
        units.append(app->utf16.data, app->utf16.count);
    }
    units.push(0);
    DROPFILES header{};
    header.pFiles = sizeof(header);
    header.fWide = TRUE;
    size_t bytes = sizeof(header) + size_t(units.count) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    uint8_t* at = memory != nullptr ? static_cast<uint8_t*>(GlobalLock(memory)) : nullptr;
    if (at != nullptr) {
        prMemCopy(at, &header, sizeof(header));
        prMemCopy(at + sizeof(header), units.data, size_t(units.count) * sizeof(wchar_t));
        GlobalUnlock(memory);
    } else if (memory != nullptr) {
        GlobalFree(memory);
        memory = nullptr;
    }
    units.reset();
    return memory;
}

// Returns a request for `format` in global memory.
FORMATETC formatOf(CLIPFORMAT format) {
    return FORMATETC{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
}

// Returns the clipboard format that carries a form of a drag's content.
CLIPFORMAT clipFormatOf(const Win32App* app, PrAppDataFormFlags form) {
    switch (form) {
        case PrAppDataFormFlag_Text: return CF_UNICODETEXT;
        case PrAppDataFormFlag_Files: return CF_HDROP;
        case PrAppDataFormFlag_Image: return CF_DIBV5;
        default: return CLIPFORMAT(app->payloadFormat);
    }
}

// The forms of a drag's content, in the order that the data object offers them.
constexpr PrAppDataFormFlags kDragForms[] = {
    PrAppDataFormFlag_Payload, PrAppDataFormFlag_Files, PrAppDataFormFlag_Image, PrAppDataFormFlag_Text,
};

// A format that SetData added to the data object, as the shell's helper adds the drag's image.
struct DataFormat {
    FORMATETC format;
    STGMEDIUM medium;
};

// Returns the form of the drag's content that `format` carries, or zero if the drag has no such form.
PrAppDataFormFlags formOf(const Win32App* app, const FORMATETC& format) {
    if ((format.tymed & TYMED_HGLOBAL) == 0 || format.dwAspect != DVASPECT_CONTENT)
        return 0;
    for (PrAppDataFormFlags form : kDragForms) {
        if ((app->drag.content.forms & form) != 0 && format.cfFormat == clipFormatOf(app, form))
            return form;
    }
    return 0;
}

// Returns the form of the drag's content in global memory that the caller owns, or null.
HGLOBAL PR_NULLABLE globalOfForm(Win32App* app, PrAppDataFormFlags form) {
    PrAppDataContent content = appDataStoreContent(app->drag.content);
    switch (form) {
        case PrAppDataFormFlag_Text:
            windowsWiden(content.text, app->utf16);
            return globalOfBytes(app->utf16.data, size_t(app->utf16.count) * sizeof(wchar_t));
        case PrAppDataFormFlag_Files: return fileListOf(app, content);
        case PrAppDataFormFlag_Image: return content.image.data != nullptr ? dibOf(content.image) : nullptr;
        case PrAppDataFormFlag_Payload: return globalOfBytes(&app->drag.token, sizeof(app->drag.token));
        default: return nullptr;
    }
}

// The data object of a drag that a window started. It offers each form of the content in Win32App::drag, and the
// formats that SetData added. The content stays until the next drag, for a target that reads it after the drop.
struct DataObject final : IDataObject {
    Win32App* PR_NULLABLE app;
    RawArray<DataFormat> added;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDataObject)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        *medium = STGMEDIUM{};
        if (app == nullptr)
            return DV_E_FORMATETC;
        PrAppDataFormFlags form = formOf(app, *format);
        if (form != 0) {
            HGLOBAL global = globalOfForm(app, form);
            if (global == nullptr)
                return E_OUTOFMEMORY;
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = global;
            return S_OK;
        }
        for (uint32_t i = 0; i < added.count; ++i) {
            const DataFormat& entry = added.data[i];
            if (entry.format.cfFormat != format->cfFormat || (format->tymed & entry.medium.tymed) == 0)
                continue;
            if (entry.medium.tymed == TYMED_HGLOBAL) {
                medium->hGlobal = OleDuplicateData(entry.medium.hGlobal, entry.format.cfFormat, 0);
                if (medium->hGlobal == nullptr)
                    return E_OUTOFMEMORY;
            } else if (entry.medium.tymed == TYMED_ISTREAM) {
                medium->pstm = entry.medium.pstm;
                medium->pstm->AddRef();
            } else {
                return DV_E_TYMED;
            }
            medium->tymed = entry.medium.tymed;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (app != nullptr && formOf(app, *format) != 0)
            return S_OK;
        for (uint32_t i = 0; i < added.count; ++i) {
            if (added.data[i].format.cfFormat == format->cfFormat && (format->tymed & added.data[i].medium.tymed) != 0)
                return S_OK;
        }
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    // Takes ownership of the medium of a format that another party adds, such as the shell's helper. A call that keeps
    // ownership of its medium is refused.
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {
        if (!release)
            return E_NOTIMPL;
        DataFormat entry{.format = *format, .medium = *medium};
        entry.format.ptd = nullptr;
        for (uint32_t i = 0; i < added.count; ++i) {
            if (added.data[i].format.cfFormat == format->cfFormat) {
                ReleaseStgMedium(&added.data[i].medium);
                added.data[i] = entry;
                return S_OK;
            }
        }
        added.push(entry);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        *out = nullptr;
        if (direction != DATADIR_GET || app == nullptr)
            return E_NOTIMPL;
        RawArray<FORMATETC> formats{};
        for (PrAppDataFormFlags form : kDragForms) {
            if ((app->drag.content.forms & form) != 0)
                formats.push(formatOf(clipFormatOf(app, form)));
        }
        for (uint32_t i = 0; i < added.count; ++i) {
            FORMATETC format = added.data[i].format;
            format.tymed = added.data[i].medium.tymed;
            formats.push(format);
        }
        HRESULT result = SHCreateStdEnumFmtEtc(formats.count, formats.data, out);
        formats.reset();
        return result;
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
};

// Releases the formats that SetData added to the data object.
void dataObjectClear(DataObject& object) {
    for (uint32_t i = 0; i < object.added.count; ++i)
        ReleaseStgMedium(&object.added.data[i].medium);
    object.added.reset();
}

// The source of a drag that a window started. The drag drops when the primary button is released, and Escape cancels
// it.
struct DropSource final : IDropSource {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDropSource)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape)
            return DRAGDROP_S_CANCEL;
        if ((keys & MK_LBUTTON) == 0)
            return DRAGDROP_S_DROP;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
};

// Returns the application's window whose top-level window holds the screen point, or null.
Win32Window* PR_NULLABLE windowAt(Win32App* app, POINTL point) {
    HWND hwnd = WindowFromPoint(POINT{point.x, point.y});
    hwnd = hwnd != nullptr ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    for (uint32_t i = 0; i < app->windows.count && hwnd != nullptr; ++i) {
        if (app->windows.data[i]->window == hwnd)
            return app->windows.data[i];
    }
    return nullptr;
}

// Returns a drag as a window's events report it before the drop: the forms of its content that `data` offers, and the
// payload of the drag that the application has in flight if `data` carries its token.
PrAppDragEvent dragOf(Win32App* app, Win32Window* window, IDataObject* PR_NULLABLE data,
        PrAppDragOperationFlags operations) {
    PrAppDragEvent drag{.operations = operations};
    if (data == nullptr)
        return drag;
    for (PrAppDataFormFlags form : kDragForms) {
        if (form == PrAppDataFormFlag_Payload)
            continue;
        FORMATETC format = formatOf(clipFormatOf(app, form));
        if (data->QueryGetData(&format) == S_OK)
            drag.forms |= form;
    }
    FORMATETC format = formatOf(CLIPFORMAT(app->payloadFormat));
    STGMEDIUM medium{};
    if (app->drag.active && data->GetData(&format, &medium) == S_OK) {
        const void* at = GlobalLock(medium.hGlobal);
        uint64_t token = 0;
        if (at != nullptr) {
            if (GlobalSize(medium.hGlobal) >= sizeof(token))
                prMemCopy(&token, at, sizeof(token));
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
        if (token == app->drag.token) {
            drag.fromThisWindow = app->drag.window == window;
            if ((app->drag.content.forms & PrAppDataFormFlag_Payload) != 0) {
                drag.forms |= PrAppDataFormFlag_Payload;
                drag.payload = app->drag.content.payload;
            }
        }
    }
    return drag;
}

// Reads each form of the content that `flags` names from `data` into `store`.
void readDrop(Win32App* app, IDataObject* data, PrAppDataFormFlags flags, AppDataStore& store) {
    STGMEDIUM medium{};
    FORMATETC text = formatOf(CF_UNICODETEXT);
    if ((flags & PrAppDataFormFlag_Text) != 0 && data->GetData(&text, &medium) == S_OK) {
        const wchar_t* units = static_cast<const wchar_t*>(GlobalLock(medium.hGlobal));
        if (units != nullptr) {
            // The text ends at its NUL, or at the end of its memory if the source wrote none.
            size_t capacity = GlobalSize(medium.hGlobal) / sizeof(wchar_t);
            size_t count = 0;
            while (count < capacity && units[count] != 0)
                ++count;
            int length = int(count < size_t(INT32_MAX) ? count : size_t(INT32_MAX));
            appDataStoreSetText(store, windowsNarrow(units, uint32_t(length), app->utf8));
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    FORMATETC files = formatOf(CF_HDROP);
    if ((flags & PrAppDataFormFlag_Files) != 0 && data->GetData(&files, &medium) == S_OK) {
        addDroppedFiles(app, static_cast<HDROP>(medium.hGlobal), store);
        ReleaseStgMedium(&medium);
    }
    FORMATETC image = formatOf(CF_DIBV5);
    if ((flags & PrAppDataFormFlag_Image) != 0 && data->GetData(&image, &medium) == S_OK) {
        const uint8_t* bytes = static_cast<const uint8_t*>(GlobalLock(medium.hGlobal));
        if (bytes != nullptr) {
            appDataStoreAddCustom(store, kDropImageType, PrByteView{bytes, GlobalSize(medium.hGlobal)});
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
}

// The drop target of every window of the application. A drag over a window is answered with the drop operation of the
// window's latest output, and each of the drag's events requests a frame, so that the program answers the drag's latest
// position.
struct DropTarget final : IDropTarget {
    Win32App* PR_NULLABLE app;
    Win32Window* PR_NULLABLE over;      // the window under the drag, from DragEnter to DragLeave or Drop
    uint64_t overSerial;                // the serial of `over`
    IDataObject* PR_NULLABLE data;      // the drag's data, held from DragEnter to DragLeave or Drop
    PrAppDragOperationFlags allowed;    // the operations that the source allows

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDropTarget)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    // Delivers a drag event to the window under the drag, and returns the window's answer.
    DWORD deliverDrag(PrAppEventKind kind, DWORD keys, POINTL point) {
        Win32Window* window = over;
        if (app == nullptr || window == nullptr)
            return DROPEFFECT_NONE;
        PrAppDragEvent drag = dragOf(app, window, data, narrowedByKeys(allowed, keys));
        POINT client{point.x, point.y};
        ScreenToClient(window->window, &client);
        drag.position = win32PositionOf(window, client.x, client.y);
        win32Deliver(window, PrAppEvent{.kind = kind, .modifiers = win32ModifiersNow(), .time = win32SecondsNow(), .drag = drag});
        if (!win32IsOpen(app, window, overSerial)) {
            over = nullptr;
            return DROPEFFECT_NONE;
        }
        win32RequestFrame(window);
        return effectOfOperation(appDragAnswer(window->dropOperation, drag.operations));
    }

    // Ends the drag over the window, and releases its data.
    void leave() {
        Win32Window* window = over;
        over = nullptr;
        if (app != nullptr && window != nullptr && win32IsOpen(app, window, overSerial)) {
            PrAppEvent event{
                .kind = PrAppEventKind_DragLeave,
                .modifiers = win32ModifiersNow(),
                .time = win32SecondsNow(),
                .drag = dragOf(app, window, data, allowed),
            };
            win32Deliver(window, event);
            win32RequestFrame(window);
        }
        if (data != nullptr)
            data->Release();
        data = nullptr;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* object, DWORD keys, POINTL point, DWORD* effect) override {
        if (data != nullptr)
            data->Release();
        data = object;
        if (data != nullptr)
            data->AddRef();
        over = app != nullptr ? windowAt(app, point) : nullptr;
        overSerial = over != nullptr ? over->serial : 0;
        allowed = operationsOfEffect(*effect);
        DWORD answer = deliverDrag(PrAppEventKind_DragEnter, keys, point);
        if (app != nullptr && app->dropHelper != nullptr && over != nullptr) {
            POINT screen{point.x, point.y};
            app->dropHelper->DragEnter(over->window, object, &screen, answer);
        }
        *effect = answer;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD* effect) override {
        allowed = operationsOfEffect(*effect);
        DWORD answer = deliverDrag(PrAppEventKind_DragOver, keys, point);
        if (app != nullptr && app->dropHelper != nullptr) {
            POINT screen{point.x, point.y};
            app->dropHelper->DragOver(&screen, answer);
        }
        *effect = answer;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (app != nullptr && app->dropHelper != nullptr)
            app->dropHelper->DragLeave();
        leave();
        return S_OK;
    }

    // Reads every form of the drag's content into the application's drop, and delivers the drop, if the window answers
    // it. The program reads the content by the drop's source. The data object is the system's only until this returns.
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* object, DWORD keys, POINTL point, DWORD* effect) override {
        Win32Window* window = over;
        allowed = operationsOfEffect(*effect);
        PrAppDragOperationFlags operations = narrowedByKeys(allowed, keys);
        PrAppDragOperation operation = PrAppDragOperation_None;
        if (app != nullptr && window != nullptr && win32IsOpen(app, window, overSerial))
            operation = appDragAnswer(window->dropOperation, operations);
        *effect = effectOfOperation(operation);
        if (app != nullptr && app->dropHelper != nullptr) {
            POINT screen{point.x, point.y};
            app->dropHelper->Drop(object, &screen, *effect);
        }
        if (operation == PrAppDragOperation_None) {
            leave();
            return S_OK;
        }
        PrAppDragEvent drag = dragOf(app, window, object, operations);
        drag.operation = operation;
        AppDataStore* store = allocZeroed<AppDataStore>();
        readDrop(app, object, drag.forms, *store);
        // The image's bytes are custom content of the store, which the drop shows as its image.
        store->forms = drag.forms;
        drag.drop = appDataSourceCreate(app->sources, store, nullptr);
        POINT client{point.x, point.y};
        ScreenToClient(window->window, &client);
        drag.position = win32PositionOf(window, client.x, client.y);
        PrAppEvent event{.kind = PrAppEventKind_Drop, .modifiers = win32ModifiersNow(), .time = win32SecondsNow(), .drag = drag};
        over = nullptr;
        if (data != nullptr)
            data->Release();
        data = nullptr;
        uint64_t serial = overSerial;
        win32Deliver(window, event);
        if (win32IsOpen(app, window, serial))
            win32RequestFrame(window);
        return S_OK;
    }
};

DropTarget gDropTarget{};
DataObject gDataObject{};
DropSource gDropSource{};

// Gives the data object the preview of the window's drag, through the shell's helper, which shows it under the pointer.
void setDragPreview(Win32Window* window) {
    const AppDragPending& drag = window->drag;
    IDragSourceHelper* helper = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper))))
        return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = LONG(drag.previewExtent.x);
    info.bmiHeader.biHeight = -LONG(drag.previewExtent.y);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap != nullptr && bits != nullptr) {
        // The helper reads premultiplied BGRA, from the top row down.
        uint8_t* to = static_cast<uint8_t*>(bits);
        size_t count = size_t(drag.previewExtent.x) * drag.previewExtent.y;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* from = drag.preview.data + i * 4;
            uint32_t alpha = from[3];
            to[i * 4] = uint8_t((from[2] * alpha + 127) / 255);
            to[i * 4 + 1] = uint8_t((from[1] * alpha + 127) / 255);
            to[i * 4 + 2] = uint8_t((from[0] * alpha + 127) / 255);
            to[i * 4 + 3] = uint8_t(alpha);
        }
        float scale = float(window->dpi) / 96.0f;
        SHDRAGIMAGE image{};
        image.sizeDragImage = SIZE{LONG(drag.previewExtent.x), LONG(drag.previewExtent.y)};
        image.ptOffset = POINT{LONG(drag.previewOffset.x * scale), LONG(drag.previewOffset.y * scale)};
        image.hbmpDragImage = bitmap;
        image.crColorKey = CLR_NONE;
        // The helper owns the bitmap once it takes it.
        if (FAILED(helper->InitializeFromBitmap(&image, &gDataObject)))
            DeleteObject(bitmap);
    } else if (bitmap != nullptr) {
        DeleteObject(bitmap);
    }
    helper->Release();
}

// Delivers the end of the window's drag, with the operation that the target performed.
void endDrag(Win32Window* window, PrAppDragOperation operation) {
    PrAppEvent event{
        .kind = PrAppEventKind_DragEnd,
        .modifiers = win32ModifiersNow(),
        .time = win32SecondsNow(),
        .drag = {
            .forms = window->drag.content.forms,
            .operations = window->drag.operations,
            .operation = operation,
            .fromThisWindow = true,
            .payload = window->drag.content.payload,
        },
    };
    win32Deliver(window, event);
}

void postRequest(Win32App* app, const Win32Request& request) {
    Win32Request* posted = allocZeroed<Win32Request>();
    *posted = request;
    HWND window = app->wakeWindow.load();
    if (window == nullptr || !PostMessageW(window, kMessageRead, 0, LPARAM(posted))) {
        prRawStringReset(&posted->type);
        posted->bytes.reset();
        prMemoryFree(posted);
    }
}

uint32_t addRequest(Win32App* app, PrAppDataSource source, AppDataAnswer kind, void (*done)(void),
        void* PR_NULLABLE context, PrStringView type = {}) {
    return appDataRequestsAdd(app->requests, source, kind, done, context, type);
}

// Reads or writes a file of a request's source, on the application's thread, and answers. A write at offset zero
// empties the file first.
void answerFileFor(Win32App* app, Win32Request& request) {
    PrAppDataSource source = kPrAppDataSourceNone;
    AppDataAnswer kind = PrAppDataReadKind_File;
    if (!appDataRequestsFind(app->requests, request.id, &source, &kind))
        return;
    bool status = request.write || kind == AppDataAnswer_Status;
    AppAnswer* answer = appAnswerCreate(status ? AppAnswerKind_Status : AppAnswerKind_Bytes, request.id,
        request.index == UINT32_MAX ? PrStatus_NotSupported : PrStatus_NotFound);
    AppDataStore* store = appDataSourceStore(app->sources, source);
    uint32_t count = 0;
    const PrAppDataFile* files = store != nullptr ? appDataStoreFiles(*store, &count) : nullptr;
    if (request.index < count && files[request.index].path.size != 0 && !files[request.index].directory) {
        RawArray<wchar_t> path{};
        windowsWiden(files[request.index].path, path);
        DWORD disposition = request.write ? (request.offset == 0 ? CREATE_ALWAYS : OPEN_ALWAYS) : OPEN_EXISTING;
        HANDLE file = CreateFileW(path.data, request.write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, nullptr,
            disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
        path.reset();
        answer->status = PrStatus_Io;
        LARGE_INTEGER at{};
        at.QuadPart = LONGLONG(request.offset);
        if (file != INVALID_HANDLE_VALUE && SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) {
            DWORD done = 0;
            if (request.write) {
                if (WriteFile(file, request.bytes.data, request.bytes.count, &done, nullptr) &&
                        done == request.bytes.count)
                    answer->status = PrStatus_Ok;
            } else {
                answer->bytes.resize(uint32_t(request.size < UINT32_MAX ? request.size : UINT32_MAX));
                if (ReadFile(file, answer->bytes.data, answer->bytes.count, &done, nullptr)) {
                    answer->bytes.resize(done);
                    answer->status = PrStatus_Ok;
                }
            }
        }
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
    win32AnswerDeliver(app, answer);
}

// Adds `text` to the request's strings, with its NUL.
void requestAdd(Win32DialogRequest& request, PrStringView text) {
    RawArray<wchar_t> wide{};
    windowsWiden(text, wide);
    request.starts.push(request.strings.count);
    request.strings.append(wide.data, wide.count);
    wide.reset();
}

void requestReset(Win32DialogRequest* request) {
    request->strings.reset();
    request->starts.reset();
    prMemoryFree(request);
}

// Adds the file of `item` to `content`.
void addItemPath(Win32App* app, IShellItem* item, AppDataStore& content) {
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    addFile(app, path, content);
    CoTaskMemFree(path);
}

// Returns `html` as CF_HTML holds it: a header of the offsets of the document and of its fragment, and the document.
RawArray<char> cfHtmlOf(PrStringView html) {
    constexpr PrStringView kHeader = "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n"
        "StartFragment:0000000000\r\nEndFragment:0000000000\r\n";
    constexpr PrStringView kBefore = "<html><body><!--StartFragment-->";
    constexpr PrStringView kAfter = "<!--EndFragment--></body></html>";
    RawArray<char> out{};
    out.append(kHeader.data, uint32_t(kHeader.size));
    out.append(kBefore.data, uint32_t(kBefore.size));
    if (html.size != 0)
        out.append(html.data, uint32_t(html.size));
    out.append(kAfter.data, uint32_t(kAfter.size));
    out.push('\0');
    // Each offset is ten digits, after its name and its colon, in the header's order.
    uint32_t offsets[] = {
        uint32_t(kHeader.size),
        out.count - 1,
        uint32_t(kHeader.size + kBefore.size),
        uint32_t(kHeader.size + kBefore.size + html.size),
    };
    PrStringView names[] = {"StartHTML:", "EndHTML:", "StartFragment:", "EndFragment:"};
    for (uint32_t i = 0; i < 4; ++i) {
        PrStringView header{out.data, kHeader.size};
        size_t at = prStringFind(header, names[i]) + names[i].size;
        for (uint32_t digit = 10, value = offsets[i]; digit > 0; --digit, value /= 10)
            out.data[at + digit - 1] = char('0' + value % 10);
    }
    return out;
}

// Sets one format of the clipboard, which owns `memory` once it takes it. Returns false if it refuses.
bool setClipboard(UINT format, HGLOBAL PR_NULLABLE memory) {
    if (memory == nullptr)
        return false;
    if (SetClipboardData(format, memory) != nullptr)
        return true;
    GlobalFree(memory);
    return false;
}

} // namespace

void win32DragsStart(Win32App* app) {
    app->payloadFormat = RegisterClipboardFormatW(kPayloadFormatName);
    app->htmlFormat = RegisterClipboardFormatW(L"HTML Format");
    if (app->ole && FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&app->dropHelper))))
        app->dropHelper = nullptr;
    gDropTarget.app = app;
    gDataObject.app = app;
}

void win32DragsStop(Win32App* app) {
    if (app->dropHelper != nullptr)
        app->dropHelper->Release();
    app->dropHelper = nullptr;
    appDataStoreReset(app->drag.content);
    dataObjectClear(gDataObject);
    gDropTarget.app = nullptr;
    gDataObject.app = nullptr;
}

void win32DragsRegister(Win32Window* window) {
    if (appOf(window)->ole)
        RegisterDragDrop(window->window, &gDropTarget);
}

void win32DragsRevoke(Win32Window* window) {
    if (appOf(window)->ole)
        RevokeDragDrop(window->window);
}

void win32StartDrag(Win32Window* window) {
    if (!window->drag.pending)
        return;
    window->drag.pending = false;
    Win32App* app = appOf(window);
    uint64_t serial = window->serial;
    PrAppDataContent content = appDataStoreContent(window->drag.content);
    bool down = (window->buttons & 1u) != 0 && GetKeyState(VK_LBUTTON) < 0;
    bool startable = app->ole && !app->headless && !app->drag.active && !window->inSizeMove && down;
    if (!startable || content.forms == 0) {
        endDrag(window, PrAppDragOperation_None);
        return;
    }
    dataObjectClear(gDataObject);
    appDataStoreSet(app->drag.content, content);
    app->drag.active = true;
    app->drag.token = prRandomSeed();
    app->drag.window = window;
    app->drag.operations = window->drag.operations;
    if (window->drag.previewExtent.x != 0 && window->drag.previewExtent.y != 0)
        setDragPreview(window);
    // The system takes the pointer, so no release reaches the window to end the press.
    window->buttons &= uint8_t(~1u);
    PrAppEvent cancel{
        .kind = PrAppEventKind_PointerCancel,
        .modifiers = win32ModifiersNow(),
        .time = win32SecondsNow(),
        .pointer =
            {.position = window->pointer, .type = PrAppPointerType_Mouse, .primary = true, .id = kPrAppPointerMouse},
    };
    win32Deliver(window, cancel);
    DWORD effect = DROPEFFECT_NONE;
    HRESULT result = DRAGDROP_S_CANCEL;
    if (win32IsOpen(app, window, serial))
        result = DoDragDrop(&gDataObject, &gDropSource, effectOf(app->drag.operations), &effect);
    Win32Window* source = app->drag.window;
    app->drag.active = false;
    app->drag.window = nullptr;
    // A window that closed during the drag receives no DragEnd. Its destruction cleared the drag's window.
    if (source != nullptr && win32IsOpen(app, source, serial))
        endDrag(source, result == DRAGDROP_S_DROP ? operationOfEffect(effect) : PrAppDragOperation_None);
}

void win32AnswerRequest(Win32App* app, Win32Request* request) {
    if (request->file)
        answerFileFor(app, *request);
    else
        answerReadFor(app, request->id, prRawStringView(&request->type));
    prRawStringReset(&request->type);
    request->bytes.reset();
    prMemoryFree(request);
}

void win32ShowDialog(Win32App* app, Win32DialogRequest* request) {
    PrAppDataSource source = kPrAppDataSourceNone;
    AppDataAnswer kind = AppDataAnswer_Source;
    if (!appDataRequestsFind(app->requests, request->id, &source, &kind)) {
        requestReset(request);
        return;
    }
    AppDataStore* content = allocZeroed<AppDataStore>();
    bool save = request->kind == PrAppFileDialogKind_Save;
    IFileDialog* dialog = nullptr;
    HRESULT result = E_FAIL;
    if (!app->headless && app->ole) {
        const CLSID& kind = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
        result = CoCreateInstance(kind, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    }
    if (FAILED(result) || dialog == nullptr) {
        appDataStoreReset(*content);
        prMemoryFree(content);
        win32AnswerDeliver(app, appAnswerCreate(AppAnswerKind_Source, request->id, PrStatus_NotSupported));
        requestReset(request);
        return;
    }
    auto text = [&](uint32_t index) { return request->strings.data + request->starts.data[index]; };
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM;
    bool directories = !save && (request->flags & PrAppFileDialogFlag_Directories) != 0;
    if (!save && (request->flags & PrAppFileDialogFlag_Multiple) != 0)
        options |= FOS_ALLOWMULTISELECT;
    if (directories)
        options |= FOS_PICKFOLDERS;
    if (save)
        options |= FOS_OVERWRITEPROMPT;
    dialog->SetOptions(options);
    if (text(0)[0] != 0)
        dialog->SetTitle(text(0));
    IShellItem* folder = nullptr;
    if (text(1)[0] != 0 && SUCCEEDED(SHCreateItemFromParsingName(text(1), nullptr, IID_PPV_ARGS(&folder)))) {
        dialog->SetFolder(folder);
        folder->Release();
    }
    if (save && text(2)[0] != 0)
        dialog->SetFileName(text(2));
    uint32_t types = (request->starts.count - 3) / 2;
    if (types != 0 && !directories) {
        RawArray<COMDLG_FILTERSPEC> specs{};
        for (uint32_t i = 0; i < types; ++i)
            specs.push(COMDLG_FILTERSPEC{text(3 + 2 * i), text(4 + 2 * i)});
        dialog->SetFileTypes(types, specs.data);
        specs.reset();
        // A Save dialog adds the first type's first extension, after its "*.", to a name that has none.
        const wchar_t* pattern = text(4);
        if (save && pattern[0] == L'*' && pattern[1] == L'.') {
            wchar_t extension[32]{};
            for (uint32_t i = 0; i + 1 < countOf(extension) && pattern[i + 2] != 0 && pattern[i + 2] != L';'; ++i)
                extension[i] = pattern[i + 2];
            dialog->SetDefaultExtension(extension);
        }
    }
    app->dialog = dialog;
    app->dialogRead = request->id;
    app->dialogParent = request->parent;
    result = dialog->Show(request->parent);
    app->dialog = nullptr;
    app->dialogParent = nullptr;
    if (SUCCEEDED(result) && save) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            addItemPath(app, item, *content);
            item->Release();
        }
    } else if (SUCCEEDED(result)) {
        IFileOpenDialog* open = nullptr;
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&open))) && SUCCEEDED(open->GetResults(&items))) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &item))) {
                    addItemPath(app, item, *content);
                    item->Release();
                }
            }
        }
        if (items != nullptr)
            items->Release();
        if (open != nullptr)
            open->Release();
    }
    dialog->Release();
    // The user's choice is a source of its files, which the answer holds a reference to. A cancel answers None.
    AppAnswer* answer = appAnswerCreate(AppAnswerKind_Source, request->id, PrStatus_Ok);
    uint32_t chosen = 0;
    appDataStoreFiles(*content, &chosen);
    if (chosen != 0) {
        answer->source = appDataSourceCreate(app->sources, content, nullptr);
    } else {
        appDataStoreReset(*content);
        prMemoryFree(content);
    }
    win32AnswerDeliver(app, answer);
    requestReset(request);
}

void win32CloseCancelledDialog(Win32App* app) {
    PrAppDataSource source = kPrAppDataSourceNone;
    AppDataAnswer kind = AppDataAnswer_Source;
    if (app->dialog != nullptr && !appDataRequestsFind(app->requests, app->dialogRead, &source, &kind))
        app->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
}

void win32DataCancel(PrApp* base, void* PR_NULLABLE context) {
    Win32App* app = static_cast<Win32App*>(base);
    appDataRequestsCancel(app->requests, context);
    if (HWND message = app->wakeWindow.load())
        PostMessageW(message, kMessageDialogCancel, 0, 0);
}

void win32DataRetain(PrApp* base, PrAppDataSource source) {
    appDataSourceRetain(static_cast<Win32App*>(base)->sources, source);
}

void win32DataRelease(PrApp* base, PrAppDataSource source) {
    appDataSourceRelease(static_cast<Win32App*>(base)->sources, source);
}

void win32DataRead(PrApp* base, const PrAppDataRead* read) {
    Win32App* app = static_cast<Win32App*>(base);
    uint32_t id = addRequest(app, read->source, read->kind, read->done, read->context, read->type);
    if (read->kind == PrAppDataReadKind_File) {
        postRequest(app, Win32Request{
            .id = id,
            .file = true,
            .index = read->index,
            .offset = read->offset,
            .size = read->size,
        });
    } else {
        postRequest(app, Win32Request{.id = id, .type = prRawStringCopy(read->type)});
    }
}

PrStatus win32DataWrite(PrApp* base, PrAppDataSource source, const PrAppDataContent* content) {
    Win32App* app = static_cast<Win32App*>(base);
    if (source == kPrAppDataSourceSelection)
        return PrStatus_NotSupported;
    // The clipboard takes data only from an owner window, which exists during a run.
    if (app->messageWindow == nullptr || !openClipboard(app->messageWindow))
        return PrStatus_Io;
    bool set = EmptyClipboard() != FALSE;
    if (set && (content->forms & PrAppDataFormFlag_Text) != 0) {
        windowsWiden(content->text, app->utf16);
        set = setClipboard(CF_UNICODETEXT, globalOfBytes(app->utf16.data, size_t(app->utf16.count) * sizeof(wchar_t)));
    }
    if (set && (content->forms & PrAppDataFormFlag_Html) != 0 && app->htmlFormat != 0) {
        RawArray<char> html = cfHtmlOf(content->html);
        set = setClipboard(app->htmlFormat, globalOfBytes(html.data, html.count));
        html.reset();
    }
    if (set && (content->forms & PrAppDataFormFlag_Files) != 0)
        set = setClipboard(CF_HDROP, fileListOf(app, *content));
    if (set && (content->forms & PrAppDataFormFlag_Image) != 0)
        set = setClipboard(CF_DIBV5, dibOf(content->image));
    for (uint32_t i = 0; set && (content->forms & PrAppDataFormFlag_Custom) != 0 && i < content->customCount; ++i) {
        RawArray<wchar_t> name{};
        windowsWiden(content->custom[i].type, name);
        UINT format = RegisterClipboardFormatW(name.data);
        name.reset();
        set = format != 0 &&
            setClipboard(format, globalOfBytes(content->custom[i].bytes.data, content->custom[i].bytes.size));
    }
    CloseClipboard();
    return set ? PrStatus_Ok : PrStatus_Io;
}

void win32DataWriteFile(PrApp* base, PrAppDataSource source, uint32_t index, uint64_t offset, PrByteView bytes,
        PrAppDataStatusFn done, void* PR_NULLABLE context) {
    Win32App* app = static_cast<Win32App*>(base);
    uint32_t id = addRequest(app, source, AppDataAnswer_Status, (void (*)(void))done, context);
    Win32Request request{.id = id, .file = true, .write = true, .index = index, .offset = offset};
    if (bytes.size != 0)
        request.bytes.append(bytes.data, uint32_t(bytes.size));
    postRequest(app, request);
}

void win32FileDialog(PrApp* base, const PrAppFileDialogDesc* desc, PrAppFileDialogFn done, void* PR_NULLABLE context) {
    Win32App* app = static_cast<Win32App*>(base);
    uint32_t id = addRequest(app, kPrAppDataSourceNone, AppDataAnswer_Source, (void (*)(void))done, context);
    Win32DialogRequest* request = allocZeroed<Win32DialogRequest>();
    request->id = id;
    request->kind = desc->kind;
    request->flags = desc->flags;
    request->parent = desc->parent != nullptr ? static_cast<Win32Window*>(desc->parent)->window : nullptr;
    requestAdd(*request, desc->title);
    requestAdd(*request, desc->directory);
    requestAdd(*request, desc->name);
    RawArray<char> pattern{};
    for (uint32_t i = 0; i < desc->typeCount; ++i) {
        pattern.clear();
        PrStringView rest = desc->types[i].extensions;
        while (rest.size != 0) {
            size_t comma = 0;
            while (comma < rest.size && rest.data[comma] != ',')
                ++comma;
            PrStringView extension = prStringTrim(PrStringView{rest.data, comma});
            if (extension.size != 0) {
                if (pattern.count != 0)
                    pattern.push(';');
                pattern.append("*.", 2);
                pattern.append(extension.data, uint32_t(extension.size));
            }
            rest = comma < rest.size ? PrStringView{rest.data + comma + 1, rest.size - comma - 1} : PrStringView{};
        }
        requestAdd(*request, desc->types[i].name);
        requestAdd(*request, PrStringView{pattern.data != nullptr ? pattern.data : "", pattern.count});
    }
    pattern.reset();
    if (app->messageWindow == nullptr || !PostMessageW(app->messageWindow, kMessageDialog, 0, LPARAM(request)))
        requestReset(request);
}

void win32Share(PrAppWindow* window, const PrAppDataContent*, PrRect, PrAppShareFn done, void* PR_NULLABLE context) {
    Win32App* app = appOf(static_cast<Win32Window*>(window));
    uint32_t id = addRequest(app, kPrAppDataSourceNone, AppDataAnswer_Status, (void (*)(void))done, context);
    postRequest(app, Win32Request{.id = id, .file = true, .index = UINT32_MAX});
}

} // namespace pr_impl

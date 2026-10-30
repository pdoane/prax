// A WebView2 controller that the Win32 backend's browser loads, runs and captures. When hidden, it is in a window of
// its own beyond the left edge of the desktop, where the page renders but no one sees it. When shown, it is in a
// clipping window over the app's own window. The engine starts asynchronously, so the calls made before it has started
// wait for it, in order.
//
// The engine holds its handlers past a browser's destruction, and may call them after it. Each handler holds the
// browser's memory until it runs, and does nothing once the browser is destroyed. A browser destroyed while the engine
// creates its page lets the creation finish and then closes the page, because a process that exits while an environment
// has never had a controller can hang in the kernel as it exits.

#include "app/backends/win32/src/win32_browser.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/base64.h"
#include "pr/count_of.h"
#include "pr/float_convert.h"
#include "pr/json.h"
#include "pr/text_buffer.h"

#include "app/backends/win32/src/win32_text.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <WebView2EnvironmentOptions.h>
#include <wincodec.h>
#include <wrl.h>

namespace pr_impl {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

// The message that a browser posts to its window to deliver the completions that failed during their calls.
constexpr UINT kMessageFailed = WM_APP + 0;

constexpr wchar_t kClassName[] = L"PrAppWin32Browser";

// The folder of the engine's profile, under the user's temporary folder. Pages run in private mode, so the profile
// keeps no data of theirs.
constexpr wchar_t kDataFolder[] = L"PrAppWebView2";

// The arguments of the engine's browser process. A page from a file can read the files beside it. A page renders and
// runs its timers at full speed in a window that no one sees. Pages render in sRGB, whatever the display's profile, so
// that captures are in sRGB.
constexpr wchar_t kArguments[] = L"--allow-file-access-from-files --disable-background-timer-throttling "
    L"--disable-renderer-backgrounding --disable-backgrounding-occluded-windows "
    L"--disable-features=CalculateNativeWinOcclusion --force-color-profile=srgb";

// The two halves of the expression that runs a script as global code, with the script between them as a string. The
// expression awaits the result, so that a promise answers once it settles. It returns the result as JSON, or the
// message of the script's error.
constexpr char kEvaluateBefore[] = "(async () => { try { const value = await (0, eval)(";
constexpr char kEvaluateAfter[] = "); return value === undefined ? {} : {json: JSON.stringify(value)}; } catch (error) "
    "{ return {error: String(error instanceof Error ? error.message : error)}; } })()";

enum {
    RequestKind_Load,
    RequestKind_Evaluate,
    RequestKind_Capture,
    RequestKind_Print,
};
typedef uint8_t RequestKind;

// A call that waits for the engine to start, or the completion of a call that failed during it, which waits to be
// delivered. The call's kind decides which completion it has.
struct Request {
    RequestKind kind;
    PrStatus status;            // the failure that a completion delivers
    Array<char> text;           // the URL, script or path
    PrRect rect;
    PrUVec2 size;
    PrVec2 paper;
    PrAppBrowserDoneFn done;
    PrAppBrowserResultFn result;
    PrAppBrowserCaptureFn capture;
    void* context;
};

struct Win32Browser : PrAppBrowser {
    HWND host;                  // the application's window, which a shown page is placed over, or null
    HWND window;                // the window beyond the desktop that holds the hidden page
    HWND clip;                  // the child of `host` that clips the shown page, or null
    ICoreWebView2Environment* environment;
    ICoreWebView2Controller* controller;
    ICoreWebView2* view;
    IWICImagingFactory* imaging;
    EventRegistrationToken starting;
    EventRegistrationToken completed;
    PrUVec2 viewport;           // CSS pixels
    float pixelRatio;           // the pixels per CSS pixel that the page renders at, or zero for the display's
    COREWEBVIEW2_PREFERRED_COLOR_SCHEME colorScheme;
    bool started;               // the engine has started, and calls go to it
    bool failed;                // the engine did not start, and calls fail
    bool creating;              // the engine's environment or the page's controller is being created
    bool destroyed;
    bool shown;
    PrRect shownClip;
    PrRect shownFrame;
    uint32_t holds;             // the engine's handlers that have not run
    uint64_t navigation;        // the latest navigation that started since the last load, or zero

    // The completion of the pending load, or null.
    PrAppBrowserDoneFn loaded;
    void* loadedContext;

    Array<Request> waiting;     // the calls made before the engine started
    Array<Request> failures;    // the completions that the next kMessageFailed delivers

    // Scratch storage for conversions.
    Array<wchar_t> utf16;
    Array<char> utf8;
};

template <typename T>
void release(T* PR_NULLABLE object) {
    if (object != nullptr)
        object->Release();
}

// Takes a hold on the browser for a handler, which keeps the browser's memory until the handler runs.
Win32Browser* hold(Win32Browser* b) {
    ++b->holds;
    return b;
}

// Ends a handler's hold. Returns false if the browser was destroyed, and frees it once nothing holds it and its
// teardown has run.
bool unhold(Win32Browser* b) {
    --b->holds;
    if (!b->destroyed)
        return true;
    if (b->holds == 0 && !b->creating)
        prMemoryFree(b);
    return false;
}

// Releases the engine's objects and the browser's windows once the browser is destroyed, and frees the browser once
// nothing holds it.
void teardown(Win32Browser* b) {
    if (b->view != nullptr) {
        b->view->remove_NavigationStarting(b->starting);
        b->view->remove_NavigationCompleted(b->completed);
        b->view->Release();
    }
    if (b->controller != nullptr) {
        b->controller->Close();
        b->controller->Release();
    }
    release(b->environment);
    release(b->imaging);
    for (uint32_t i = 0; i < b->waiting.count; ++i)
        b->waiting.data[i].text.reset();
    b->waiting.reset();
    b->failures.reset();
    b->utf16.reset();
    b->utf8.reset();
    if (b->clip != nullptr)
        DestroyWindow(b->clip);
    SetWindowLongPtrW(b->window, GWLP_USERDATA, 0);
    DestroyWindow(b->window);
    if (b->holds == 0)
        prMemoryFree(b);
}

void complete(const Request& request, PrStatus status) {
    switch (request.kind) {
        case RequestKind_Load:
        case RequestKind_Print: request.done(request.context, status); break;
        case RequestKind_Evaluate: request.result(request.context, status, PrStringView{}); break;
        case RequestKind_Capture: request.capture(request.context, status, PrPixelView{}); break;
        default: break;
    }
}

// Delivers the completion of `request` as a failure with `status` once the message loop runs, and takes the request.
void fail(Win32Browser* b, Request& request, PrStatus status) {
    request.text.reset();
    request.status = status;
    b->failures.push(request);
    PostMessageW(b->window, kMessageFailed, 0, 0);
}

void deliverFailures(Win32Browser* b) {
    hold(b);
    while (!b->destroyed && b->failures.count > 0) {
        Request request = b->failures.data[0];
        b->failures.erase(0);
        complete(request, request.status);
    }
    unhold(b);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    } else if (message == kMessageFailed) {
        Win32Browser* b = reinterpret_cast<Win32Browser*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (b != nullptr)
            deliverFailures(b);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void registerClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);
}

// Appends `text` to `out` as a JSON string.
void appendJsonString(TextBuffer* out, PrStringView text) {
    static const char kHex[] = "0123456789abcdef";
    textAppendChar(out, '"');
    for (size_t i = 0; i < text.size; ++i) {
        unsigned char c = static_cast<unsigned char>(text.data[i]);
        if (c == '"' || c == '\\') {
            textAppendChar(out, '\\');
            textAppendChar(out, char(c));
        } else if (c < 0x20) {
            textAppend(out, "\\u00");
            textAppendChar(out, kHex[c >> 4]);
            textAppendChar(out, kHex[c & 15]);
        } else {
            textAppendChar(out, char(c));
        }
    }
    textAppendChar(out, '"');
}

// Appends `value` to `out` as a JSON number, or zero if it is not finite.
void appendJsonNumber(TextBuffer* out, double value) {
    if (!(value > -1e308 && value < 1e308) || value == 0) {
        textAppendChar(out, '0');
        return;
    }
    if (value < 0)
        textAppendChar(out, '-');
    ShortestDecimal decimal = shortestDecimalF64(value < 0 ? -value : value);
    textAppendU64(out, decimal.digits);
    if (decimal.exponent != 0) {
        textAppendChar(out, 'e');
        if (decimal.exponent < 0)
            textAppendChar(out, '-');
        textAppendU64(out, uint64_t(decimal.exponent < 0 ? -decimal.exponent : decimal.exponent));
    }
}

// The pixels per CSS pixel that the page renders at.
float ratioOf(const Win32Browser* b) {
    if (b->pixelRatio > 0)
        return b->pixelRatio;
    UINT dpi = b->host != nullptr ? GetDpiForWindow(b->host) : GetDpiForSystem();
    return float(dpi != 0 ? dpi : 96) / 96.0f;
}

LONG rounded(float value) {
    return LONG(value < 0 ? value - 0.5f : value + 0.5f);
}

// Places the page. While the page is shown, its bounds fill its frame in the host's pixels, clipped by the clipping
// window, and the page's zoom keeps the CSS pixels of its viewport. While it is hidden, its window has the viewport's
// size in the page's pixels.
void layOut(Win32Browser* b) {
    float ratio = ratioOf(b);
    HWND parent = b->window;
    RECT bounds{};
    if (b->shown) {
        float scale = float(GetDpiForWindow(b->host)) / 96.0f;
        PrRect clip = b->shownClip;
        PrRect frame = b->shownFrame;
        LONG left = rounded(clip.left * scale);
        LONG top = rounded(clip.top * scale);
        SetWindowPos(b->clip, HWND_TOP, left, top, rounded(clip.right * scale) - left,
            rounded(clip.bottom * scale) - top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        bounds = RECT{
            rounded(frame.left * scale) - left, rounded(frame.top * scale) - top,
            rounded(frame.right * scale) - left, rounded(frame.bottom * scale) - top,
        };
        parent = b->clip;
    } else {
        LONG width = rounded(float(b->viewport.x) * ratio);
        LONG height = rounded(float(b->viewport.y) * ratio);
        width = width > 0 ? width : 1;
        height = height > 0 ? height : 1;
        SetWindowPos(b->window, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN) - width,
            GetSystemMetrics(SM_YVIRTUALSCREEN), width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        bounds = RECT{0, 0, width, height};
    }
    if (b->controller == nullptr)
        return;
    HWND current = nullptr;
    if (SUCCEEDED(b->controller->get_ParentWindow(&current)) && current != parent)
        b->controller->put_ParentWindow(parent);
    ICoreWebView2Controller3* scaled = nullptr;
    if (SUCCEEDED(b->controller->QueryInterface(IID_PPV_ARGS(&scaled)))) {
        scaled->put_RasterizationScale(double(ratio));
        scaled->Release();
    }
    double extent = double(bounds.right - bounds.left);
    double zoom = b->viewport.x > 0 ? extent / (double(ratio) * double(b->viewport.x)) : 1.0;
    b->controller->put_ZoomFactor(zoom > 0 ? zoom : 1.0);
    b->controller->put_Bounds(bounds);
}

// Completes the pending load with `status`.
void finishLoad(Win32Browser* b, PrStatus status) {
    PrAppBrowserDoneFn done = b->loaded;
    void* context = b->loadedContext;
    b->loaded = nullptr;
    if (done != nullptr)
        done(context, status);
}

// A load that replaces a pending one completes the pending one as failed.
void navigate(Win32Browser* b, Request& request) {
    if (b->loaded != nullptr) {
        Request replaced{.kind = RequestKind_Load, .done = b->loaded, .context = b->loadedContext};
        fail(b, replaced, PrStatus_NotFound);
    }
    win32Widen(PrStringView{request.text.data, request.text.count}, b->utf16);
    request.text.reset();
    b->navigation = 0;
    b->loaded = request.done;
    b->loadedContext = request.context;
    if (FAILED(b->view->Navigate(b->utf16.data))) {
        b->loaded = nullptr;
        fail(b, request, PrStatus_NotFound);
    }
}

// Calls `done` with the result of an evaluation, from the engine's answer to Runtime.evaluate.
void answer(Win32Browser* b, HRESULT error, LPCWSTR json, PrAppBrowserResultFn done, void* PR_NULLABLE context) {
    PrJson* document = prJsonCreate();
    PrStringView text = json != nullptr ? win32Narrow(json, lstrlenW(json), b->utf8) : PrStringView{""};
    if (!prJsonParse(document, text, nullptr)) {
        prJsonDestroy(document);
        done(context, PrStatus_InvalidData, "the engine did not answer");
        return;
    }
    if (FAILED(error)) {
        PrStringView message = prJsonString(document, prJsonMember(document, 0, "message"));
        done(context, PrStatus_InvalidData, message.data != nullptr ? message : "the engine did not run the script");
        prJsonDestroy(document);
        return;
    }
    PrJsonValueIndex result = prJsonMember(document, 0, "result");
    PrJsonValueIndex value = prJsonMember(document, result, "value");
    PrStringView returned = prJsonString(document, prJsonMember(document, value, "json"));
    PrStringView thrown = prJsonString(document, prJsonMember(document, value, "error"));
    if (value == kPrJsonMissing) {
        PrJsonValueIndex details = prJsonMember(document, 0, "exceptionDetails");
        PrStringView message = prJsonString(document, prJsonMember(document, details, "text"));
        done(context, PrStatus_InvalidData, message.data != nullptr ? message : "the script did not run");
    } else if (thrown.data != nullptr) {
        done(context, PrStatus_InvalidData, thrown);
    } else {
        done(context, PrStatus_Ok, returned);
    }
    prJsonDestroy(document);
}

void evaluate(Win32Browser* b, Request& request) {
    TextBuffer expression;
    textAppend(&expression, kEvaluateBefore);
    appendJsonString(&expression, PrStringView{request.text.data, request.text.count});
    textAppend(&expression, kEvaluateAfter);
    TextBuffer params;
    textAppend(&params, "{\"expression\":");
    appendJsonString(&params, textView(&expression));
    textAppend(&params, ",\"awaitPromise\":true,\"returnByValue\":true}");
    win32Widen(textView(&params), b->utf16);
    textReset(&expression);
    textReset(&params);
    request.text.reset();
    PrAppBrowserResultFn done = request.result;
    void* context = request.context;
    HRESULT called = b->view->CallDevToolsProtocolMethod(L"Runtime.evaluate", b->utf16.data,
        Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [b = hold(b), done, context](HRESULT error, LPCWSTR json) -> HRESULT {
                if (unhold(b))
                    answer(b, error, json, done, context);
                return S_OK;
            }) .Get());
    if (FAILED(called)) {
        unhold(b);
        fail(b, request, PrStatus_Io);
    }
}

// Decodes `png` to premultiplied Bgra8 pixels of `extent` in `out`, scaling it if it has another extent.
bool decodePng(IWICImagingFactory* imaging, const Array<char>& png, PrUVec2 extent, Array<uint8_t>& out) {
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    bool ok = SUCCEEDED(imaging->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(png.data), png.count)) &&
        SUCCEEDED(imaging->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(imaging->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom));
    IWICBitmapSource* source = converter;
    UINT width = 0;
    UINT height = 0;
    if (ok && SUCCEEDED(converter->GetSize(&width, &height)) && (width != extent.x || height != extent.y)) {
        ok = SUCCEEDED(imaging->CreateBitmapScaler(&scaler)) && SUCCEEDED(
            scaler->Initialize(converter, extent.x, extent.y, WICBitmapInterpolationModeHighQualityCubic));
        source = scaler;
    }
    if (ok) {
        out.resize(extent.x * extent.y * 4);
        ok = SUCCEEDED(source->CopyPixels(nullptr, extent.x * 4, out.count, out.data));
    }
    release(scaler);
    release(converter);
    release(frame);
    release(decoder);
    release(stream);
    return ok;
}

// Calls `done` with the pixels of a capture, from the engine's answer to Page.captureScreenshot.
void deliverCapture(Win32Browser* b, HRESULT error, LPCWSTR json, PrUVec2 size, PrAppBrowserCaptureFn done,
        void* PR_NULLABLE context) {
    Array<char> png;
    Array<uint8_t> pixels;
    bool ok = SUCCEEDED(error) && json != nullptr && size.x > 0 && size.y > 0;
    if (ok) {
        PrJson* document = prJsonCreate();
        PrStringView data;
        if (prJsonParse(document, win32Narrow(json, lstrlenW(json), b->utf8), nullptr))
            data = prJsonString(document, prJsonMember(document, 0, "data"));
        else
            data = PrStringView{};
        size_t decoded = 0;
        png.resize(uint32_t(base64DecodedCapacity(data.size)));
        ok = data.data != nullptr && base64Decode(data, png.data, png.count, &decoded);
        png.resize(uint32_t(decoded));
        prJsonDestroy(document);
    }
    if (ok && b->imaging == nullptr)
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&b->imaging)));
    ok = ok && decodePng(b->imaging, png, size, pixels);
    png.reset();
    if (ok)
        done(context, PrStatus_Ok, PrPixelView{pixels.data, size_t(size.x) * 4, size});
    else
        done(context, PrStatus_Io, PrPixelView{});
    pixels.reset();
}

// Captures `rect` of the document, in CSS pixels. Returns false if the engine refused the call, and calls nothing.
bool screenshot(Win32Browser* b, PrRect rect, PrUVec2 size, PrAppBrowserCaptureFn done, void* PR_NULLABLE context) {
    TextBuffer params;
    textAppend(&params, "{\"format\":\"png\",\"fromSurface\":true,\"clip\":{\"x\":");
    appendJsonNumber(&params, double(rect.left));
    textAppend(&params, ",\"y\":");
    appendJsonNumber(&params, double(rect.top));
    textAppend(&params, ",\"width\":");
    appendJsonNumber(&params, double(rect.right - rect.left));
    textAppend(&params, ",\"height\":");
    appendJsonNumber(&params, double(rect.bottom - rect.top));
    textAppend(&params, ",\"scale\":1}}");
    win32Widen(textView(&params), b->utf16);
    textReset(&params);
    HRESULT called = b->view->CallDevToolsProtocolMethod(L"Page.captureScreenshot", b->utf16.data,
        Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [b = hold(b), size, done, context](HRESULT error, LPCWSTR json) -> HRESULT {
                if (unhold(b))
                    deliverCapture(b, error, json, size, done, context);
                return S_OK;
            }) .Get());
    if (FAILED(called))
        unhold(b);
    return SUCCEEDED(called);
}

// Reads the position that the viewport has scrolled to in the document, in CSS pixels, from the engine's answer to
// Page.getLayoutMetrics.
bool scrollOf(Win32Browser* b, LPCWSTR json, PrVec2* out) {
    PrJson* document = prJsonCreate();
    bool ok = prJsonParse(document, win32Narrow(json, lstrlenW(json), b->utf8), nullptr);
    PrJsonValueIndex viewport = prJsonMember(document, 0, "cssVisualViewport");
    ok = ok && viewport != kPrJsonMissing;
    *out = PrVec2{
        float(prJsonNumber(document, prJsonMember(document, viewport, "pageX"), 0)),
        float(prJsonNumber(document, prJsonMember(document, viewport, "pageY"), 0)),
    };
    prJsonDestroy(document);
    return ok;
}

// The engine's screenshot clips a rect of the document, so the rect of the viewport moves to where the viewport has
// scrolled.
void capture(Win32Browser* b, Request& request) {
    PrRect rect = request.rect;
    PrUVec2 size = request.size;
    PrAppBrowserCaptureFn done = request.capture;
    void* context = request.context;
    HRESULT called = b->view->CallDevToolsProtocolMethod(L"Page.getLayoutMetrics", L"{}",
        Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [b = hold(b), rect, size, done, context](HRESULT error, LPCWSTR json) -> HRESULT {
                if (!unhold(b))
                    return S_OK;
                PrVec2 scroll{};
                bool ok = SUCCEEDED(error) && json != nullptr && scrollOf(b, json, &scroll);
                PrRect moved{rect.left + scroll.x, rect.top + scroll.y, rect.right + scroll.x, rect.bottom + scroll.y};
                if (!ok || !screenshot(b, moved, size, done, context))
                    done(context, PrStatus_Io, PrPixelView{});
                return S_OK;
            }) .Get());
    if (FAILED(called)) {
        unhold(b);
        fail(b, request, PrStatus_Io);
    }
}

void print(Win32Browser* b, Request& request) {
    ICoreWebView2_7* view = nullptr;
    ICoreWebView2Environment6* environment = nullptr;
    ICoreWebView2PrintSettings* settings = nullptr;
    Array<wchar_t> path;
    bool ok = SUCCEEDED(b->view->QueryInterface(IID_PPV_ARGS(&view))) &&
        SUCCEEDED(b->environment->QueryInterface(IID_PPV_ARGS(&environment))) &&
        SUCCEEDED(environment->CreatePrintSettings(&settings));
    if (ok) {
        // A CSS px is a 96th of an inch.
        PrVec2 paper = request.paper;
        if (paper.x > 0 && paper.y > 0) {
            settings->put_PageWidth(double(paper.x) / 96.0);
            settings->put_PageHeight(double(paper.y) / 96.0);
        }
        settings->put_MarginTop(0);
        settings->put_MarginBottom(0);
        settings->put_MarginLeft(0);
        settings->put_MarginRight(0);
        settings->put_ShouldPrintBackgrounds(TRUE);
        settings->put_ShouldPrintHeaderAndFooter(FALSE);
        // The engine writes only to an absolute path.
        win32Widen(PrStringView{request.text.data, request.text.count}, b->utf16);
        path.resize(GetFullPathNameW(b->utf16.data, 0, nullptr, nullptr) + 1);
        ok = GetFullPathNameW(b->utf16.data, path.count, path.data, nullptr) != 0;
    }
    if (ok) {
        PrAppBrowserDoneFn done = request.done;
        void* context = request.context;
        auto printed = [b = hold(b), done, context](HRESULT error, BOOL written) -> HRESULT {
            if (unhold(b))
                done(context, SUCCEEDED(error) && written ? PrStatus_Ok : PrStatus_Io);
            return S_OK;
        };
        ok = SUCCEEDED(
            view->PrintToPdf(path.data, settings, Callback<ICoreWebView2PrintToPdfCompletedHandler>(printed).Get()));
        if (!ok)
            unhold(b);
    }
    path.reset();
    release(settings);
    release(environment);
    release(view);
    if (ok)
        request.text.reset();
    else
        fail(b, request, PrStatus_Io);
}

void perform(Win32Browser* b, Request& request) {
    switch (request.kind) {
        case RequestKind_Load: navigate(b, request); break;
        case RequestKind_Evaluate: evaluate(b, request); break;
        case RequestKind_Capture: capture(b, request); break;
        case RequestKind_Print: print(b, request); break;
        default: break;
    }
}

// Sends a call to the engine, keeps it until the engine starts, or fails it if the engine did not start.
void submit(Win32Browser* b, Request& request) {
    if (b->failed)
        fail(b, request, PrStatus_Io);
    else if (!b->started)
        b->waiting.push(request);
    else
        perform(b, request);
}

// Fails the calls that wait for an engine that did not start, and every call after them.
void abandon(Win32Browser* b) {
    b->failed = true;
    for (uint32_t i = 0; i < b->waiting.count; ++i)
        fail(b, b->waiting.data[i], PrStatus_Io);
    b->waiting.clear();
}

void start(Win32Browser* b, ICoreWebView2Controller* controller) {
    controller->AddRef();
    b->controller = controller;
    if (FAILED(controller->get_CoreWebView2(&b->view)) || b->view == nullptr) {
        abandon(b);
        return;
    }
    // The application sets the page's pixel ratio, whichever display the page is on.
    ICoreWebView2Controller3* scaled = nullptr;
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&scaled)))) {
        scaled->put_ShouldDetectMonitorScaleChanges(FALSE);
        scaled->Release();
    }
    // The page's zoom is the application's, and no status bubble covers what a capture shows.
    ICoreWebView2Settings* settings = nullptr;
    if (SUCCEEDED(b->view->get_Settings(&settings))) {
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->Release();
    }
    ICoreWebView2_13* profiled = nullptr;
    ICoreWebView2Profile* profile = nullptr;
    if (SUCCEEDED(b->view->QueryInterface(IID_PPV_ARGS(&profiled))) && SUCCEEDED(profiled->get_Profile(&profile)))
        profile->put_PreferredColorScheme(b->colorScheme);
    release(profile);
    release(profiled);
    b->view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [b](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            UINT64 id = 0;
            if (SUCCEEDED(args->get_NavigationId(&id)))
                b->navigation = id;
            return S_OK;
        }) .Get(), &b->starting);
    // A navigation that a later one replaced completes after the later one starts, and is not the pending load's.
    b->view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [b](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            UINT64 id = 0;
            BOOL success = FALSE;
            if (FAILED(args->get_NavigationId(&id)) || id != b->navigation)
                return S_OK;
            args->get_IsSuccess(&success);
            finishLoad(b, success ? PrStatus_Ok : PrStatus_NotFound);
            return S_OK;
        }) .Get(), &b->completed);
    b->started = true;
    layOut(b);
    controller->put_IsVisible(TRUE);
    Array<Request> waiting = b->waiting;
    b->waiting = Array<Request>{};
    for (uint32_t i = 0; i < waiting.count; ++i)
        perform(b, waiting.data[i]);
    waiting.reset();
}

// Creates the page's controller in the engine's environment, in private mode where the engine has it, so that the page
// keeps no data.
void createController(Win32Browser* b) {
    auto created = [b = hold(b)](HRESULT error, ICoreWebView2Controller* controller) -> HRESULT {
        bool alive = unhold(b);
        b->creating = false;
        if (!alive) {
            if (SUCCEEDED(error) && controller != nullptr)
                controller->Close();
            teardown(b);
        } else if (FAILED(error) || controller == nullptr) {
            abandon(b);
        } else {
            start(b, controller);
        }
        return S_OK;
    };
    ICoreWebView2Environment10* environment = nullptr;
    ICoreWebView2ControllerOptions* options = nullptr;
    HRESULT called = E_FAIL;
    if (SUCCEEDED(b->environment->QueryInterface(IID_PPV_ARGS(&environment))) &&
            SUCCEEDED(environment->CreateCoreWebView2ControllerOptions(&options))) {
        options->put_IsInPrivateModeEnabled(TRUE);
        called = environment->CreateCoreWebView2ControllerWithOptions(
            b->window, options, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(created).Get());
    } else {
        called = b->environment->CreateCoreWebView2Controller(
            b->window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(created).Get());
    }
    release(options);
    release(environment);
    if (FAILED(called)) {
        b->creating = false;
        if (unhold(b))
            abandon(b);
        else
            teardown(b);
    }
}

Request requestOf(RequestKind kind, void* PR_NULLABLE context) {
    Request request{};
    request.kind = kind;
    request.context = context;
    return request;
}

void copyText(Request& request, PrStringView text) {
    if (text.size != 0)
        request.text.append(text.data, uint32_t(text.size));
}

} // namespace

void win32BrowserDestroy(PrApp*, PrAppBrowser* browser) {
    Win32Browser* b = static_cast<Win32Browser*>(browser);
    b->destroyed = true;
    b->loaded = nullptr;
    if (!b->creating)
        teardown(b);
}

void win32BrowserResize(PrApp*, PrAppBrowser* browser, PrUVec2 viewport) {
    Win32Browser* b = static_cast<Win32Browser*>(browser);
    b->viewport = viewport;
    layOut(b);
}

void win32BrowserShow(PrApp*, PrAppBrowser* browser, PrRect clip, PrRect frame) {
    Win32Browser* b = static_cast<Win32Browser*>(browser);
    if (b->host == nullptr)
        return;
    if (b->clip == nullptr)
        b->clip = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0,
            b->host, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (b->clip == nullptr)
        return;
    b->shown = true;
    b->shownClip = clip;
    b->shownFrame = frame;
    layOut(b);
}

void win32BrowserHide(PrApp*, PrAppBrowser* browser) {
    Win32Browser* b = static_cast<Win32Browser*>(browser);
    if (!b->shown)
        return;
    b->shown = false;
    layOut(b);
    ShowWindow(b->clip, SW_HIDE);
}

void win32BrowserLoad(PrApp*, PrAppBrowser* browser, PrStringView url, PrAppBrowserDoneFn done, void* PR_NULLABLE context) {
    Request request = requestOf(RequestKind_Load, context);
    request.done = done;
    copyText(request, url);
    submit(static_cast<Win32Browser*>(browser), request);
}

void win32BrowserEvaluate(PrApp*, PrAppBrowser* browser, PrStringView script, PrAppBrowserResultFn done,
        void* PR_NULLABLE context) {
    Request request = requestOf(RequestKind_Evaluate, context);
    request.result = done;
    copyText(request, script);
    submit(static_cast<Win32Browser*>(browser), request);
}

// The engine's screenshot renders a frame of the page's latest changes and copies it, so it shows what the page's
// scripts have changed.
void win32BrowserCapture(PrApp*, PrAppBrowser* browser, PrRect rect, PrUVec2 size, PrAppBrowserCaptureFn done,
        void* PR_NULLABLE context) {
    Request request = requestOf(RequestKind_Capture, context);
    request.capture = done;
    request.rect = rect;
    request.size = size;
    submit(static_cast<Win32Browser*>(browser), request);
}

void win32BrowserPrint(PrApp*, PrAppBrowser* browser, PrStringView path, PrVec2 paper, PrAppBrowserDoneFn done,
        void* PR_NULLABLE context) {
    Request request = requestOf(RequestKind_Print, context);
    request.done = done;
    request.paper = paper;
    copyText(request, path);
    submit(static_cast<Win32Browser*>(browser), request);
}

// The engine names the kind of each process that it runs, but not which service a utility process runs, so the network
// process is left out.
uint32_t win32BrowserProcesses(PrApp*, PrAppBrowser* browser, PrAppBrowserProcess* PR_NULLABLE out, uint32_t capacity) {
    Win32Browser* b = static_cast<Win32Browser*>(browser);
    ICoreWebView2Environment8* environment = nullptr;
    ICoreWebView2ProcessInfoCollection* processes = nullptr;
    UINT total = 0;
    if (b->environment == nullptr || FAILED(b->environment->QueryInterface(IID_PPV_ARGS(&environment))) ||
        FAILED(environment->GetProcessInfos(&processes)) || FAILED(processes->get_Count(&total)))
    total = 0;
    uint32_t count = 0;
    for (UINT i = 0; i < total; ++i) {
        ICoreWebView2ProcessInfo* process = nullptr;
        COREWEBVIEW2_PROCESS_KIND kind = COREWEBVIEW2_PROCESS_KIND_BROWSER;
        INT32 id = 0;
        if (SUCCEEDED(processes->GetValueAtIndex(i, &process))) {
            process->get_Kind(&kind);
            process->get_ProcessId(&id);
            process->Release();
        }
        PrAppBrowserProcess found{};
        found.id = id;
        if (kind == COREWEBVIEW2_PROCESS_KIND_RENDERER)
            found.role = PrAppBrowserProcessRole_Content;
        else if (kind == COREWEBVIEW2_PROCESS_KIND_GPU)
            found.role = PrAppBrowserProcessRole_Gpu;
        else
            continue;
        if (id <= 0)
            continue;
        if (count < capacity)
            out[count] = found;
        ++count;
    }
    release(processes);
    release(environment);
    return count;
}

PrStatus win32BrowserCreate(PrApp* app, void* PR_NULLABLE host, const PrAppBrowserDesc* desc,
        PrAppBrowser* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    LPWSTR version = nullptr;
    HRESULT found = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    bool installed = SUCCEEDED(found) && version != nullptr;
    CoTaskMemFree(version);
    if (!installed)
        return PrStatus_NotSupported;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    registerClass(instance);
    Win32Browser* b = allocZeroed<Win32Browser>();
    b->app = app;
    b->host = static_cast<HWND>(host);
    b->viewport = desc->viewport;
    b->pixelRatio = desc->pixelRatio;
    b->colorScheme = COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO;
    if (desc->preferences != nullptr) {
        bool dark = desc->preferences->colorScheme == PrAppColorScheme_Dark;
        b->colorScheme = dark ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT;
    }
    // The window is shown, so that the page renders, and never activates, so that the application keeps the focus.
    b->window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr,
        nullptr, instance, b);
    if (b->window == nullptr) {
        prMemoryFree(b);
        return PrStatus_Io;
    }
    layOut(b);
    ShowWindow(b->window, SW_SHOWNOACTIVATE);
    Array<wchar_t> folder;
    folder.resize(GetTempPathW(0, nullptr) + 1);
    DWORD length = GetTempPathW(folder.count, folder.data);
    folder.resize(length < folder.count ? length : 0);
    folder.append(kDataFolder, uint32_t(countOf(kDataFolder)));
    // The engine may complete the creation within the call, and the completion ends the creation.
    b->creating = true;
    ComPtr<CoreWebView2EnvironmentOptions> options = Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(kArguments);
    HRESULT called = CreateCoreWebView2EnvironmentWithOptions(nullptr, folder.data, options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [b = hold(b)](HRESULT error, ICoreWebView2Environment* environment) -> HRESULT {
                bool alive = unhold(b);
                if (FAILED(error) || environment == nullptr) {
                    b->creating = false;
                    if (alive)
                        abandon(b);
                    else
                        teardown(b);
                    return S_OK;
                }
                environment->AddRef();
                b->environment = environment;
                createController(b);
                return S_OK;
            }) .Get());
    folder.reset();
    if (FAILED(called)) {
        b->creating = false;
        unhold(b);
        b->destroyed = true;
        teardown(b);
        return PrStatus_Io;
    }
    *out = b;
    return PrStatus_Ok;
}

} // namespace pr_impl

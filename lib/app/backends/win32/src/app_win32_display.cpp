// The Win32 backend's displays: their list, their properties, and positions on them.

#include "app/backends/win32/src/app_win32_display.h"

namespace pr_impl {

namespace {

// The luminance of SDR white in nits on `monitor`, which the user sets for a display that shows HDR.
float sdrWhiteNits(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (!GetMonitorInfoW(monitor, &info) ||
            GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return kDefaultSdrWhiteNits;
    RawArray<DISPLAYCONFIG_PATH_INFO> paths;
    RawArray<DISPLAYCONFIG_MODE_INFO> modes;
    paths.resize(pathCount);
    modes.resize(modeCount);
    float nits = kDefaultSdrWhiteNits;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data, &modeCount, modes.data, nullptr) ==
            ERROR_SUCCESS) {
        for (uint32_t i = 0; i < pathCount; ++i) {
            const DISPLAYCONFIG_PATH_INFO& path = paths.data[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                    CompareStringOrdinal(source.viewGdiDeviceName, -1, info.szDevice, -1, TRUE) != CSTR_EQUAL)
                continue;
            DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
            white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            white.header.size = sizeof(white);
            white.header.adapterId = path.targetInfo.adapterId;
            white.header.id = path.targetInfo.id;
            // The level is in thousandths of 80 nits.
            if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel != 0)
                nits = float(white.SDRWhiteLevel) / 1000.0f * 80.0f;
            break;
        }
    }
    paths.reset();
    modes.reset();
    return nits;
}

// The chromaticities of an RGB space's primaries, red, green then blue, as CIE xy.
struct Primaries {
    PrColorSpace space;
    float xy[6];
};

constexpr Primaries kGamuts[] = {
    {PrColorSpace_Srgb, {0.640f, 0.330f, 0.300f, 0.600f, 0.150f, 0.060f}},
    {PrColorSpace_DisplayP3, {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f}},
    {PrColorSpace_Rec2020, {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f}},
};

// The RGB space whose primaries are nearest the display's.
PrColorSpace gamutOf(const DXGI_OUTPUT_DESC1& desc) {
    const float display[6] = {
        desc.RedPrimary[0],   desc.RedPrimary[1],  desc.GreenPrimary[0],
        desc.GreenPrimary[1], desc.BluePrimary[0], desc.BluePrimary[1],
    };
    PrColorSpace nearest = PrColorSpace_Srgb;
    float nearestDistance = 0;
    for (uint32_t g = 0; g < countOf(kGamuts); ++g) {
        float distance = 0;
        for (uint32_t i = 0; i < 6; ++i)
            distance += (display[i] - kGamuts[g].xy[i]) * (display[i] - kGamuts[g].xy[i]);
        if (g == 0 || distance < nearestDistance) {
            nearest = kGamuts[g].space;
            nearestDistance = distance;
        }
    }
    return nearest;
}

// Returns the id of the monitor whose device is `device`: the hash of the interface name of the display device that it
// shows, which stays the same across connections, or of the device's name if it has none. It is never zero.
PrAppDisplayId monitorIdOf(const wchar_t* device) {
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);
    const wchar_t* name = device;
    if (EnumDisplayDevicesW(device, 0, &adapter, EDD_GET_DEVICE_INTERFACE_NAME) && adapter.DeviceID[0] != 0)
        name = adapter.DeviceID;
    RawArray<char> utf8;
    PrStringView text = windowsNarrow(name, uint32_t(lstrlenW(name)), utf8);
    PrAppDisplayId id = prHashBytes(text.data, text.size, 0);
    utf8.reset();
    return id != kPrAppDisplayNone ? id : 1;
}

// The monitors, the primary first.
void monitorsOf(RawArray<Win32Monitor>& out) {
    out.clear();
    auto add = [](HMONITOR handle, HDC, LPRECT, LPARAM context) -> BOOL {
        RawArray<Win32Monitor>& monitors = *reinterpret_cast<RawArray<Win32Monitor>*>(context);
        Win32Monitor monitor{};
        if (win32MonitorOf(handle, &monitor)) {
            monitors.push(monitor);
            if (monitor.primary && monitors.count > 1) {
                Win32Monitor first = monitors.data[0];
                monitors.data[0] = monitor;
                monitors.data[monitors.count - 1] = first;
            }
        }
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, add, reinterpret_cast<LPARAM>(&out));
}

// Returns the orientation of a display whose current bounds are `bounds` and which `turn` rotates. Windows names no
// natural orientation, so a display that is taller than wide is a portrait, flipped if it is turned by 270 degrees, and
// one that is wider is a landscape, flipped if it is turned by 180.
PrAppOrientation orientationOf(DWORD turn, PrRect bounds) {
    if (bounds.bottom - bounds.top > bounds.right - bounds.left)
        return turn == DMDO_270 ? PrAppOrientation_PortraitFlipped : PrAppOrientation_Portrait;
    return turn == DMDO_180 ? PrAppOrientation_LandscapeFlipped : PrAppOrientation_Landscape;
}

// Returns the name of the monitor's display as the system shows it, which the application keeps until the next
// DisplaysChanged event.
PrStringView displayNameOf(const Win32App* app, const Win32Monitor& monitor) {
    PrStringView kept{};
    if (appDisplayListFind(app->displays, monitor.id, &kept))
        return kept;
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (!EnumDisplayDevicesW(monitor.device, 0, &device, 0))
        return PrStringView{};
    PrStringView name = windowsNarrow(device.DeviceString, uint32_t(wcslen(device.DeviceString)), app->utf8);
    return appDisplayListAdd(app->displays, monitor.id, name);
}

// Returns what the application reports of `monitor`. `factory` gives the monitor's output, for its colors, or null.
PrAppDisplayInfo displayInfoOf(const Win32App* app, const Win32Monitor& monitor, IDXGIFactory1* PR_NULLABLE factory) {
    IDXGIOutput6* output = factory != nullptr ? win32OutputOf(factory, monitor.handle) : nullptr;
    PrAppDisplay display = win32DisplayOfOutput(output, monitor.handle);
    if (output != nullptr)
        output->Release();
    display.pixelRatio = float(monitor.dpi) / 96.0f;
    PrRect bounds = win32LogicalOn(monitor, monitor.bounds);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    bool moded = EnumDisplaySettingsW(monitor.device, ENUM_CURRENT_SETTINGS, &mode) != 0;
    return PrAppDisplayInfo{
        .id = monitor.id,
        .name = displayNameOf(app, monitor),
        .display = display,
        .extent = PrVec2{bounds.right, bounds.bottom},
        .workArea = win32LogicalOn(monitor, monitor.work),
        .refreshRate = moded && mode.dmDisplayFrequency > 1 ? float(mode.dmDisplayFrequency) : 0.0f,
        .orientation = orientationOf(moded ? mode.dmDisplayOrientation : DMDO_DEFAULT, bounds),
        .main = monitor.primary,
    };
}

// Returns a factory of DXGI, which the caller releases, or null if it cannot be made.
IDXGIFactory1* PR_NULLABLE dxgiFactory() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return nullptr;
    return factory;
}

} // namespace

IDXGIOutput6* PR_NULLABLE win32OutputOf(IDXGIFactory1* factory, HMONITOR monitor) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; SUCCEEDED(factory->EnumAdapters1(a, &adapter)); ++a) {
        IDXGIOutput* output = nullptr;
        IDXGIOutput6* found = nullptr;
        for (UINT o = 0; found == nullptr && SUCCEEDED(adapter->EnumOutputs(o, &output)); ++o) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
                output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&found));
            output->Release();
        }
        adapter->Release();
        if (found != nullptr)
            return found;
    }
    return nullptr;
}

DWORD win32PersonalizeValue(const wchar_t* name, DWORD fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) !=
            ERROR_SUCCESS)
        return fallback;
    return value;
}

PrAppDisplay win32DisplayOfOutput(IDXGIOutput6* PR_NULLABLE output, HMONITOR monitor) {
    PrAppDisplay display = prAppDisplayDefault();
    DXGI_OUTPUT_DESC1 desc;
    if (output != nullptr && SUCCEEDED(output->GetDesc1(&desc)) &&
            desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
        float headroom = desc.MaxLuminance / sdrWhiteNits(monitor);
        display.headroom = headroom > 1 ? headroom : 1;
        display.potentialHeadroom = display.headroom;
        display.gamut = gamutOf(desc);
    }
    return display;
}

bool win32MonitorOf(HMONITOR handle, Win32Monitor* out) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info))
        return false;
    UINT x = 96;
    UINT y = 96;
    if (FAILED(GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &x, &y)) || x == 0)
        x = 96;
    *out = Win32Monitor{
        .handle = handle,
        .bounds = info.rcMonitor,
        .work = info.rcWork,
        .dpi = x,
        .primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0,
        .id = monitorIdOf(info.szDevice),
    };
    wcsncpy_s(out->device, info.szDevice, _TRUNCATE);
    return true;
}

bool win32MonitorWithId(PrAppDisplayId id, Win32Monitor* out) {
    RawArray<Win32Monitor> monitors;
    monitorsOf(monitors);
    bool found = false;
    for (uint32_t i = 0; i < monitors.count && !found; ++i) {
        found = monitors.data[i].id == id;
        if (found)
            *out = monitors.data[i];
    }
    monitors.reset();
    return found;
}

bool win32PrimaryMonitor(Win32Monitor* out) {
    return win32MonitorOf(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), out);
}

PrRect win32LogicalOn(const Win32Monitor& monitor, RECT rect) {
    float scale = 96.0f / float(monitor.dpi);
    return PrRect{
        float(rect.left - monitor.bounds.left) * scale,
        float(rect.top - monitor.bounds.top) * scale,
        float(rect.right - monitor.bounds.left) * scale,
        float(rect.bottom - monitor.bounds.top) * scale,
    };
}

void win32NoteDisplaysChanged(Win32App* app) {
    if (app->displaysChanged || app->messageWindow == nullptr)
        return;
    app->displaysChanged = true;
    PostMessageW(app->messageWindow, kMessageDisplaysChanged, 0, 0);
}

uint32_t win32Displays(const PrApp* base, PrAppDisplayInfo* PR_NULLABLE out, uint32_t capacity) {
    const Win32App* app = static_cast<const Win32App*>(base);
    RawArray<Win32Monitor> monitors;
    monitorsOf(monitors);
    IDXGIFactory1* factory = capacity != 0 ? dxgiFactory() : nullptr;
    for (uint32_t i = 0; i < monitors.count && i < capacity; ++i)
        out[i] = displayInfoOf(app, monitors.data[i], factory);
    if (factory != nullptr)
        factory->Release();
    uint32_t count = monitors.count;
    monitors.reset();
    return count;
}

bool win32DisplayInfo(const PrApp* base, PrAppDisplayId id, PrAppDisplayInfo* out) {
    Win32Monitor monitor{};
    if (!win32MonitorWithId(id, &monitor))
        return false;
    IDXGIFactory1* factory = dxgiFactory();
    *out = displayInfoOf(static_cast<const Win32App*>(base), monitor, factory);
    if (factory != nullptr)
        factory->Release();
    return true;
}

bool win32PointerPosition(const PrApp* base, PrAppDisplayPosition* out) {
    const Win32App* app = static_cast<const Win32App*>(base);
    POINT at{};
    Win32Monitor monitor{};
    if (app->headless || !GetCursorPos(&at) || !win32MonitorOf(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    PrRect logical = win32LogicalOn(monitor, RECT{at.x, at.y, at.x, at.y});
    *out = PrAppDisplayPosition{.display = monitor.id, .position = PrVec2{logical.left, logical.top}};
    return true;
}

} // namespace pr_impl

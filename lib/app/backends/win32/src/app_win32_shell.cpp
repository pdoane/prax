// The Win32 backend's shell and device: notifications, URLs, the trash, recent documents, the taskbar, power and
// keeping awake.

#include "app/backends/win32/src/app_win32_shell.h"

#include "app/backends/win32/src/app_win32_window.h"

namespace pr_impl {

namespace {

// Returns true if `text` is an absolute URL: a scheme (RFC 3986, section 3.1), a colon, and characters that a URL holds
// unescaped. A scheme of one letter is a drive, and the text a path.
bool isUrl(PrStringView text) {
    size_t colon = 0;
    while (colon < text.size && text.data[colon] != ':')
        ++colon;
    if (colon < 2 || colon == text.size)
        return false;
    for (size_t i = 0; i < colon; ++i) {
        char c = text.data[i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        bool other = (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!letter && (i == 0 || !other))
            return false;
    }
    for (size_t i = colon + 1; i < text.size; ++i) {
        unsigned char c = static_cast<unsigned char>(text.data[i]);
        if (c <= 0x20 || c == 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' ||
                c == '{' || c == '|' || c == '}')
            return false;
    }
    return true;
}

// Returns an icon of `text` in white on a red circle, the size of a small icon, for the taskbar's overlay.
HICON PR_NULLABLE badgeIconOf(const wchar_t* text) {
    int size = GetSystemMetrics(SM_CXSMICON);
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (color == nullptr)
        return nullptr;
    HDC context = CreateCompatibleDC(nullptr);
    HGDIOBJ previous = SelectObject(context, color);
    HBRUSH red = CreateSolidBrush(RGB(220, 40, 40));
    HGDIOBJ previousBrush = SelectObject(context, red);
    HGDIOBJ previousPen = SelectObject(context, GetStockObject(NULL_PEN));
    Ellipse(context, 0, 0, size + 1, size + 1);
    SetBkMode(context, TRANSPARENT);
    SetTextColor(context, RGB(255, 255, 255));
    RECT bounds{0, 0, size, size};
    DrawTextW(context, text, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(context, previousPen);
    SelectObject(context, previousBrush);
    SelectObject(context, previous);
    DeleteObject(red);
    DeleteDC(context);
    // GDI leaves the alpha at zero, so the circle's pixels become opaque, and the others transparent.
    uint32_t* pixels = static_cast<uint32_t*>(bits);
    float radius = float(size) / 2;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = float(x) + 0.5f - radius;
            float dy = float(y) + 0.5f - radius;
            bool inside = dx * dx + dy * dy <= radius * radius;
            uint32_t& pixel = pixels[y * size + x];
            pixel = inside ? (pixel | 0xFF000000u) : 0;
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{.fIcon = TRUE, .hbmMask = mask, .hbmColor = color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

// Adds the application's icon to the notification area, if it is not there, with the tip of its first window's title.
// Notifications appear as balloons on this icon. Returns false if the shell refuses the icon.
bool addNotifyIcon(Win32App* app) {
    if (app->notifyIcon)
        return true;
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = app->messageWindow;
    icon.uID = kNotifyIconId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = kMessageNotifyIcon;
    icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(reinterpret_cast<ULONG_PTR>(IDI_APPLICATION)));
    if (app->windows.count != 0)
        lstrcpynW(icon.szTip, app->windows.data[0]->title, int(countOf(icon.szTip)));
    if (!Shell_NotifyIconW(NIM_ADD, &icon))
        return false;
    icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon);
    app->notifyIcon = true;
    return true;
}

} // namespace

void win32NotifyIconAction(Win32App* app, UINT action) {
    switch (action) {
        case NIN_BALLOONUSERCLICK: {
            PrAppEvent event{};
            event.kind = PrAppEventKind_Notification;
            event.tag = app->notificationTag;
            event.time = win32SecondsNow();
            win32Deliver(app, nullptr, event);
            break;
        }
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (app->lastActive != nullptr)
                win32BringForward(app->lastActive);
            else if (app->windows.count != 0)
                win32BringForward(app->windows.data[app->windows.count - 1]);
            break;
        default:
            break;
    }
}

ITaskbarList3* PR_NULLABLE win32TaskbarOf(Win32App* app) {
    if (app->taskbar != nullptr || !app->ole)
        return app->taskbar;
    ITaskbarList3* taskbar = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar))))
        return nullptr;
    if (FAILED(taskbar->HrInit())) {
        taskbar->Release();
        return nullptr;
    }
    app->taskbar = taskbar;
    return taskbar;
}

void win32ApplyTaskbar(Win32Window* window) {
    Win32App* app = appOf(window);
    ITaskbarList3* taskbar = win32TaskbarOf(app);
    if (taskbar == nullptr || window->role != PrAppWindowRole_Normal)
        return;
    TBPFLAG flag = TBPF_NOPROGRESS;
    switch (window->progressState) {
        case PrAppProgressState_Normal:
            flag = TBPF_NORMAL;
            break;
        case PrAppProgressState_Indeterminate:
            flag = TBPF_INDETERMINATE;
            break;
        case PrAppProgressState_Paused:
            flag = TBPF_PAUSED;
            break;
        case PrAppProgressState_Error:
            flag = TBPF_ERROR;
            break;
        default:
            break;
    }
    taskbar->SetProgressState(window->window, flag);
    if (flag != TBPF_NOPROGRESS && flag != TBPF_INDETERMINATE) {
        float clamped = window->progress < 0 ? 0 : window->progress > 1 ? 1 : window->progress;
        taskbar->SetProgressValue(window->window, ULONGLONG(clamped * 10000.0f), 10000);
    }
    const wchar_t* description = app->badgeText.count != 0 ? app->badgeText.data : L"";
    taskbar->SetOverlayIcon(window->window, app->badge, description);
}

void win32CopyField(Win32App* app, PrStringView text, wchar_t* out, size_t capacity) {
    windowsWiden(text, app->utf16);
    size_t count = app->utf16.count - 1;
    if (count > capacity - 1) {
        count = capacity - 1;
        wchar_t last = app->utf16.data[count - 1];
        if (last >= 0xD800 && last <= 0xDBFF)
            --count;
    }
    prMemCopy(out, app->utf16.data, count * sizeof(wchar_t));
    out[count] = 0;
}

PrStatus win32NotificationPost(PrApp* base, const PrAppNotificationDesc* desc) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running)
        return PrStatus_InvalidState;
    // A headless application posts nothing, because no user sees it.
    if (app->headless)
        return PrStatus_NotSupported;
    // A balloon without text removes the balloon, so a notification with only a title shows the title as its text. A
    // notification with neither posts nothing.
    PrStringView text = desc->body.size != 0 ? desc->body : desc->title;
    PrStringView title = desc->body.size != 0 ? desc->title : PrStringView{};
    if (text.size == 0)
        return PrStatus_Ok;
    if (!addNotifyIcon(app))
        return PrStatus_Io;
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = app->messageWindow;
    icon.uID = kNotifyIconId;
    icon.uFlags = NIF_INFO;
    icon.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    win32CopyField(app, title, icon.szInfoTitle, countOf(icon.szInfoTitle));
    win32CopyField(app, text, icon.szInfo, countOf(icon.szInfo));
    if (!Shell_NotifyIconW(NIM_MODIFY, &icon))
        return PrStatus_Io;
    app->notificationTag = desc->tag;
    return PrStatus_Ok;
}

PrStatus win32OpenUrl(PrApp* base, PrStringView url) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!isUrl(url))
        return PrStatus_Syntax;
    windowsWiden(url, app->utf16);
    HINSTANCE result = ShellExecuteW(nullptr, L"open", app->utf16.data, nullptr, nullptr, SW_SHOWNORMAL);
    // A result above 32 is success, as the function's older interface defines it.
    return reinterpret_cast<INT_PTR>(result) > 32 ? PrStatus_Ok : PrStatus_Io;
}

PrStatus win32RevealPath(PrApp* base, PrStringView path) {
    Win32App* app = static_cast<Win32App*>(base);
    RawArray<wchar_t> wide{};
    windowsWiden(path, wide);
    for (uint32_t i = 0; i < wide.count; ++i) {
        if (wide.data[i] == L'/')
            wide.data[i] = L'\\';
    }
    PrStatus status = PrStatus_Ok;
    PIDLIST_ABSOLUTE item = nullptr;
    if (GetFileAttributesW(wide.data) == INVALID_FILE_ATTRIBUTES)
        status = PrStatus_NotFound;
    else if (app->headless)
        status = PrStatus_NotSupported;
    else if (FAILED(SHParseDisplayName(wide.data, nullptr, &item, 0, nullptr)))
        status = PrStatus_Io;
    else if (FAILED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0)))
        status = PrStatus_Io;
    if (item != nullptr)
        CoTaskMemFree(item);
    wide.reset();
    return status;
}

PrStatus win32TrashPath(PrApp* base, PrStringView path) {
    Win32App* app = static_cast<Win32App*>(base);
    windowsWiden(path, app->utf16);
    if (GetFileAttributesW(app->utf16.data) == INVALID_FILE_ATTRIBUTES)
        return PrStatus_NotFound;
    // The shell's operation takes a list of paths, each ended by a NUL, and the list by another.
    app->utf16.push(0);
    SHFILEOPSTRUCTW operation{
        .wFunc = FO_DELETE,
        .pFrom = app->utf16.data,
        .fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT,
    };
    return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted ? PrStatus_Ok : PrStatus_Io;
}

void win32RecentDocumentAdd(PrApp* base, PrStringView path) {
    Win32App* app = static_cast<Win32App*>(base);
    windowsWiden(path, app->utf16);
    SHAddToRecentDocs(SHARD_PATHW, app->utf16.data);
}

void win32RecentDocumentsClear(PrApp*) {
    SHAddToRecentDocs(SHARD_PATHW, nullptr);
}

void win32SetBadge(PrApp* base, PrStringView badge) {
    Win32App* app = static_cast<Win32App*>(base);
    if (app->badge != nullptr)
        DestroyIcon(app->badge);
    app->badge = nullptr;
    app->badgeText.clear();
    if (badge.size != 0) {
        windowsWiden(badge, app->utf16);
        app->badgeText.append(app->utf16.data, app->utf16.count);
        app->badge = badgeIconOf(app->badgeText.data);
    }
    for (uint32_t i = 0; i < app->windows.count; ++i)
        win32ApplyTaskbar(app->windows.data[i]);
}

PrAppPower win32Power(const PrApp*) {
    PrAppPower result{.source = PrAppPowerSource_Unknown, .level = -1};
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status))
        return result;
    if (status.ACLineStatus == 1)
        result.source = PrAppPowerSource_External;
    else if (status.ACLineStatus == 0)
        result.source = PrAppPowerSource_Battery;
    // A battery flag of 128 means that the system has no battery, and a percentage of 255 that it is unknown.
    if (status.BatteryFlag != 128 && status.BatteryLifePercent <= 100)
        result.level = float(status.BatteryLifePercent) / 100.0f;
    result.charging = status.BatteryFlag != 128 && (status.BatteryFlag & 8) != 0;
    result.lowPower = status.SystemStatusFlag == 1;
    return result;
}

void win32SetAwake(PrApp*, PrAppAwakeFlags flags) {
    EXECUTION_STATE state = ES_CONTINUOUS;
    if ((flags & PrAppAwakeFlag_Display) != 0)
        state |= ES_DISPLAY_REQUIRED;
    if ((flags & PrAppAwakeFlag_System) != 0)
        state |= ES_SYSTEM_REQUIRED;
    SetThreadExecutionState(state);
}

} // namespace pr_impl

// The Win32 application backend: the application, its message window, the delivery of callbacks, and the system's
// notices.

#include "pr/app_win32.h"
#include "app/backends/win32/src/app_win32.h"

#include "app/backends/win32/src/app_win32_data.h"
#include "app/backends/win32/src/app_win32_display.h"
#include "app/backends/win32/src/app_win32_input.h"
#include "app/backends/win32/src/app_win32_menu.h"
#include "app/backends/win32/src/app_win32_shell.h"
#include "app/backends/win32/src/app_win32_window.h"

namespace pr_impl {

namespace {

// Calls the program with an event, as no other callback runs. A window that the program destroyed during the call is
// freed. The run stops after a QuitRequested event that the program did not cancel.
void callEvent(Win32App* app, Win32Window* PR_NULLABLE window, const PrAppEvent& event) {
    app->inCallback = true;
    bool quit = event.kind == PrAppEventKind_QuitRequested;
    app->quitDelivering = quit;
    app->quitCancelled = false;
    if (window == nullptr)
        app->handler.event(app->handler.context, app, &event);
    else if (!appWindowDeliverEvent(window->delivery, window, &event, win32FrameState))
        win32WindowDestroy(window);
    app->quitDelivering = false;
    app->inCallback = false;
    if (quit) {
        app->quitAccepted = !app->quitCancelled;
        if (app->quitAccepted)
            app->stopFn(app);
    }
}

// Calls the callback of an answer's request, as no other callback runs.
void callAnswer(Win32App* app, AppAnswer& answer) {
    app->inCallback = true;
    appAnswerCall(answer, app->requests, app->sources);
    app->inCallback = false;
}

bool serviceMessage(Win32App* app, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result);

// The procedure of the application's message window.
LRESULT CALLBACK messageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    Win32App* app = reinterpret_cast<Win32App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (app == nullptr)
        return DefWindowProcW(hwnd, message, wParam, lParam);
    if (message == app->taskbarCreated && app->taskbarCreated != 0)
        app->notifyIcon = false;
    LRESULT handled = 0;
    if (serviceMessage(app, message, wParam, lParam, &handled))
        return handled;
    switch (message) {
        case kMessageFrame:
            // The run's loop holds the pacer's message back, so another loop dispatched it: the system's modal loop for
            // a move or a resize. A frame that waits behind input goes to the window's frame timer, whose message comes
            // only when no other waits, so that the loop sees the input, such as the button's release that ends a
            // resize, even where a frame takes longer than a refresh.
            if (HIWORD(GetQueueStatus(QS_INPUT)) == 0) {
                win32RunRequestedFrames(app);
                return 0;
            }
            for (uint32_t i = 0; i < app->windows.count; ++i) {
                Win32Window* window = app->windows.data[i];
                if (window->frameRequested)
                    SetTimer(window->window, kTimerFrame, USER_TIMER_MINIMUM, nullptr);
            }
            return 0;
        case kMessageWake: {
            app->wakePending.store(false);
            PrAppEvent event{};
            event.kind = PrAppEventKind_Wake;
            event.time = win32SecondsNow();
            win32Deliver(app, nullptr, event);
            return 0;
        }
        case kMessageNotifyIcon:
            // With NOTIFYICON_VERSION_4, the low word of lParam is the user's action.
            win32NotifyIconAction(app, LOWORD(lParam));
            return 0;
        case kMessageStatusItem:
            win32StatusItemAction(app, HIWORD(lParam), LOWORD(lParam));
            return 0;
        case WM_INITMENUPOPUP:
            win32MenuOpening(app, nullptr, reinterpret_cast<HMENU>(wParam));
            return 0;
        case WM_COMMAND:
            win32MenuCommand(app, nullptr, LOWORD(wParam));
            return 0;
        case kMessageDisplaysChanged: {
            app->displaysChanged = false;
            // The names that the last event's displays had stay valid until this event, which may change them.
            appDisplayListReset(app->displays);
            PrAppEvent event{};
            event.kind = PrAppEventKind_DisplaysChanged;
            event.time = win32SecondsNow();
            win32Deliver(app, nullptr, event);
            return 0;
        }
        case kMessageRead:
            win32AnswerRequest(app, reinterpret_cast<Win32Request*>(lParam));
            return 0;
        case WM_HOTKEY:
            win32HotkeyPressed(app, int(wParam));
            return 0;
        case kMessageDialog:
            // A dialog's modal loop waits for the callback that runs, so that no callback runs within another.
            if (app->inCallback || app->dialog != nullptr)
                PostMessageW(hwnd, kMessageDialog, wParam, lParam);
            else
                win32ShowDialog(app, reinterpret_cast<Win32DialogRequest*>(lParam));
            return 0;
        case kMessageDialogCancel:
            win32CloseCancelledDialog(app);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void registerClasses(HINSTANCE instance) {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (!GetClassInfoExW(instance, kClassName, &existing)) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = win32WindowProc;
        windowClass.hInstance = instance;
        windowClass.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(reinterpret_cast<ULONG_PTR>(IDI_APPLICATION)));
        windowClass.lpszClassName = kClassName;
        RegisterClassExW(&windowClass);
    }
    if (!GetClassInfoExW(instance, kMessageClassName, &existing)) {
        WNDCLASSEXW messageClass{};
        messageClass.cbSize = sizeof(messageClass);
        messageClass.lpfnWndProc = messageProc;
        messageClass.hInstance = instance;
        messageClass.lpszClassName = kMessageClassName;
        RegisterClassExW(&messageClass);
    }
    if (!GetClassInfoExW(instance, kDimClassName, &existing)) {
        WNDCLASSEXW dimClass{};
        dimClass.cbSize = sizeof(dimClass);
        dimClass.lpfnWndProc = DefWindowProcW;
        dimClass.hInstance = instance;
        dimClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        dimClass.lpszClassName = kDimClassName;
        RegisterClassExW(&dimClass);
    }
}

// Returns the application's lifecycle: Active while one of its windows is the foreground window, Background while none
// of its windows shows, and Inactive otherwise.
PrAppLifecycle computedLifecycle(const Win32App* app) {
    if (app->headless)
        return PrAppLifecycle_Background;
    HWND foreground = GetForegroundWindow();
    bool shows = false;
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        HWND window = app->windows.data[i]->window;
        if (window == foreground)
            return PrAppLifecycle_Active;
        if (IsWindowVisible(window) && !IsIconic(window))
            shows = true;
    }
    return shows ? PrAppLifecycle_Inactive : PrAppLifecycle_Background;
}

// Takes the single instance's mutex, or hands this launch's command line to the instance that holds it and returns
// false. The first instance's message window carries the instance's name as its title.
bool claimSingleInstance(Win32App* app) {
    if (app->instanceName.count == 0)
        return true;
    RawArray<wchar_t> mutexName;
    mutexName.append(L"Local\\", 6);
    mutexName.append(app->instanceName.data, app->instanceName.count);
    app->instanceMutex = CreateMutexW(nullptr, FALSE, mutexName.data);
    DWORD error = GetLastError();
    mutexName.reset();
    if (app->instanceMutex != nullptr && error != ERROR_ALREADY_EXISTS)
        return true;
    if (app->instanceMutex != nullptr)
        CloseHandle(app->instanceMutex);
    app->instanceMutex = nullptr;
    HWND first = FindWindowW(kMessageClassName, app->instanceName.data);
    if (first != nullptr) {
        const wchar_t* line = GetCommandLineW();
        COPYDATASTRUCT data{
            .dwData = kCopyDataOpen,
            .cbData = DWORD((wcslen(line) + 1) * sizeof(wchar_t)),
            .lpData = const_cast<wchar_t*>(line),
        };
        DWORD process = 0;
        GetWindowThreadProcessId(first, &process);
        AllowSetForegroundWindow(process);
        SendMessageW(first, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
    }
    return false;
}

// Delivers the request of a second launch, whose command line `line` holds, with its arguments past the program.
void openCommandLine(Win32App* app, const wchar_t* line) {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(line, &count);
    if (arguments == nullptr)
        return;
    AppStrings strings{};
    for (int i = 1; i < count; ++i) {
        appStringsAdd(strings, windowsNarrow(arguments[i], uint32_t(wcslen(arguments[i])), app->utf8));
    }
    LocalFree(arguments);
    if (app->lastActive != nullptr)
        win32BringForward(app->lastActive);
    PrAppEvent event{};
    event.kind = PrAppEventKind_Open;
    event.time = win32SecondsNow();
    event.open.arguments = appStringsViews(strings);
    event.open.argumentCount = strings.ends.count;
    win32Deliver(app, nullptr, event);
    appStringsReset(strings);
}

void startServices(Win32App* app) {
    app->lifecycle = computedLifecycle(app);
    AddClipboardFormatListener(app->messageWindow);
    WTSRegisterSessionNotification(app->messageWindow, NOTIFY_FOR_THIS_SESSION);
    app->lowMemory = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    if (app->lowMemory != nullptr)
        SetTimer(app->messageWindow, kTimerLowMemory, kLowMemoryMilliseconds, nullptr);
}

void stopServices(Win32App* app) {
    for (uint32_t i = 0; i < app->timers.count; ++i)
        KillTimer(app->messageWindow, app->timers.data[i].id);
    app->timers.reset();
    KillTimer(app->messageWindow, kTimerLowMemory);
    if (app->lowMemory != nullptr)
        CloseHandle(app->lowMemory);
    app->lowMemory = nullptr;
    RemoveClipboardFormatListener(app->messageWindow);
    WTSUnRegisterSessionNotification(app->messageWindow);
    if (app->taskbar != nullptr)
        app->taskbar->Release();
    app->taskbar = nullptr;
    if (app->badge != nullptr)
        DestroyIcon(app->badge);
    app->badge = nullptr;
    SetThreadExecutionState(ES_CONTINUOUS);
}

// Polls the memory's state, and delivers LowMemory as it turns low.
void pollMemory(Win32App* app) {
    BOOL low = FALSE;
    if (app->lowMemory == nullptr || !QueryMemoryResourceNotification(app->lowMemory, &low))
        return;
    bool turned = low && !app->memoryLow;
    app->memoryLow = low;
    if (turned)
        win32DeliverKind(app, PrAppEventKind_LowMemory);
}

Win32Timer* PR_NULLABLE timerOf(Win32App* app, uint64_t tag, uint32_t* index) {
    for (uint32_t i = 0; i < app->timers.count; ++i) {
        if (app->timers.data[i].tag == tag) {
            *index = i;
            return &app->timers.data[i];
        }
    }
    return nullptr;
}

UINT millisecondsOf(double seconds) {
    double milliseconds = seconds * 1000.0;
    if (milliseconds < USER_TIMER_MINIMUM)
        return USER_TIMER_MINIMUM;
    return milliseconds > double(USER_TIMER_MAXIMUM) ? USER_TIMER_MAXIMUM : UINT(milliseconds);
}

void timerCancel(PrApp* base, uint64_t tag) {
    Win32App* app = static_cast<Win32App*>(base);
    uint32_t index = 0;
    if (Win32Timer* timer = timerOf(app, tag, &index)) {
        KillTimer(app->messageWindow, timer->id);
        app->timers.erase(index);
    }
}

void timerSet(PrApp* base, uint64_t tag, double delay, double interval) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running)
        return;
    timerCancel(base, tag);
    UINT_PTR id = kFirstProgramTimer + ++app->lastTimerId;
    app->timers.push(Win32Timer{.tag = tag, .id = id, .interval = interval > 0 ? interval : 0});
    SetTimer(app->messageWindow, id, millisecondsOf(delay), nullptr);
}

// Delivers the firing of the program's timer `id`. Its first firing sets it to repeat at its interval, before the
// delivery, so that the program may set or cancel the tag during the event.
void timerFired(Win32App* app, UINT_PTR id) {
    for (uint32_t i = 0; i < app->timers.count; ++i) {
        Win32Timer& timer = app->timers.data[i];
        if (timer.id != id)
            continue;
        uint64_t tag = timer.tag;
        if (timer.interval == 0) {
            KillTimer(app->messageWindow, id);
            app->timers.erase(i);
        } else if (!timer.repeating) {
            timer.repeating = true;
            SetTimer(app->messageWindow, id, millisecondsOf(timer.interval), nullptr);
        }
        PrAppEvent event{};
        event.kind = PrAppEventKind_Timer;
        event.time = win32SecondsNow();
        event.tag = tag;
        win32Deliver(app, nullptr, event);
        return;
    }
    KillTimer(app->messageWindow, id);
}

// Asks the program whether the session may end, and answers at once: the session ends unless the program cancels the
// quit. A request that arrives while a callback runs waits for it, and the session waits for nothing, so the
// application refuses the end, and stops after the callback if the program lets it.
LRESULT queryEndSession(Win32App* app) {
    if (!app->running)
        return TRUE;
    bool held = app->inCallback;
    PrAppEvent event{};
    event.kind = PrAppEventKind_QuitRequested;
    event.time = win32SecondsNow();
    event.quit.sessionEnding = true;
    win32Deliver(app, nullptr, event);
    return !held && app->quitAccepted ? TRUE : FALSE;
}

void quitCancel(PrApp* base) {
    Win32App* app = static_cast<Win32App*>(base);
    if (app->quitDelivering)
        app->quitCancelled = true;
}

// Handles the system's notices that reach the message window. Returns true and writes the result for a message that it
// handled.
bool serviceMessage(Win32App* app, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result) {
    *result = 0;
    switch (message) {
        case WM_TIMER:
            if (wParam == kTimerLowMemory)
                pollMemory(app);
            else if (wParam >= kFirstProgramTimer)
                timerFired(app, wParam);
            else
                return false;
            return true;
        case WM_CLIPBOARDUPDATE:
            win32DeliverKind(app, PrAppEventKind_ClipboardChanged);
            return true;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMPOWERSTATUSCHANGE)
                win32DeliverKind(app, PrAppEventKind_PowerChanged);
            else if (wParam == PBT_APMSUSPEND)
                win32DeliverKind(app, PrAppEventKind_Suspend);
            else if (wParam == PBT_APMRESUMEAUTOMATIC)
                win32DeliverKind(app, PrAppEventKind_Resume);
            *result = TRUE;
            return true;
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK)
                win32DeliverKind(app, PrAppEventKind_ScreenLocked);
            else if (wParam == WTS_SESSION_UNLOCK)
                win32DeliverKind(app, PrAppEventKind_ScreenUnlocked);
            return true;
        case WM_SETTINGCHANGE:
            if (lParam != 0 && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"intl") == 0)
                win32DeliverKind(app, PrAppEventKind_LocaleChanged);
            return false;
        case WM_QUERYENDSESSION:
            *result = queryEndSession(app);
            return true;
        case WM_COPYDATA: {
            const COPYDATASTRUCT* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (data == nullptr || data->dwData != kCopyDataOpen || data->cbData < sizeof(wchar_t))
                return false;
            RawArray<wchar_t> line;
            line.append(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
            line.push(0);
            openCommandLine(app, line.data);
            line.reset();
            *result = TRUE;
            return true;
        }
        default:
            return false;
    }
}

// Without a taskbar button, the application's windows are owned by its message window, which keeps them off the taskbar
// and the window switcher. The taskbar decides a window's button as the window shows, so a window that shows already
// gains or loses its button through the taskbar itself.
void setPresence(PrApp* base, PrAppPresence presence) {
    Win32App* app = static_cast<Win32App*>(base);
    app->presence = presence;
    bool accessory = presence == PrAppPresence_Accessory;
    ITaskbarList3* taskbar = win32TaskbarOf(app);
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* window = app->windows.data[i];
        if (window->role != PrAppWindowRole_Normal)
            continue;
        HWND owner = accessory ? app->messageWindow : nullptr;
        SetWindowLongPtrW(window->window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
        if (taskbar == nullptr || !IsWindowVisible(window->window))
            continue;
        if (accessory) {
            taskbar->DeleteTab(window->window);
        } else {
            taskbar->AddTab(window->window);
            win32ApplyTaskbar(window);
        }
    }
}

PrAppWindowModel windowModel(const PrApp* base) {
    return static_cast<const Win32App*>(base)->windowModel;
}

PrAppLifecycle lifecycle(const PrApp* base) {
    const Win32App* app = static_cast<const Win32App*>(base);
    return app->running ? app->lifecycle : computedLifecycle(app);
}

void destroy(PrApp* base) {
    Win32App* app = static_cast<Win32App*>(base);
    win32KeyLabelsReset(app);
    appDisplayListReset(app->displays);
    app->badgeText.reset();
    app->instanceName.reset();
    if (app->instanceMutex != nullptr)
        CloseHandle(app->instanceMutex);
    appDataRequestsReset(app->requests);
    appDataSourcesReset(app->sources);
    appEventQueueReset(app->held, &app->sources);
    app->windows.reset();
    app->utf8.reset();
    app->utf16.reset();
    prMemoryFree(app);
}

void run(PrApp* base, PrAppHandler handler) {
    Win32App* app = static_cast<Win32App*>(base);
    app->handler = handler;
    // The shell, which opens URLs, and the input methods expect a single-threaded apartment on the windows' thread, and
    // drags need OLE in it. A thread that already joined another apartment keeps it, and has no drags.
    app->ole = SUCCEEDED(OleInitialize(nullptr));
    win32DragsStart(app);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    registerClasses(instance);
    app->taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // A second launch of a single instance hands its command line to the first, and its run ends without a callback.
    if (!claimSingleInstance(app)) {
        win32DragsStop(app);
        if (app->ole)
            OleUninitialize();
        app->ole = false;
        return;
    }
    app->taskbarButtonCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
    // The message window is a hidden top-level window rather than a message-only one, because only top-level windows
    // receive the system's broadcasts: the session's end, power, and the settings.
    const wchar_t* name = app->instanceName.count != 0 ? app->instanceName.data : L"";
    app->messageWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kMessageClassName, name, WS_POPUP, 0, 0, 0, 0, nullptr,
        nullptr, instance, app);
    if (app->messageWindow == nullptr)
        PR_FATAL("the application's message window could not be created");
    startServices(app);
    app->pacerRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    app->pacerQuit.store(false);
    prThreadStart(&app->pacer, win32Pace, app);
    app->wakePending.store(false);
    app->wakeWindow.store(app->messageWindow);
    app->running = true;
    app->inCallback = true;
    if (!handler.start(handler.context, app))
        app->stopFn(app);
    win32EndCallback(app);
    // A frame runs when nothing else waits in the queue, as WM_PAINT does, since a posted message comes ahead of input:
    // a frame that takes longer than a refresh would otherwise find the next frame's message waiting each time it
    // returned, and the windows would take no input. The pacer's message only marks the frames due here. A modal loop,
    // such as a move's or a resize's, dispatches it to the message window, which runs the frames at once.
    while (app->running) {
        MSG message;
        if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (app->frameDue && win32AnyFrameRequested(app))
                win32RunRequestedFrames(app);
            else
                WaitMessage();
            continue;
        }
        if (message.message == WM_QUIT) {
            app->stopFn(app);
            break;
        }
        if (message.message == kMessageFrame && message.hwnd == app->messageWindow) {
            app->frameDue = true;
            continue;
        }
        if (win32TranslateAccelerator(app, message))
            continue;
        if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN)
            app->keyText = win32TranslateKey(message.message, message.wParam, message.lParam);
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    app->inCallback = true;
    handler.shutdown(handler.context, app);
    win32EndCallback(app);
    stopServices(app);
    while (app->statusItems.count != 0)
        win32StatusItemDestroy(app->statusItems.data[app->statusItems.count - 1]);
    app->statusItems.reset();
    win32MenuCopyReset(app->menuBar);
    app->menuBar = nullptr;
    // The windows that the program left open close with the run.
    while (app->windows.count != 0)
        prAppWindowDestroy(app->windows.data[app->windows.count - 1]);
    app->wakeWindow.store(nullptr);
    app->pacerQuit.store(true);
    SetEvent(app->pacerRequest);
    prThreadJoin(&app->pacer);
    CloseHandle(app->pacerRequest);
    app->pacerRequest = nullptr;
    if (app->notifyIcon) {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = app->messageWindow;
        icon.uID = kNotifyIconId;
        Shell_NotifyIconW(NIM_DELETE, &icon);
        app->notifyIcon = false;
    }
    for (uint32_t i = 0; i < app->hotkeys.count; ++i)
        UnregisterHotKey(app->messageWindow, app->hotkeys.data[i].id);
    app->hotkeys.reset();
    SetWindowLongPtrW(app->messageWindow, GWLP_USERDATA, 0);
    DestroyWindow(app->messageWindow);
    app->messageWindow = nullptr;
    app->frameDue = false;
    win32DragsStop(app);
    if (app->ole)
        OleUninitialize();
    app->ole = false;
}

void stop(PrApp* base) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running)
        return;
    app->running = false;
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* window = app->windows.data[i];
        KillTimer(window->window, kTimerFrame);
        KillTimer(window->window, kTimerFrameAt);
        // The system's modal loop for a move or a resize ends when it is canceled, and the main loop then sees the
        // stop.
        if (window->inSizeMove)
            PostMessageW(window->window, WM_CANCELMODE, 0, 0);
    }
    PostMessageW(app->messageWindow, WM_NULL, 0, 0);
}

void wake(PrApp* base) {
    Win32App* app = static_cast<Win32App*>(base);
    if (app->wakePending.exchange(true))
        return;
    HWND window = app->wakeWindow.load();
    if (window == nullptr || !PostMessageW(window, kMessageWake, 0, 0))
        app->wakePending.store(false);
}

bool isHidden(const PrApp* base) {
    return static_cast<const Win32App*>(base)->hidden;
}

} // namespace

Win32App* appOf(const Win32Window* window) {
    return window->app;
}

double win32SecondsNow() {
    return prTimeMonotonicMilliseconds() / 1e3;
}

void win32EndCallback(Win32App* app) {
    app->inCallback = false;
    uint32_t at = 0;
    PrAppWindow* window = nullptr;
    PrAppEvent event{};
    AppAnswer* answer = nullptr;
    while (app->running && appEventQueueNext(app->held, &at, &window, &event, &answer)) {
        if (answer != nullptr)
            callAnswer(app, *answer);
        else
            callEvent(app, static_cast<Win32Window*>(window), event);
    }
    appEventQueueClear(app->held, &app->sources);
}

void win32Deliver(Win32App* app, Win32Window* PR_NULLABLE window, const PrAppEvent& event) {
    if (app->running && app->inCallback) {
        appEventQueueHold(app->held, window, event);
        return;
    }
    if (app->running)
        callEvent(app, window, event);
    appDataSourceRelease(app->sources, prAppEventSource(&event));
    if (app->running)
        win32EndCallback(app);
}

void win32AnswerDeliver(Win32App* app, AppAnswer* answer) {
    if (!app->running) {
        appAnswerDestroy(answer, &app->sources);
        return;
    }
    if (app->inCallback) {
        appEventQueueHoldAnswer(app->held, answer);
        return;
    }
    callAnswer(app, *answer);
    appAnswerDestroy(answer, &app->sources);
    win32EndCallback(app);
}

void win32Deliver(Win32Window* window, const PrAppEvent& event) {
    win32Deliver(appOf(window), window, event);
}

void win32ForgetWindow(Win32App* app, Win32Window* window) {
    appEventQueueForget(app->held, window);
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        if (app->windows.data[i] == window) {
            app->windows.erase(i);
            break;
        }
    }
    if (app->lastActive == window)
        app->lastActive = nullptr;
}

void win32DeliverKind(Win32App* app, PrAppEventKind kind) {
    if (!app->running)
        return;
    PrAppEvent event{};
    event.kind = kind;
    event.time = win32SecondsNow();
    win32Deliver(app, nullptr, event);
}

void win32LifecycleMayChange(Win32App* app) {
    PrAppLifecycle now = computedLifecycle(app);
    if (now == app->lifecycle)
        return;
    app->lifecycle = now;
    win32DeliverKind(app, PrAppEventKind_LifecycleChanged);
}

void win32SetHidden(PrApp* base, bool hidden) {
    Win32App* app = static_cast<Win32App*>(base);
    if (app->headless || app->hidden == hidden)
        return;
    app->hidden = hidden;
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* window = app->windows.data[i];
        if (hidden && window->shown && IsWindowVisible(window->window)) {
            window->hiddenByApp = true;
            win32SetLock(window, false);
            ShowWindow(window->window, SW_HIDE);
        } else if (!hidden && window->hiddenByApp) {
            window->hiddenByApp = false;
            ShowWindow(window->window, window->overlay ? SW_SHOWNOACTIVATE : SW_SHOW);
            win32RequestFrame(window);
        }
    }
}

} // namespace pr_impl

using namespace pr_impl;

PrApp* prAppWin32Create(const PrAppOptions* options) {
    // The call fails if the process already chose an awareness, which then stands.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Win32App* app = allocZeroed<Win32App>();
    app->destroyFn = destroy;
    app->runFn = run;
    app->stopFn = stop;
    app->wakeFn = wake;
    app->notificationPostFn = win32NotificationPost;
    app->openUrlFn = win32OpenUrl;
    app->cancelFn = win32DataCancel;
    app->dataRetainFn = win32DataRetain;
    app->dataReleaseFn = win32DataRelease;
    app->dataReadFn = win32DataRead;
    app->dataWriteFn = win32DataWrite;
    app->dataWriteFileFn = win32DataWriteFile;
    app->windowCreateFn = win32WindowCreate;
    app->displaysFn = win32Displays;
    app->displayInfoFn = win32DisplayInfo;
    app->pointerPositionFn = win32PointerPosition;
    app->setHiddenFn = win32SetHidden;
    app->isHiddenFn = isHidden;
    app->revealPathFn = win32RevealPath;
    app->hotkeyRegisterFn = win32HotkeyRegister;
    app->hotkeyUnregisterFn = win32HotkeyUnregister;
    app->fileDialogFn = win32FileDialog;
    app->quitCancelFn = quitCancel;
    app->setPresenceFn = setPresence;
    app->windowModelFn = windowModel;
    app->lifecycleFn = lifecycle;
    app->timerSetFn = timerSet;
    app->timerCancelFn = timerCancel;
    app->keyCodeLabelFn = win32KeyCodeLabel;
    app->trashPathFn = win32TrashPath;
    app->recentDocumentAddFn = win32RecentDocumentAdd;
    app->recentDocumentsClearFn = win32RecentDocumentsClear;
    app->setBadgeFn = win32SetBadge;
    app->powerFn = win32Power;
    app->setAwakeFn = win32SetAwake;
    app->setMenuBarFn = win32SetMenuBar;
    app->statusItemCreateFn = win32StatusItemCreate;
    appDataSourcesInit(app->sources);
    appDataRequestsInit(app->requests, app->sources);
    app->headless = options->headless;
    app->presence = options->presence;
    app->windowModel = options->windowModel;
    if (options->singleInstance.size != 0)
        windowsWiden(options->singleInstance, app->instanceName);
    return app;
}

"""The windows of macOS applications: which processes' windows are on screen, how much of them other windows cover, and
how to bring a process's windows to the front.

None of it needs a permission that the user grants. Activation goes through NSRunningApplication, which any process may
ask of another. If the application ignores that, activation goes through the window server's front process. The window
server's list of windows (CGWindowListCopyWindowInfo) gives every process the owners, levels and bounds of the windows.
Only their titles need the Screen Recording permission.
"""

import ctypes
import sys
import threading
import time


class ProcessSerialNumber(ctypes.Structure):
    _fields_ = [("high", ctypes.c_uint32), ("low", ctypes.c_uint32)]


class Rect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double), ("width", ctypes.c_double),
                ("height", ctypes.c_double)]


if sys.platform == "darwin":
    OBJC = ctypes.CDLL("/usr/lib/libobjc.A.dylib")
    ctypes.CDLL("/System/Library/Frameworks/AppKit.framework/AppKit")
    CG = ctypes.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
    CF = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
    HISERVICES = ctypes.CDLL("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices")
    SKYLIGHT = ctypes.CDLL("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight")
    HISERVICES.GetProcessForPID.restype = ctypes.c_int32
    HISERVICES.GetProcessForPID.argtypes = [ctypes.c_int, ctypes.POINTER(ProcessSerialNumber)]
    SKYLIGHT._SLPSSetFrontProcessWithOptions.restype = ctypes.c_int32
    SKYLIGHT._SLPSSetFrontProcessWithOptions.argtypes = [ctypes.POINTER(ProcessSerialNumber), ctypes.c_uint32,
                                                         ctypes.c_uint32]
    OBJC.objc_getClass.restype = ctypes.c_void_p
    OBJC.objc_getClass.argtypes = [ctypes.c_char_p]
    OBJC.sel_registerName.restype = ctypes.c_void_p
    OBJC.sel_registerName.argtypes = [ctypes.c_char_p]
    OBJC.objc_autoreleasePoolPush.restype = ctypes.c_void_p
    OBJC.objc_autoreleasePoolPop.argtypes = [ctypes.c_void_p]
    CF.CFArrayGetCount.restype = ctypes.c_long
    CF.CFArrayGetCount.argtypes = [ctypes.c_void_p]
    CF.CFArrayGetValueAtIndex.restype = ctypes.c_void_p
    CF.CFArrayGetValueAtIndex.argtypes = [ctypes.c_void_p, ctypes.c_long]
    CF.CFDictionaryGetValue.restype = ctypes.c_void_p
    CF.CFDictionaryGetValue.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    CF.CFStringCreateWithCString.restype = ctypes.c_void_p
    CF.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
    CF.CFNumberGetValue.restype = ctypes.c_bool
    CF.CFNumberGetValue.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
    CF.CFRelease.argtypes = [ctypes.c_void_p]
    CG.CGWindowListCopyWindowInfo.restype = ctypes.c_void_p
    CG.CGWindowListCopyWindowInfo.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    CG.CGRectMakeWithDictionaryRepresentation.restype = ctypes.c_bool
    CG.CGRectMakeWithDictionaryRepresentation.argtypes = [ctypes.c_void_p, ctypes.POINTER(Rect)]

# CGWindowListOption: the windows on screen, without the desktop's.
ON_SCREEN_ONLY = 1 << 0
EXCLUDE_DESKTOP_ELEMENTS = 1 << 4

# CFNumberType and CFStringEncoding.
NUMBER_SINT64 = 4
NUMBER_DOUBLE = 13
UTF8 = 0x08000100

# NSApplicationActivationOptions: every window of the application, ahead of the active application's.
ACTIVATE_ALL_WINDOWS = 1 << 0
ACTIVATE_IGNORING_OTHER_APPS = 1 << 1

# The mode of _SLPSSetFrontProcessWithOptions that brings a process to the front with its windows in their current
# order, without naming a window (kCPSNoWindows, as yabai names it).
FRONT_PROCESS_MODE = 0x400

# The level of the Dock's windows. Windows below this level belong to applications: normal, floating, modal and utility
# windows. The Dock, the menu bar and the window server's overlays are at this level or above. They span the whole
# screen and are mostly transparent, so they do not count as covering a window.
DOCK_LEVEL = 20

# A window smaller than this in either dimension, in points, such as an application's status item, is not a content
# window.
MIN_EXTENT = 64


class Window:
    """A window on screen: its owner's process, its level, its bounds in points (x, y, width, height), and its alpha."""

    def __init__(self, pid, level, bounds, alpha):
        self.pid = pid
        self.level = level
        self.bounds = bounds
        self.alpha = alpha

    def area(self):
        return self.bounds[2] * self.bounds[3]


def send(restype, receiver, selector, *arguments):
    """Sends an Objective-C message: `arguments` are pairs of a ctypes type and a value."""
    function = ctypes.CFUNCTYPE(restype, ctypes.c_void_p, ctypes.c_void_p, *[kind for kind, _ in arguments])
    return ctypes.cast(OBJC.objc_msgSend, function)(receiver, OBJC.sel_registerName(selector.encode()),
                                                    *[value for _, value in arguments])


KEYS = {}


def key(name):
    """Returns the CFString of a key of the window list's dictionaries, which lives as long as the process."""
    if name not in KEYS:
        KEYS[name] = CF.CFStringCreateWithCString(None, name.encode(), UTF8)
    return KEYS[name]


def number(dictionary, name, kind=ctypes.c_int64, code=NUMBER_SINT64):
    value = CF.CFDictionaryGetValue(dictionary, key(name))
    out = kind()
    if value:
        CF.CFNumberGetValue(value, code, ctypes.byref(out))
    return out.value


def windows():
    """Returns the windows on screen, front to back."""
    listing = CG.CGWindowListCopyWindowInfo(ON_SCREEN_ONLY | EXCLUDE_DESKTOP_ELEMENTS, 0)
    if not listing:
        return []
    out = []
    try:
        for i in range(CF.CFArrayGetCount(listing)):
            entry = CF.CFArrayGetValueAtIndex(listing, i)
            rect = Rect()
            CG.CGRectMakeWithDictionaryRepresentation(CF.CFDictionaryGetValue(entry, key("kCGWindowBounds")),
                                                      ctypes.byref(rect))
            out.append(Window(number(entry, "kCGWindowOwnerPID"), number(entry, "kCGWindowLayer"),
                              (rect.x, rect.y, rect.width, rect.height),
                              number(entry, "kCGWindowAlpha", ctypes.c_double, NUMBER_DOUBLE)))
    finally:
        CF.CFRelease(listing)
    return out


def union_area(rects):
    """Returns the area of the union of rectangles (x0, y0, x1, y1)."""
    xs = sorted({x for rect in rects for x in (rect[0], rect[2])})
    area = 0.0
    for left, right in zip(xs, xs[1:]):
        spans = sorted((rect[1], rect[3]) for rect in rects if rect[0] <= left and rect[2] >= right)
        covered, top, bottom = 0.0, None, None
        for y0, y1 in spans:
            if bottom is None or y0 > bottom:
                covered += 0 if bottom is None else bottom - top
                top, bottom = y0, y1
            else:
                bottom = max(bottom, y1)
        covered += 0 if bottom is None else bottom - top
        area += covered * (right - left)
    return area


def covered_share(listing, pids):
    """Returns the share of the largest content window of `pids` in `listing` that other processes' windows in front of
    it cover, or None if `pids` have no content window on screen."""
    content = [(i, window) for i, window in enumerate(listing) if window.pid in pids and window.level < DOCK_LEVEL
               and window.alpha > 0 and min(window.bounds[2:]) >= MIN_EXTENT]
    if not content:
        return None
    index, mine = max(content, key=lambda item: item[1].area())
    x, y, width, height = mine.bounds
    clipped = []
    for window in listing[:index]:
        if window.pid in pids or window.level >= DOCK_LEVEL or window.alpha <= 0:
            continue
        wx, wy, ww, wh = window.bounds
        x0, y0, x1, y1 = max(x, wx), max(y, wy), min(x + width, wx + ww), min(y + height, wy + wh)
        if x0 < x1 and y0 < y1:
            clipped.append((x0, y0, x1, y1))
    return union_area(clipped) / mine.area()


def covered(pids):
    """Returns the share of the largest content window of `pids` that other processes' windows cover, or None if `pids`
    have no content window on screen."""
    return covered_share(windows(), pids)


class Watch:
    """Samples the share of the largest window of `pids` that other windows cover, every `interval` seconds from start()
    to stop(), on a thread of its own. stop() returns the largest share sampled, or None if the window was off screen at
    any sample."""

    def __init__(self, pids, interval=0.1):
        self.pids = set(pids)
        self.interval = interval
        self.most = 0.0
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.sample, daemon=True)

    def sample(self):
        while True:
            share = covered(self.pids)
            self.most = None if share is None or self.most is None else max(self.most, share)
            if self.stopped.wait(self.interval):
                return

    def start(self):
        self.thread.start()
        return self

    def stop(self):
        self.stopped.set()
        self.thread.join()
        return self.most


def activate(pid):
    """Activates the application of process `pid`, bringing all of its windows to the front. Returns false if the
    process is not an application, or the system refused."""
    pool = OBJC.objc_autoreleasePoolPush()
    try:
        application = send(ctypes.c_void_p, OBJC.objc_getClass(b"NSRunningApplication"),
                           "runningApplicationWithProcessIdentifier:", (ctypes.c_int, pid))
        if not application:
            return False
        return send(ctypes.c_bool, application, "activateWithOptions:",
                    (ctypes.c_ulong, ACTIVATE_ALL_WINDOWS | ACTIVATE_IGNORING_OTHER_APPS))
    finally:
        OBJC.objc_autoreleasePoolPop(pool)


def set_front_process(pid):
    """Makes process `pid` the window server's front process, with its windows. Returns false if the process has no
    connection to the window server, or the window server refused.

    The window server's call is private (SkyLight's _SLPSSetFrontProcessWithOptions, which window managers such as
    yabai use). It is the fallback for an application that ignores a request to activate. Since macOS 14, activation
    is cooperative, and the system may decline a request from a process that is not the active application. It does so
    for an executable outside a bundle."""
    process = ProcessSerialNumber()
    if HISERVICES.GetProcessForPID(pid, ctypes.byref(process)) != 0:
        return False
    return SKYLIGHT._SLPSSetFrontProcessWithOptions(ctypes.byref(process), 0, FRONT_PROCESS_MODE) == 0


def bring_to_front(pids, timeout):
    """Activates the processes among `pids` that have windows on screen, or all of `pids` if none has, until their
    largest window is uncovered or `timeout` seconds pass. Every other attempt asks the window server instead. Returns
    the share of that window that remains covered, or None if `pids` have no content window on screen."""
    deadline = time.monotonic() + timeout
    attempt = 0
    while True:
        listing = windows()
        share = covered_share(listing, pids)
        if share == 0 or time.monotonic() >= deadline:
            return share
        owners = {window.pid for window in listing if window.pid in pids and window.level < DOCK_LEVEL} or pids
        for pid in sorted(owners):
            if attempt % 2 == 0:
                activate(pid)
            else:
                set_front_process(pid)
        attempt += 1
        time.sleep(0.1)

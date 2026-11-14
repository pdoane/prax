// The Win32 backend's displays: their list, their properties, and positions on them.

#pragma once

#include "app/backends/win32/src/app_win32.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The DXGI output that shows `monitor`, or null. The caller releases it.
IDXGIOutput6* PR_NULLABLE win32OutputOf(IDXGIFactory1* factory, HMONITOR monitor);

// Returns the value `name` of the user's personalization settings, or `fallback` if it is not set.
DWORD win32PersonalizeValue(const wchar_t* name, DWORD fallback);

// Returns the headroom and gamut of the display that `output` shows, on `monitor`. A display shows more than SDR only
// while Windows composes it in HDR, and then its headroom is its peak luminance over SDR white. In SDR, Windows
// composes in sRGB.
PrAppDisplay win32DisplayOfOutput(IDXGIOutput6* PR_NULLABLE output, HMONITOR monitor);

// Writes the monitor `handle` to `out` and returns true, or returns false if the system no longer knows it.
bool win32MonitorOf(HMONITOR handle, Win32Monitor* out);

// Finds the monitor with `id`, or returns false if none is connected.
bool win32MonitorWithId(PrAppDisplayId id, Win32Monitor* out);

// Writes the primary monitor to `out` and returns true, or returns false if the system reports none.
bool win32PrimaryMonitor(Win32Monitor* out);

// Returns a rectangle of `monitor`, in physical pixels on the virtual screen, in the monitor's logical pixels from its
// top left.
PrRect win32LogicalOn(const Win32Monitor& monitor, RECT rect);

// Posts one DisplaysChanged event for the messages that each window receives about a change.
void win32NoteDisplaysChanged(Win32App* app);

// Implements prAppDisplays.
uint32_t win32Displays(const PrApp* base, PrAppDisplayInfo* PR_NULLABLE out, uint32_t capacity);

// Implements prAppDisplayInfoFromId.
bool win32DisplayInfo(const PrApp* base, PrAppDisplayId id, PrAppDisplayInfo* out);

// Implements prAppPointerPosition.
bool win32PointerPosition(const PrApp* base, PrAppDisplayPosition* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END

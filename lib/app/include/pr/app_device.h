// The device: its power, whether it stays awake, its motion sensors and its haptics.

#pragma once

#include "pr/app_application.h"
#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The source of the device's power.
enum {
    PrAppPowerSource_Unknown,
    PrAppPowerSource_External,      // a power supply, whether or not the device has a battery
    PrAppPowerSource_Battery,
};
typedef uint8_t PrAppPowerSource;

// Returns the name of a power source, as in "Battery", or the null view if `source` is not one.
PrStringView prAppPowerSourceName(PrAppPowerSource source);

// The state of the device's power. `level` is the battery's charge in [0, 1], or -1 if the device has no battery.
// `lowPower` is true while the user or the system is saving power, as in Low Power Mode. The program should then do
// less work in the background.
typedef struct PrAppPower {
    PrAppPowerSource source;
    float level;
    bool charging;
    bool lowPower;
} PrAppPower;

// Returns the state of the device's power. A PowerChanged event follows each change.
PrAppPower prAppPower(const PrApp* app);

// What the application keeps awake. Display keeps the display from dimming or sleeping. System keeps the system from
// sleeping while it is idle.
enum {
    PrAppAwakeFlag_Display = 1 << 0,
    PrAppAwakeFlag_System = 1 << 1,
};
typedef uint8_t PrAppAwakeFlags;

// Keeps awake what `flags` lists until the next call, for example during video playback or a long task.
void prAppSetAwake(PrApp* app, PrAppAwakeFlags flags);

// The motion sensors (prAppSensorEnable). The axes are the device's (PrAppSensorEvent).
enum {
    PrAppSensor_Accelerometer,  // m/s², including gravity
    PrAppSensor_Gyroscope,      // rad/s
    PrAppSensor_Magnetometer,   // µT
    PrAppSensor_Attitude,       // the device's orientation in space, as a unit quaternion (x, y, z, w)
    PrAppSensor_Count,
};
typedef uint8_t PrAppSensor;

// Returns the name of a sensor, as in "Gyroscope", or the null view if `sensor` is not one.
PrStringView prAppSensorName(PrAppSensor sensor);

// Starts or stops readings from `sensor`. Readings arrive as Sensor events, roughly every `interval` seconds. Returns
// NotSupported if the device does not have the sensor.
PrStatus prAppSensorEnable(PrApp* app, PrAppSensor sensor, bool enable, double interval);

// The kinds of haptic feedback that the system's own controls use.
enum {
    PrAppHaptic_Selection,      // a selection changed, such as the value of a picker

    // An impact, at three strengths, such as when a control snaps into place.
    PrAppHaptic_Light,
    PrAppHaptic_Medium,
    PrAppHaptic_Heavy,

    // The outcome of a task.
    PrAppHaptic_Success,
    PrAppHaptic_Warning,
    PrAppHaptic_Error,
};
typedef uint8_t PrAppHaptic;

// Returns the name of a haptic, as in "Success", or the null view if `haptic` is not one.
PrStringView prAppHapticName(PrAppHaptic haptic);

// Plays `haptic` on the device, or on its trackpad if it has one. On a device without haptics, this does nothing.
void prAppHapticPlay(PrApp* app, PrAppHaptic haptic);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

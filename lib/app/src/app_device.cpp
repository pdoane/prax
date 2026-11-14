// The device: the contract around each backend's functions.

#include "pr/app_device.h"

#include "pr/app_backend.h"
#include "pr/assert.h"
#include "pr/count_of.h"

#include "app/src/app_names.h"

namespace pr_impl {

namespace {

// The names of the enumerations, in the order of their enumerators.
const char* const kPowerSources[] = {"Unknown", "External", "Battery"};
static_assert(countOf(kPowerSources) == PrAppPowerSource_Battery + 1, "a name for each power source");

const char* const kSensors[] = {"Accelerometer", "Gyroscope", "Magnetometer", "Attitude"};
static_assert(countOf(kSensors) == PrAppSensor_Count, "a name for each sensor");

const char* const kHaptics[] = {"Selection", "Light", "Medium", "Heavy", "Success", "Warning", "Error"};
static_assert(countOf(kHaptics) == PrAppHaptic_Error + 1, "a name for each haptic");

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStringView prAppPowerSourceName(PrAppPowerSource source) {
    return appNameOf(kPowerSources, source);
}

PrAppPower prAppPower(const PrApp* app) {
    if (app->powerFn != nullptr)
        return app->powerFn(app);
    return PrAppPower{.source = PrAppPowerSource_Unknown, .level = -1};
}

void prAppSetAwake(PrApp* app, PrAppAwakeFlags flags) {
    PR_ASSERT((flags & ~(PrAppAwakeFlag_Display | PrAppAwakeFlag_System)) == 0);
    if (app->setAwakeFn != nullptr)
        app->setAwakeFn(app, flags);
}

PrStringView prAppSensorName(PrAppSensor sensor) {
    return appNameOf(kSensors, sensor);
}

PrStatus prAppSensorEnable(PrApp* app, PrAppSensor sensor, bool enable, double interval) {
    PR_ASSERT(sensor < PrAppSensor_Count && interval >= 0);
    return app->sensorEnableFn != nullptr ? app->sensorEnableFn(app, sensor, enable, interval) : PrStatus_NotSupported;
}

PrStringView prAppHapticName(PrAppHaptic haptic) {
    return appNameOf(kHaptics, haptic);
}

void prAppHapticPlay(PrApp* app, PrAppHaptic haptic) {
    PR_ASSERT(haptic <= PrAppHaptic_Error);
    if (app->hapticPlayFn != nullptr)
        app->hapticPlayFn(app, haptic);
}

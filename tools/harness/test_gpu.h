// The GPU devices that the cases of a test binary share. Making a device costs more than most cases. A driver may also
// not survive making many devices at once, which test binaries that run side by side do.

#pragma once

#include "pr/gpu.h"

#include "tools/harness/harness_gpu.h"
#include "tools/harness/test_main.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// Returns the device of the platform's GPU that the cases of the binary share. The device has the default backend and
// options, or is one of slots alone if `slotsOnly` is set (PrGpuDeviceOptions::slotsOnly). The first call makes the
// device, and the end of the run destroys it (atTestRunEnd). Returns null if the platform cannot make the device. A
// case leaves the device as it found it, so it destroys what it creates on the device.
inline PrGpuDevice* PR_NULLABLE testGpuDevice(bool slotsOnly = false) {
    struct Shared {
        bool made = false;
        PrGpuDevice* PR_NULLABLE device = nullptr;
    };
    static Shared shared[2];
    Shared& tier = shared[slotsOnly ? 1 : 0];
    if (!tier.made) {
        tier.made = true;
        PrGpuDeviceOptions options = prGpuDeviceOptionsDefault();
        options.slotsOnly = slotsOnly;
        if (gpuDeviceCreate(PrGpuBackend_Default, &options, &tier.device) == PrStatus_Ok) {
            atTestRunEnd([](void* PR_NULLABLE context) {
                Shared& ended = *static_cast<Shared*>(context);
                prGpuDeviceDestroy(ended.device);
                ended.device = nullptr;
            }, &tier);
        }
    }
    return tier.device;
}

} // namespace harness

PR_ASSUME_NONNULL_END

// GPU devices and readbacks for the tests and the tools, which wait for each creation and each read.

#pragma once

#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// Creates a device by `backend` without a pool, writes it to `*out`, and returns Ok. If the creation fails, destroys
// the device, sets `*out` to null, and returns the reason (prGpuDeviceLostStatus), or NotSupported if the build does
// not have the backend.
inline PrStatus gpuDeviceCreate(PrGpuBackend backend, const PrGpuDeviceOptions* PR_NULLABLE options,
        PrGpuDevice* PR_NULLABLE* PR_NONNULL out) {
    PrGpuDeviceOptions o = options != nullptr ? *options : prGpuDeviceOptionsDefault();
    o.pool = nullptr;
    *out = prGpuDeviceCreate(backend, &o);
    if (*out == nullptr)
        return PrStatus_NotSupported;
    PrStatus status = prGpuDeviceIsReady(*out) ? prGpuDeviceLostStatus(*out) : PrStatus_NotSupported;
    if (status != PrStatus_Ok) {
        prGpuDeviceDestroy(*out);
        *out = nullptr;
    }
    return status;
}

// Submits the command buffer and waits for its work. Returns the seconds that the device spent on it, which are zero
// without PrGpuFeatureFlag_Timestamps or if the device was lost first.
inline double gpuSubmitAndWait(PrGpuCommands* commands) {
    double seconds = 0;
    PrGpuDevice* device = prGpuCommandsDevice(commands);
    prGpuCommandsOnDone(commands, [](void* PR_NULLABLE context, double spent) {
        *static_cast<double*>(context) = spent;
    }, &seconds);
    prGpuDeviceWait(device, prGpuCommandsSubmit(commands));
    return seconds;
}

// Reads the texels of the first level of a 2D texture at `origin` into `out`, in the texture's format, once the work
// submitted before the call is done, and waits for the read. The size of `out` is the size read, and texels outside the
// texture are zero, as are the texels of a lost device. The texture is in `access` before and after the read.
inline void gpuTextureRead(PrGpuDevice* device, PrGpuTexture texture, uint32_t access, PrIVec2 origin, PrPixels out) {
    struct Read {
        PrIVec2 origin;
        PrPixels out;
        uint32_t bytes;
    };
    Read read{.origin = origin, .out = out, .bytes = prGpuFormatBlock(prGpuTextureDesc(device, texture).format).bytes};
    for (uint32_t y = 0; y < out.extent.y; ++y) {
        for (size_t i = 0; i < size_t(out.extent.x) * read.bytes; ++i)
            out.data[y * out.stride + i] = 0;
    }
    PrGpuCommands* commands = nullptr;
    if (!prGpuCommandsBegin(device, nullptr, &commands))
        return;
    prGpuCommandsReadTexture(commands, texture, access, [](void* PR_NULLABLE context, PrPixelView texels) {
        const Read& r = *static_cast<const Read*>(context);
        for (uint32_t y = 0; y < r.out.extent.y; ++y) {
            int64_t from = int64_t(r.origin.y) + y;
            if (from < 0 || from >= int64_t(texels.extent.y))
                continue;
            for (uint32_t x = 0; x < r.out.extent.x; ++x) {
                int64_t column = int64_t(r.origin.x) + x;
                if (column < 0 || column >= int64_t(texels.extent.x))
                    continue;
                const uint8_t* texel = texels.data + size_t(from) * texels.stride + size_t(column) * r.bytes;
                for (uint32_t i = 0; i < r.bytes; ++i)
                    r.out.data[y * r.out.stride + size_t(x) * r.bytes + i] = texel[i];
            }
        }
    }, &read);
    prGpuDeviceWait(device, prGpuCommandsSubmit(commands));
}

} // namespace harness

PR_ASSUME_NONNULL_END

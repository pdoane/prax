// The selector for lib/gpu's backends.

#include "pr/gpu_select.h"

#include "pr/alloc.h"
#include "pr/atomic.h"

#if defined(PR_GPU_HAS_METAL)
#include "pr/gpu_metal.h"
#endif
#if defined(PR_GPU_HAS_METAL4)
#include "pr/gpu_metal4.h"
#endif
#if defined(PR_GPU_HAS_VULKAN)
#include "pr/gpu_vulkan.h"
#endif
#if defined(PR_GPU_HAS_D3D12)
#include "pr/gpu_d3d12.h"
#endif
#if defined(PR_GPU_HAS_D3D11)
#include "pr/gpu_d3d11.h"
#endif
#if defined(PR_GPU_HAS_CANVAS2D)
#include "pr/gpu_canvas2d.h"
#endif
#if defined(PR_GPU_HAS_WEBGPU)
#include "pr/gpu_webgpu.h"
#endif
#if defined(PR_GPU_HAS_MEMORY)
#include "pr/gpu_memory.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrGpuBackend_Count] = {
    "default", "metal", "metal4", "vulkan", "d3d12", "d3d11", "webgpu", "memory", "canvas2d",
};

// The backends that the build includes, the platform's own first and memory last. Default ends the list, so that a
// build without backends creates no device.
constexpr PrGpuBackend kBuilt[] = {
#if defined(PR_GPU_HAS_METAL)
    PrGpuBackend_Metal,
#endif
#if defined(PR_GPU_HAS_METAL4)
    PrGpuBackend_Metal4,
#endif
#if defined(PR_GPU_HAS_D3D12)
    PrGpuBackend_D3d12,
#endif
#if defined(PR_GPU_HAS_VULKAN)
    PrGpuBackend_Vulkan,
#endif
#if defined(PR_GPU_HAS_D3D11)
    PrGpuBackend_D3d11,
#endif
#if defined(PR_GPU_HAS_WEBGPU)
    PrGpuBackend_Webgpu,
#endif
#if defined(PR_GPU_HAS_CANVAS2D)
    PrGpuBackend_Canvas2d,
#endif
#if defined(PR_GPU_HAS_MEMORY)
    PrGpuBackend_Memory,
#endif
    PrGpuBackend_Default,
};

constexpr uint64_t builtMask() {
    uint64_t mask = 0;
    for (PrGpuBackend backend : kBuilt)
        mask |= uint64_t(1) << backend;
    return mask & ~uint64_t(1);
}

constexpr PrBackendInterface kInterface{
    .name = "gpu",
    .names = kBackendNames,
    .count = PrGpuBackend_Count,
    .built = builtMask(),
};

// Creates a device of `backend`, which is not Default, with `o` in place of the backend's own options where they
// overlap. Returns null if the build does not include the backend, or if the backend creates no device with the
// options, as for slotsOnly on a backend whose devices all have bindless.
PrGpuDevice* PR_NULLABLE create(PrGpuBackend backend, const PrGpuDeviceOptions& o) {
    // A build without a backend has no use for the options.
    (void)o;
    switch (backend) {
#if defined(PR_GPU_HAS_METAL)
        case PrGpuBackend_Metal: {
            PrGpuMetalOptions metal = prGpuMetalOptionsDefault();
            metal.validation = o.validation;
            metal.slotsOnly = o.slotsOnly;
            metal.pool = o.pool;
            metal.ready = o.ready;
            metal.readyContext = o.readyContext;
            metal.limits = o.limits;
            return prGpuMetalCreate(&metal);
        }
#endif
#if defined(PR_GPU_HAS_METAL4)
        case PrGpuBackend_Metal4: {
            PrGpuMetal4Options metal = prGpuMetal4OptionsDefault();
            metal.validation = o.validation;
            metal.slotsOnly = o.slotsOnly;
            metal.pool = o.pool;
            metal.ready = o.ready;
            metal.readyContext = o.readyContext;
            metal.limits = o.limits;
            return prGpuMetal4Create(&metal);
        }
#endif
#if defined(PR_GPU_HAS_VULKAN)
        case PrGpuBackend_Vulkan: {
            PrGpuVulkanOptions vulkan = prGpuVulkanOptionsDefault();
            vulkan.validation = vulkan.validation || o.validation;
            vulkan.slotsOnly = o.slotsOnly;
            vulkan.pool = o.pool;
            vulkan.ready = o.ready;
            vulkan.readyContext = o.readyContext;
            vulkan.limits = o.limits;
            return prGpuVulkanCreate(&vulkan);
        }
#endif
#if defined(PR_GPU_HAS_D3D12)
        case PrGpuBackend_D3d12: {
            // Direct3D 12 always has bindless, and its slots are tested beside it.
            if (o.slotsOnly)
                return nullptr;
            PrGpuD3d12Options d3d12 = prGpuD3d12OptionsDefault();
            d3d12.validation = d3d12.validation || o.validation;
            d3d12.pool = o.pool;
            d3d12.ready = o.ready;
            d3d12.readyContext = o.readyContext;
            d3d12.limits = o.limits;
            return prGpuD3d12Create(&d3d12);
        }
#endif
#if defined(PR_GPU_HAS_D3D11)
        case PrGpuBackend_D3d11: {
            // Direct3D 11 has no bindless, so every device of it binds resources only to slots.
            PrGpuD3d11Options d3d11 = prGpuD3d11OptionsDefault();
            d3d11.validation = d3d11.validation || o.validation;
            d3d11.pool = o.pool;
            d3d11.ready = o.ready;
            d3d11.readyContext = o.readyContext;
            d3d11.limits = o.limits;
            return prGpuD3d11Create(&d3d11);
        }
#endif
#if defined(PR_GPU_HAS_WEBGPU)
        case PrGpuBackend_Webgpu:
            // WebGPU has no bindless, so every device of it binds resources only to slots.
            return prGpuWebgpuCreate(&o);
#endif
#if defined(PR_GPU_HAS_CANVAS2D)
        case PrGpuBackend_Canvas2d:
            return prGpuCanvas2dCreate(&o);
#endif
#if defined(PR_GPU_HAS_MEMORY)
        case PrGpuBackend_Memory:
            return prGpuMemoryCreate(&o);
#endif
        default:
            return nullptr;
    }
}

// The backends that Default passes over, bit k for PrGpuBackend k: those whose creation failed with NotSupported. A
// system that lacks a backend's API or adapter lacks it for the life of the process. Any thread may read and set it.
Atomic<uint32_t> unsupported;

// Returns the first backend of the build that Default does not pass over and that is not in `refused`. Returns Default
// if there is none.
PrGpuBackend firstSupported(uint32_t refused = 0) {
    uint32_t passed = unsupported.load() | refused;
    for (PrGpuBackend built : kBuilt) {
        if (built == PrGpuBackend_Default || (passed & (1u << built)) == 0)
            return built;
    }
    return PrGpuBackend_Default;
}

void passOver(PrGpuBackend backend) {
    uint32_t passed = unsupported.load();
    while (!unsupported.cas(passed, passed | (1u << backend))) {}
}

// How far a creation that Default started has come. The creating call and the ready callback each mark their arrival by
// exchanging the stage. Whichever arrives second sees what the first did, and frees the creation.
enum {
    DefaultStage_Creating,      // neither has arrived
    DefaultStage_Returned,      // the creating call returned the device, and the callback has yet to run
    DefaultStage_Done,          // the callback ran during the creating call, and called the creator's callback
    DefaultStage_Retry,         // the callback ran during the creating call, which tries another backend instead
};
typedef uint8_t DefaultStage;

// A creation that Default started. `refused` holds the backends that create no device with its options (createDefault).
struct DefaultCreation {
    PrGpuBackend backend;
    uint32_t refused;
    PrGpuDeviceReadyFn PR_NULLABLE ready;
    void* PR_NULLABLE readyContext;
    Atomic<DefaultStage> stage;
};

// Passes over the backend of a creation that failed with NotSupported. If the creation is done before the creating call
// returns and another backend remains, the creating call tries that backend in place of the device. Otherwise this
// calls the creator's ready callback.
void defaultReady(void* PR_NULLABLE context, PrGpuDevice* device) {
    DefaultCreation* c = static_cast<DefaultCreation*>(context);
    bool failed = prGpuDeviceLostStatus(device) == PrStatus_NotSupported;
    if (failed)
        passOver(c->backend);
    bool retry = failed && firstSupported(c->refused) != PrGpuBackend_Default;
    // The creating call may free the creation once the stage is exchanged.
    PrGpuDeviceReadyFn ready = c->ready;
    void* readyContext = c->readyContext;
    bool returned = c->stage.exchange(retry ? DefaultStage_Retry : DefaultStage_Done) == DefaultStage_Returned;
    if (returned)
        prMemoryFree(c);
    if ((returned || !retry) && ready != nullptr)
        ready(readyContext, device);
}

// Creates a device as Default does without a choice from the environment: by the first backend that is not passed over
// and creates a device with the options. If a creation's failure is known when it returns, this moves on to the next
// backend at once. Otherwise the next creation passes over the failed backend.
PrGpuDevice* PR_NULLABLE createDefault(const PrGpuDeviceOptions& o) {
    // The backends that create no device with these options. Another creation's options may differ.
    uint32_t refused = 0;
    for (;;) {
        PrGpuBackend backend = firstSupported(refused);
        if (backend == PrGpuBackend_Default)
            return nullptr;
        DefaultCreation* c = allocZeroed<DefaultCreation>();
        c->backend = backend;
        c->refused = refused;
        c->ready = o.ready;
        c->readyContext = o.readyContext;
        c->stage.store(DefaultStage_Creating);
        PrGpuDeviceOptions chained = o;
        chained.ready = defaultReady;
        chained.readyContext = c;
        PrGpuDevice* device = create(backend, chained);
        // A backend that creates no device with the options returns null at once, and calls no ready callback.
        if (device == nullptr) {
            prMemoryFree(c);
            refused |= 1u << backend;
            continue;
        }
        DefaultStage stage = c->stage.exchange(DefaultStage_Returned);
        // The callback has yet to run, and frees the creation when it does.
        if (stage == DefaultStage_Creating)
            return device;
        prMemoryFree(c);
        if (stage == DefaultStage_Done)
            return device;
        prGpuDeviceDestroy(device);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

const PrBackendInterface* prGpuSelectInterface(void) {
    return &kInterface;
}

PrGpuDevice* PR_NULLABLE prGpuDeviceCreate(PrGpuBackend backend, const PrGpuDeviceOptions* PR_NULLABLE options) {
    PrGpuDeviceOptions o = options != nullptr ? *options : prGpuDeviceOptionsDefault();
    if (backend != PrGpuBackend_Default)
        return create(backend, o);
    uint32_t chosen = 0;
    if (prBackendFromEnvironment(&kInterface, &chosen, nullptr) != PrStatus_Ok)
        return nullptr;
    if (chosen != PrGpuBackend_Default)
        return create(PrGpuBackend(chosen), o);
    return createDefault(o);
}

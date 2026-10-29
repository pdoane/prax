// The Vulkan backend's use of the operating system: the loader that it opens, the environment that configures it, the
// standard error that validation writes to, and the native surfaces that it presents to.

#include "gpu/backends/vulkan/src/vulkan_platform.h"

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>
#else
#include <dlfcn.h>
#endif
#if defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan_metal.h>
#endif
#include <stdio.h>
#include <stdlib.h>

namespace pr_impl {

void* PR_NULLABLE vulkanLoaderOpen(PFN_vkGetInstanceProcAddr PR_NULLABLE* PR_NONNULL getInstanceProcAddr) {
    *getInstanceProcAddr = nullptr;
#if defined(_WIN32)
    HMODULE module = LoadLibraryA("vulkan-1.dll");
    if (module == nullptr)
        return nullptr;
    *getInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(module, "vkGetInstanceProcAddr")));
    if (*getInstanceProcAddr == nullptr) {
        FreeLibrary(module);
        return nullptr;
    }
    return module;
#else
#if defined(__APPLE__)
    // The loader finds the system's drivers, such as KosmicKrisp. The candidates are, in order, the application's own
    // loader, the one that the environment's library paths find, and the ones that the Vulkan SDK and Homebrew install.
    // dyld does not search the directories of the last two by default. The backend does not enumerate portability
    // drivers, so the loader does not list MoltenVK.
    const char* names[] = {"@rpath/libvulkan.1.dylib", "libvulkan.1.dylib", "/usr/local/lib/libvulkan.1.dylib",
                           "/opt/homebrew/lib/libvulkan.1.dylib"};
#else
    const char* names[] = {"libvulkan.so.1", "libvulkan.so"};
#endif
    for (const char* name : names) {
        void* module = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (module == nullptr)
            continue;
        *getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(module, "vkGetInstanceProcAddr"));
        if (*getInstanceProcAddr != nullptr)
            return module;
        dlclose(module);
    }
    return nullptr;
#endif
}

void vulkanLoaderClose(void* PR_NULLABLE loader) {
    if (loader == nullptr)
        return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(loader));
#else
    dlclose(loader);
#endif
}

int32_t vulkanEnvironment(const char* name, int32_t fallback) {
    char value[16];
#if defined(_WIN32)
    DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0 || length >= sizeof(value))
        return fallback;
#else
    const char* found = getenv(name);
    if (found == nullptr)
        return fallback;
    size_t length = 0;
    while (found[length] != '\0' && length + 1 < sizeof(value)) {
        value[length] = found[length];
        ++length;
    }
    value[length] = '\0';
#endif
    char* end = nullptr;
    long parsed = strtol(value, &end, 10);
    return end != value && *end == '\0' ? int32_t(parsed) : fallback;
}

void vulkanLog(const char* prefix, const char* text) {
    fprintf(stderr, "%s%s\n", prefix, text);
}

const char* PR_NULLABLE vulkanSurfaceExtension(void) {
#if defined(_WIN32)
    return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#elif defined(__APPLE__)
    return VK_EXT_METAL_SURFACE_EXTENSION_NAME;
#else
    return nullptr;
#endif
}

VkResult vulkanSurfaceCreate(VulkanDevice* d, const PrAppSurface* surface, const PrGpuSwapChainDesc* desc,
                             VkSurfaceKHR PR_NULLABLE* PR_NONNULL out) {
    *out = VK_NULL_HANDLE;
#if defined(_WIN32)
    (void)desc;
    if (surface->kind != PrAppSurfaceKind_Hwnd)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    PFN_vkCreateWin32SurfaceKHR create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        d->vk.GetInstanceProcAddr(d->instance, "vkCreateWin32SurfaceKHR"));
    if (create == nullptr)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = GetModuleHandleA(nullptr);
    info.hwnd = static_cast<HWND>(surface->handle);
    return create(d->instance, &info, nullptr, out);
#elif defined(__APPLE__)
    if (surface->kind != PrAppSurfaceKind_NsView)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    PFN_vkCreateMetalSurfaceEXT create = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
        d->vk.GetInstanceProcAddr(d->instance, "vkCreateMetalSurfaceEXT"));
    if (create == nullptr)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkMetalSurfaceCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    info.pLayer = static_cast<const CAMetalLayer*>(vulkanMetalLayer(surface->handle, desc->anchor));
    return create(d->instance, &info, nullptr, out);
#else
    (void)d;
    (void)surface;
    (void)desc;
    return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
}

} // namespace pr_impl

# GPUs: the interface, and the backends that the build turns on.
pr_library(gpu INCLUDE include
    src/gpu.cpp
    src/gpu_graph.cpp
    backends/shared/src/gpu_shared_device.cpp
    backends/shared/src/gpu_shared_ranges.cpp
    backends/shared/src/gpu_shared_upload.cpp
)
target_link_libraries(pr_gpu PUBLIC prax::core prax::geom prax::app prax::color prax::task PRIVATE prax::core_cpp)
pr_slang(pr_gpu backends/shared/src/gpu_shared_mipmaps.slang VARIABLE kGpuSharedMipmapShaders)
# On Apple's platforms, any device copies a CVPixelBuffer that it does not import in place.
if (APPLE)
    target_sources(pr_gpu PRIVATE src/gpu_corevideo.mm)
    set_source_files_properties(src/gpu_corevideo.mm PROPERTIES COMPILE_OPTIONS -fno-objc-arc)
    target_compile_definitions(pr_gpu PRIVATE PR_GPU_HAS_COREVIDEO)
    target_link_libraries(pr_gpu PUBLIC "-framework CoreVideo")
endif()
# The Metal backends hold Objective-C objects in plain structures, with manual reference counting.
pr_backend_shared(gpu metal SOURCES backends/shared/src/gpu_shared_metal.mm)
set_source_files_properties(backends/shared/src/gpu_shared_metal.mm PROPERTIES COMPILE_OPTIONS -fno-objc-arc)
if (PR_GPU_METAL OR PR_GPU_METAL4)
    target_link_libraries(pr_gpu PUBLIC "-framework Metal" "-framework QuartzCore" "-framework CoreVideo"
                                 "-framework Foundation" "-framework CoreFoundation")
    # A swap chain presents to an AppKit view on macOS, and to a UIKit view on UIKit's platforms.
    if (PR_MACOS)
        target_link_libraries(pr_gpu PUBLIC "-framework AppKit")
    elseif (PR_UIKIT)
        target_link_libraries(pr_gpu PUBLIC "-framework UIKit")
    endif()
endif()
pr_backend(gpu metal SHARED metal INCLUDE backends/metal/include SOURCES backends/metal/src/gpu_metal.mm)
set_source_files_properties(backends/metal/src/gpu_metal.mm PROPERTIES COMPILE_OPTIONS -fno-objc-arc)
pr_backend(gpu metal4 SHARED metal INCLUDE backends/metal4/include SOURCES backends/metal4/src/gpu_metal4.mm)
set_source_files_properties(backends/metal4/src/gpu_metal4.mm PROPERTIES COMPILE_OPTIONS -fno-objc-arc)

# The Vulkan backend builds against the Vulkan API's headers, and loads the system's Vulkan loader at run time.
pr_backend(gpu vulkan INCLUDE backends/vulkan/include SOURCES
    backends/vulkan/src/gpu_vulkan.cpp
    backends/vulkan/src/vulkan_commands.cpp
    backends/vulkan/src/vulkan_memory.cpp
    backends/vulkan/src/vulkan_pipeline.cpp
    backends/vulkan/src/vulkan_platform.cpp
    backends/vulkan/src/vulkan_resources.cpp
    backends/vulkan/src/vulkan_swap_chain.cpp
)
if (PR_GPU_VULKAN)
    if (APPLE)
        target_sources(pr_gpu PRIVATE backends/vulkan/src/vulkan_platform_apple.mm)
        set_source_files_properties(backends/vulkan/src/vulkan_platform_apple.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    endif()
    target_include_directories(pr_gpu SYSTEM PRIVATE ${PR_VULKAN_HEADERS})
    if ("spirv" IN_LIST PR_SHADER_TARGETS)
        target_compile_definitions(pr_gpu PRIVATE PR_GPU_HAS_SPIRV)
    endif()
    if ("spirv_local_read" IN_LIST PR_SHADER_TARGETS)
        target_compile_definitions(pr_gpu PRIVATE PR_GPU_HAS_SPIRV_LOCAL_READ)
    endif()
    target_link_libraries(pr_gpu PRIVATE ${CMAKE_DL_LIBS})
endif()

pr_backend(gpu d3d12 INCLUDE backends/d3d12/include SOURCES backends/d3d12/src/gpu_d3d12.cpp)
if (PR_GPU_D3D12)
    target_link_libraries(pr_gpu PUBLIC d3d12 dcomp dxgi)
endif()
pr_backend(gpu d3d11 INCLUDE backends/d3d11/include SOURCES backends/d3d11/src/gpu_d3d11.cpp)
if (PR_GPU_D3D11)
    target_link_libraries(pr_gpu PUBLIC d3d11 dcomp dxgi dxguid)
endif()

# The memory devices: the memory backend, which presents nothing, and the Canvas 2D backend, whose textures are also a
# web page's canvases and which builds only for the web. Both backends create the same device.
pr_backend_shared(gpu memory SOURCES backends/shared/src/gpu_shared_memory.cpp)
pr_backend(gpu memory SHARED memory INCLUDE backends/memory/include SOURCES backends/memory/src/gpu_memory.cpp)
pr_backend(gpu canvas2d SHARED memory INCLUDE backends/canvas2d/include SOURCES backends/canvas2d/src/gpu_canvas2d.cpp)
if (PR_GPU_CANVAS2D)
    pr_js_library(pr_gpu backends/canvas2d/src/gpu_canvas2d.js)
endif()

# The WebGPU backend runs on the page's WebGPU through a library of its own, which decodes each command buffer's
# commands.
pr_backend(gpu webgpu INCLUDE backends/webgpu/include SOURCES backends/webgpu/src/gpu_webgpu.cpp)
if (PR_GPU_WEBGPU)
    pr_js_library(pr_gpu backends/webgpu/src/gpu_webgpu.js)
endif()

# The selector for device backends. Programs and composition libraries link it.
pr_select(gpu INCLUDE select/include SOURCES select/src/gpu_select.cpp)

if (PR_DEVELOP)
    pr_module_tests(gpu
        tests/test_gpu.cpp
        tests/test_gpu_formats.cpp
        tests/test_gpu_graph.cpp
        tests/test_gpu_passes.cpp
        backends/shared/tests/test_gpu_shared_ranges.cpp
        tests/test_gpu_stress.cpp
        tests/test_gpu_c.c
    )
    target_link_libraries(gpu_tests PRIVATE prax::gpu_select prax::task)
    pr_slang(gpu_tests tests/test_gpu.slang VARIABLE kTestGpuShaders)
    pr_slang(gpu_tests tests/test_gpu_passes.slang VARIABLE kTestGpuPassShaders)
    # Each backend's own tests, which record into the application's command buffers and passes with the same shaders.
    pr_slang(gpu_tests tests/test_gpu_native.slang VARIABLE kTestGpuNativeShaders)
    if (PR_GPU_METAL)
        target_sources(gpu_tests PRIVATE backends/metal/tests/test_gpu_metal.mm)
        set_source_files_properties(backends/metal/tests/test_gpu_metal.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    endif()
    if (PR_GPU_METAL4)
        target_sources(gpu_tests PRIVATE backends/metal4/tests/test_gpu_metal4.mm)
        set_source_files_properties(backends/metal4/tests/test_gpu_metal4.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    endif()
    if (PR_GPU_VULKAN)
        target_sources(gpu_tests PRIVATE backends/vulkan/tests/test_gpu_vulkan.cpp
                                         backends/vulkan/tests/test_gpu_vulkan_memory.cpp)
        target_include_directories(gpu_tests SYSTEM PRIVATE ${PR_VULKAN_HEADERS})
    endif()
    if (PR_GPU_D3D12)
        target_sources(gpu_tests PRIVATE backends/d3d12/tests/test_gpu_d3d12.cpp)
    endif()
    if (PR_GPU_D3D11)
        target_sources(gpu_tests PRIVATE backends/d3d11/tests/test_gpu_d3d11.cpp)
    endif()
    if (PR_GPU_MEMORY)
        target_sources(gpu_tests PRIVATE backends/memory/tests/test_gpu_memory.cpp)
        target_link_libraries(gpu_tests PRIVATE prax::app)
    endif()
    if (PR_GPU_CANVAS2D)
        target_sources(gpu_tests PRIVATE backends/canvas2d/tests/test_gpu_canvas2d.cpp)
    endif()
    # On the web, the same tests run again in a page, where they reach the page's WebGPU.
    if (PR_GPU_WEBGPU)
        pr_page_tests(gpu tests/test_gpu.cpp tests/test_gpu_formats.cpp tests/test_gpu_graph.cpp tests/test_gpu_passes.cpp
            tests/test_gpu_c.c)
        target_link_libraries(gpu_page_tests PRIVATE prax::gpu_select prax::task)
        target_compile_definitions(gpu_page_tests PRIVATE PR_GPU_TEST_WEBGPU)
        # The shaders that gpu_tests compiles, which a target of the same directory shares.
        target_sources(gpu_page_tests PRIVATE ${PROJECT_BINARY_DIR}/slang/gpu/tests/kTestGpuShaders.h
                                              ${PROJECT_BINARY_DIR}/slang/gpu/tests/kTestGpuPassShaders.h)
        target_include_directories(gpu_page_tests PRIVATE ${PROJECT_BINARY_DIR}/slang)
    endif()
endif()

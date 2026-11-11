# GPUs: the interface, and the backends that the build turns on.
pr_library(gpu INCLUDE include
    src/gpu.cpp
    backends/shared/src/gpu_shared_device.cpp
    backends/shared/src/gpu_shared_upload.cpp
)
target_link_libraries(pr_gpu PUBLIC prax::core prax::geom prax::app prax::color PRIVATE prax::core_cpp)
pr_slang(pr_gpu backends/shared/src/gpu_shared_mipmaps.slang VARIABLE kGpuSharedMipmapShaders)
# On Apple's platforms, any device copies a CVPixelBuffer that it does not import in place.
if (APPLE)
    target_sources(pr_gpu PRIVATE src/gpu_corevideo.mm)
    set_source_files_properties(src/gpu_corevideo.mm PROPERTIES COMPILE_OPTIONS -fno-objc-arc)
    target_compile_definitions(pr_gpu PRIVATE PR_GPU_HAS_COREVIDEO)
    target_link_libraries(pr_gpu PUBLIC "-framework CoreVideo")
endif()
if (PR_GPU_METAL OR PR_GPU_METAL4)
    # The Metal backends hold Objective-C objects in plain structures, with manual reference counting.
    target_sources(pr_gpu PRIVATE backends/shared/src/gpu_shared_metal.mm)
    set_source_files_properties(backends/shared/src/gpu_shared_metal.mm PROPERTIES COMPILE_OPTIONS -fno-objc-arc)
    target_link_libraries(pr_gpu PUBLIC "-framework Metal" "-framework QuartzCore" "-framework CoreVideo"
                                 "-framework Foundation" "-framework CoreFoundation")
    # A swap chain presents to an AppKit view on macOS, and to a UIKit view on UIKit's platforms.
    if (PR_MACOS)
        target_link_libraries(pr_gpu PUBLIC "-framework AppKit")
    elseif (PR_UIKIT)
        target_link_libraries(pr_gpu PUBLIC "-framework UIKit")
    endif()
endif()
if (PR_GPU_METAL)
    target_sources(pr_gpu PRIVATE backends/metal/src/gpu_metal.mm)
    pr_headers(pr_gpu PUBLIC backends/metal/include)
    set_source_files_properties(backends/metal/src/gpu_metal.mm PROPERTIES COMPILE_OPTIONS -fno-objc-arc)
    target_compile_definitions(pr_gpu PRIVATE PR_GPU_HAS_METAL)
endif()
if (PR_GPU_METAL4)
    target_sources(pr_gpu PRIVATE backends/metal4/src/gpu_metal4.mm)
    pr_headers(pr_gpu PUBLIC backends/metal4/include)
    set_source_files_properties(backends/metal4/src/gpu_metal4.mm PROPERTIES COMPILE_OPTIONS -fno-objc-arc)
    target_compile_definitions(pr_gpu PRIVATE PR_GPU_HAS_METAL4)
endif()
# The Vulkan backend builds against the Vulkan API's headers, and loads the system's Vulkan loader at run time.
if (PR_GPU_VULKAN)
    target_sources(pr_gpu PRIVATE
        backends/vulkan/src/gpu_vulkan.cpp
        backends/vulkan/src/vulkan_commands.cpp
        backends/vulkan/src/vulkan_pipeline.cpp
        backends/vulkan/src/vulkan_platform.cpp
        backends/vulkan/src/vulkan_resources.cpp
        backends/vulkan/src/vulkan_swap_chain.cpp
    )
    if (APPLE)
        target_sources(pr_gpu PRIVATE backends/vulkan/src/vulkan_platform_apple.mm)
        set_source_files_properties(backends/vulkan/src/vulkan_platform_apple.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    endif()
    pr_headers(pr_gpu PUBLIC backends/vulkan/include)
    target_include_directories(pr_gpu SYSTEM PRIVATE ${PR_VULKAN_HEADERS})
    target_compile_definitions(pr_gpu PRIVATE PR_GPU_HAS_VULKAN)
    if ("spirv" IN_LIST PR_SHADER_TARGETS)
        target_compile_definitions(pr_gpu PRIVATE PR_GPU_HAS_SPIRV)
    endif()
    if ("spirv_local_read" IN_LIST PR_SHADER_TARGETS)
        target_compile_definitions(pr_gpu PRIVATE PR_GPU_HAS_SPIRV_LOCAL_READ)
    endif()
    target_link_libraries(pr_gpu PRIVATE ${CMAKE_DL_LIBS})
endif()
if (PR_GPU_D3D12)
    target_sources(pr_gpu PRIVATE backends/d3d12/src/gpu_d3d12.cpp)
    pr_headers(pr_gpu PUBLIC backends/d3d12/include)
    target_compile_definitions(pr_gpu PRIVATE PR_GPU_HAS_D3D12)
    target_link_libraries(pr_gpu PUBLIC d3d12 dxgi)
endif()

# The memory devices: the memory backend, which presents nothing, and the Canvas 2D backend, whose textures are also a
# web page's canvases and which builds only for the web. Both backends create the same device.
if (PR_GPU_MEMORY OR PR_GPU_CANVAS2D)
    target_sources(pr_gpu PRIVATE backends/shared/src/gpu_shared_memory.cpp)
endif()
if (PR_GPU_MEMORY)
    target_sources(pr_gpu PRIVATE backends/memory/src/gpu_memory.cpp)
    pr_headers(pr_gpu PUBLIC backends/memory/include)
    target_compile_definitions(pr_gpu PRIVATE PR_GPU_HAS_MEMORY)
endif()
if (PR_GPU_CANVAS2D)
    target_sources(pr_gpu PRIVATE backends/canvas2d/src/gpu_canvas2d.cpp)
    pr_headers(pr_gpu PUBLIC backends/canvas2d/include)
    target_compile_definitions(pr_gpu PRIVATE PR_GPU_HAS_CANVAS2D)
    pr_js_library(pr_gpu backends/canvas2d/src/gpu_canvas2d.js)
endif()

if (PR_DEVELOP)
    pr_module_tests(gpu
        tests/test_gpu.cpp
        tests/test_gpu_c.c
    )
    pr_slang(gpu_tests tests/test_gpu.slang VARIABLE kTestGpuShaders)
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
        target_sources(gpu_tests PRIVATE backends/vulkan/tests/test_gpu_vulkan.cpp)
        target_include_directories(gpu_tests SYSTEM PRIVATE ${PR_VULKAN_HEADERS})
    endif()
    if (PR_GPU_D3D12)
        target_sources(gpu_tests PRIVATE backends/d3d12/tests/test_gpu_d3d12.cpp)
    endif()
    if (PR_GPU_MEMORY)
        target_sources(gpu_tests PRIVATE backends/memory/tests/test_gpu_memory.cpp)
        target_link_libraries(gpu_tests PRIVATE prax::app)
    endif()
    if (PR_GPU_CANVAS2D)
        target_sources(gpu_tests PRIVATE backends/canvas2d/tests/test_gpu_canvas2d.cpp)
    endif()
endif()

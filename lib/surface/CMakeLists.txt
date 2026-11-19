# Surfaces, and the immersive sessions that make the surfaces of a headset's displays, through the backends that the
# build turns on.
pr_library(surface INCLUDE include src/surface.cpp)
target_link_libraries(pr_surface PUBLIC prax::core prax::geom PRIVATE prax::core_cpp)

# The CompositorServices backend: a SwiftUI CompositorLayer's layer renderer, and ARKit's world tracking, on visionOS.
if (PR_SURFACE_COMPOSITOR_SERVICES)
    target_sources(pr_surface PRIVATE backends/compositor_services/src/surface_compositor_services.mm)
    pr_headers(pr_surface PUBLIC backends/compositor_services/include)
    set_source_files_properties(backends/compositor_services/src/surface_compositor_services.mm
        PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_compile_definitions(pr_surface PRIVATE PR_SURFACE_HAS_COMPOSITOR_SERVICES)
    target_link_libraries(pr_surface PUBLIC "-framework CompositorServices" "-framework ARKit" "-framework QuartzCore"
                                     "-framework Metal")
endif()

// The Swift host: an application of SwiftUI's own, without lib/app, whose fully immersive space lib/surface and lib/gpu
// draw (swift_host.mm). It checks that they serve a host other than lib/app.

import CompositorServices
import SwiftUI

// The layer's configuration for a session of lib/surface (pr/surface_compositor_services.h): no foveation, and a layered
// layout where there is one, which a pass of every view draws.
struct Configuration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities, configuration: inout LayerRenderer.Configuration) {
        configuration.isFoveationEnabled = false
        configuration.layout = capabilities.supportedLayouts(options: []).contains(.layered) ? .layered : .shared
        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float
    }
}

// A window that opens the immersive space once it shows.
struct Launcher: View {
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace

    var body: some View {
        Text("Swift host")
            .padding(32)
            .task { _ = await openImmersiveSpace(id: "space") }
    }
}

@main
struct SwiftHostApp: App {
    var body: some Scene {
        WindowGroup {
            Launcher()
        }
        ImmersiveSpace(id: "space") {
            CompositorLayer(configuration: Configuration()) { renderer in
                let thread = Thread { exit(prSwiftHostRun(Unmanaged.passUnretained(renderer).toOpaque()) ? 0 : 1) }
                thread.name = "swift host"
                thread.start()
            }
        }
        .immersionStyle(selection: .constant(.full), in: .full)
    }
}

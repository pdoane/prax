// The immersive space of lib/app's UIKit backend on visionOS, which only SwiftUI makes: a scene that SwiftUI hosts in
// the UIKit application (UIHostingSceneDelegate), holding a CompositorLayer that a session of lib/surface draws. The
// rest of the backend reaches it through the C function below, and hears from it through the functions that it gives
// (uikit_immersive.mm).

import CompositorServices
import SwiftUI
import UIKit

// The layer's configuration for a session of lib/surface (pr/surface_compositor_services.h): no foveation, and a layered
// layout where there is one, which a pass of every view draws.
struct PrUiKitImmersiveConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities, configuration: inout LayerRenderer.Configuration) {
        configuration.isFoveationEnabled = false
        configuration.layout = capabilities.supportedLayouts(options: []).contains(.layered) ? .layered : .shared
        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float
    }
}

// What the backend hears: the space's layer renderer once it opens, and that it failed to open.
typealias PrUiKitImmersiveOpenedFn = @convention(c) (UnsafeMutableRawPointer) -> Void
typealias PrUiKitImmersiveFailedFn = @convention(c) () -> Void

@MainActor var prUiKitImmersiveOpened: PrUiKitImmersiveOpenedFn?

// The delegate that SwiftUI hosts the space's scene for. Its root scene is the space.
final class PrUiKitImmersiveDelegate: UIResponder, UIHostingSceneDelegate {
    static var rootScene: some Scene {
        ImmersiveSpace(id: "prax") {
            CompositorLayer(configuration: PrUiKitImmersiveConfiguration()) { renderer in
                MainActor.assumeIsolated {
                    prUiKitImmersiveOpened?(Unmanaged.passUnretained(renderer).toOpaque())
                }
            }
        }
        .immersionStyle(selection: .constant(.full), in: .full)
    }
}

// Asks the system to open the space, and gives the functions that hear that it opened and that it failed to.
@c @MainActor
func prUiKitImmersiveActivate(_ opened: PrUiKitImmersiveOpenedFn, _ failed: PrUiKitImmersiveFailedFn) {
    prUiKitImmersiveOpened = opened
    guard let request = UISceneSessionActivationRequest(hostingDelegateClass: PrUiKitImmersiveDelegate.self,
                                                        id: "prax") else {
        failed()
        return
    }
    UIApplication.shared.activateSceneSession(for: request) { _ in
        DispatchQueue.main.async { failed() }
    }
}

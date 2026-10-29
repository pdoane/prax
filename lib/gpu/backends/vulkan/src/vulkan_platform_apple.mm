// The Vulkan backend's use of the operating system on Apple platforms: the CAMetalLayer of a view, which a Metal
// surface presents to.

#include "gpu/backends/vulkan/src/vulkan_platform.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

namespace pr_impl {

void* vulkanMetalLayer(void* view, PrGpuSwapChainAnchor anchor) {
    @autoreleasepool {
        NSView* v = (__bridge NSView*)view;
        // A view that is backed by a CAMetalLayer keeps that layer, so that AppKit asks the view to display as it
        // resizes. Any other view hosts a new layer. The driver sets the layer's device, format, color space and
        // drawable size from the swap chain.
        CAMetalLayer* layer = [v.layer isKindOfClass:CAMetalLayer.class] ? (CAMetalLayer*)v.layer
                                                                         : [CAMetalLayer layer];
        layer.contentsScale = v.window != nil ? v.window.backingScaleFactor : 1;
        if (v.layer != layer) {
            v.layer = layer;
            v.wantsLayer = YES;
        }
        // AppKit sets the gravity of a layer that backs a view from the view's placement. The swap chain sets the
        // gravity of a layer that the view hosts. A gravity's top is the layer's greatest y, which is the window's
        // bottom if the layer's geometry is flipped.
        bool stretch = anchor == PrGpuSwapChainAnchor_Stretch;
        v.layerContentsPlacement = stretch ? NSViewLayerContentsPlacementScaleAxesIndependently
                                           : NSViewLayerContentsPlacementTopLeft;
        layer.contentsGravity = stretch                     ? kCAGravityResize
                                : [layer contentsAreFlipped] ? kCAGravityBottomLeft
                                                             : kCAGravityTopLeft;
        return (__bridge void*)layer;
    }
}

} // namespace pr_impl

// The shaders' side of pr/gpu.h: handles to resources, the root data of a draw or a dispatch, and reading a pass's
// target. A shader includes this file (`#include "pr/gpu.slang"`), which cmake/shader_library.cmake compiles once for
// each backend with PR_GPU_METAL, PR_GPU_VULKAN or PR_GPU_D3D12 defined to 1.
//
// A shader declares its root data as structures of its own, with the macros below:
//
//     PR_GPU_PUSH(Push, push);                // at most kPrGpuMaxPushBytes, set by prGpuCommandsSetPush
//     PR_GPU_UNIFORMS(Uniforms, uniforms);    // at most kPrGpuMaxUniformBytes, set by prGpuCommandsSetUniforms
//
// A field that names a resource is a u32 handle: a buffer's or a texture's id, a sampler's id, or a storage handle
// (prGpuBufferStorageHandle, prGpuTextureStorageHandle). A shader reads handles from any data that it can read,
// including another resource, and turns one into its resource with prGpuGet<T>(handle).
//
// A buffer holds data of any type at byte offsets. A shader reads a value of type T with prGpuLoad<T>(buffer, offset),
// and writes through RWByteAddressBuffer. T lays out as a C structure does when each of its fields lies at a multiple
// of its alignment (16 bytes for a 4-vector, 8 for a 2-vector, 4 for a scalar) with no 3-vectors, and its size is a
// multiple of its largest alignment. `offset` is a multiple of T's alignment. The loads of the fields that a shader
// uses are then as wide as the hardware allows.

#pragma once

#if !defined(PR_GPU_METAL) && !defined(PR_GPU_VULKAN) && !defined(PR_GPU_D3D12)
#error "pr/gpu.slang is compiled for one backend: PR_GPU_METAL, PR_GPU_VULKAN or PR_GPU_D3D12"
#endif

// The root data. Push data is push constants on Vulkan, root constants on Direct3D 12, and a buffer of the command
// buffer's stream on Metal. Uniforms are a uniform buffer in the stream everywhere, which the hardware reads through
// its constant path.
//
// Root data lays out as C lays it out under the rules of buffers above, and also under the constant buffer packing of
// Direct3D 12 and Vulkan's std140: an array's elements, and a structure, start at multiples of 16 bytes, and a field
// does not straddle one. So an array is of 4-vectors, or of structures whose size is a multiple of 16 bytes, and a
// structure inside root data starts at a multiple of 16 bytes. cmake/shader_library.cmake checks each library's root
// data against that rule.
#define PR_GPU_PUSH(Type, name) [[vk::push_constant]] ConstantBuffer<Type> name : register(b0)
#define PR_GPU_UNIFORMS(Type, name) [[vk::binding(0, 1)]] ConstantBuffer<Type> name : register(b1)

// The slots, which every device has: the resources bound to a command buffer's slots (prGpuCommandsSetBuffer and the
// others), declared as globals of the shader at a slot of each kind. `slot` is a literal number below the device's
// maximum of the kind (PrGpuDeviceInfo).
//
//     PR_GPU_BUFFER(ByteAddressBuffer, vertices, 0);          // a Buffer slot, read
//     PR_GPU_RW_BUFFER(RWByteAddressBuffer, counts, 0);       // an RwBuffer slot, read and written
//     PR_GPU_TEXTURE(Texture2D<float4>, image, 0);            // a Texture slot, and its sampler as imageSampler
//     PR_GPU_RW_TEXTURE(WTexture2D<float4>, written, 0);      // an RwTexture slot, written
//
// A buffer is any buffer type, such as ByteAddressBuffer or StructuredBuffer<T>, and a texture any texture type. An
// RwTexture slot that a shader only writes is of a write-only type, such as WTexture2D, which every format that a
// device stores allows. One that a shader reads and writes, such as RWTexture2D, takes only the formats that the device
// reads and writes in place, which WebGPU and Metal without bindless limit to single 32-bit channels.
//
// Every target declares a slot with its Vulkan binding, which Slang checks every register against: Vulkan pushes the
// slots in set 1 after the uniforms and the target, as vulkan_internal.h's VulkanPushBinding places them, with a
// Texture slot's sampled image and sampler at two bindings. Each target then gives the slot its register, in space 1
// for buffers and space 2 for textures.
#define PR_GPU_PASTE_(a, b) a##b
#define PR_GPU_PASTE(a, b) PR_GPU_PASTE_(a, b)

#define PR_GPU_BUFFER(Type, name, slot) [[vk::binding(2 + slot, 1)]] Type name PR_GPU_BUFFER_REGISTER(slot)
#define PR_GPU_RW_BUFFER(Type, name, slot) [[vk::binding(6 + slot, 1)]] Type name PR_GPU_RW_BUFFER_REGISTER(slot)
#define PR_GPU_TEXTURE(Type, name, slot)                                        \
    [[vk::binding(10 + 2 * slot, 1)]] Type name PR_GPU_TEXTURE_REGISTER(slot); \
    [[vk::binding(11 + 2 * slot, 1)]] SamplerState name##Sampler PR_GPU_SAMPLER_REGISTER(slot)
#define PR_GPU_RW_TEXTURE(Type, name, slot) \
    [[vk::binding(26 + slot, 1)]] PR_GPU_RW_TEXTURE_ATTRIBUTE(slot) Type name PR_GPU_RW_TEXTURE_REGISTER(slot)

#if defined(PR_GPU_METAL)

// Metal binds every buffer in one table, after the root data and the heaps, and every texture in another, which Slang
// chooses by the resource's type and numbers by the register. The tables below give each slot its index, as
// gpu_shared_metal.h's GpuMetalBinding and GpuMetalTextureBinding place them. Metal ignores the spaces, which only keep
// Slang from taking a texture's index for a buffer's. Slang numbers read-write textures in the order of their
// declarations instead, so an RwTexture slot also names its index in an attribute, which the reflection keeps and
// cmake/shader_library.cmake moves the texture to.
[__AttributeUsage(_AttributeTargets.Var)]
struct PrGpuMetalTextureAttribute {
    int index;
};

#define PR_GPU_BUFFER_INDEX_0 4
#define PR_GPU_BUFFER_INDEX_1 5
#define PR_GPU_BUFFER_INDEX_2 6
#define PR_GPU_BUFFER_INDEX_3 7
#define PR_GPU_BUFFER_INDEX_4 8
#define PR_GPU_BUFFER_INDEX_5 9
#define PR_GPU_BUFFER_INDEX_6 10
#define PR_GPU_BUFFER_INDEX_7 11
#define PR_GPU_RW_BUFFER_INDEX_0 12
#define PR_GPU_RW_BUFFER_INDEX_1 13
#define PR_GPU_RW_BUFFER_INDEX_2 14
#define PR_GPU_RW_BUFFER_INDEX_3 15
#define PR_GPU_RW_BUFFER_INDEX_4 16
#define PR_GPU_RW_BUFFER_INDEX_5 17
#define PR_GPU_RW_BUFFER_INDEX_6 18
#define PR_GPU_RW_BUFFER_INDEX_7 19
#define PR_GPU_RW_TEXTURE_INDEX_0 16
#define PR_GPU_RW_TEXTURE_INDEX_1 17
#define PR_GPU_RW_TEXTURE_INDEX_2 18
#define PR_GPU_RW_TEXTURE_INDEX_3 19
#define PR_GPU_RW_TEXTURE_INDEX_4 20
#define PR_GPU_RW_TEXTURE_INDEX_5 21
#define PR_GPU_RW_TEXTURE_INDEX_6 22
#define PR_GPU_RW_TEXTURE_INDEX_7 23
#define PR_GPU_BUFFER_REGISTER(slot) : register(PR_GPU_PASTE(t, PR_GPU_BUFFER_INDEX_##slot), space1)
#define PR_GPU_RW_BUFFER_REGISTER(slot) : register(PR_GPU_PASTE(u, PR_GPU_RW_BUFFER_INDEX_##slot), space1)
#define PR_GPU_TEXTURE_REGISTER(slot) : register(t##slot, space2)
#define PR_GPU_SAMPLER_REGISTER(slot) : register(s##slot, space2)
#define PR_GPU_RW_TEXTURE_REGISTER(slot) : register(PR_GPU_PASTE(u, PR_GPU_RW_TEXTURE_INDEX_##slot), space2)
#define PR_GPU_RW_TEXTURE_ATTRIBUTE(slot) [PrGpuMetalTexture(PR_GPU_RW_TEXTURE_INDEX_##slot)]

#elif defined(PR_GPU_VULKAN)

#define PR_GPU_BUFFER_REGISTER(slot)
#define PR_GPU_RW_BUFFER_REGISTER(slot)
#define PR_GPU_TEXTURE_REGISTER(slot)
#define PR_GPU_SAMPLER_REGISTER(slot)
#define PR_GPU_RW_TEXTURE_REGISTER(slot)
#define PR_GPU_RW_TEXTURE_ATTRIBUTE(slot)

#else

// Direct3D 12 binds shader resources (t), unordered access (u) and samplers (s) at the slot's number.
#define PR_GPU_BUFFER_REGISTER(slot) : register(t##slot, space1)
#define PR_GPU_RW_BUFFER_REGISTER(slot) : register(u##slot, space1)
#define PR_GPU_TEXTURE_REGISTER(slot) : register(t##slot, space2)
#define PR_GPU_SAMPLER_REGISTER(slot) : register(s##slot, space2)
#define PR_GPU_RW_TEXTURE_REGISTER(slot) : register(u##slot, space2)
#define PR_GPU_RW_TEXTURE_ATTRIBUTE(slot)

#endif

#if defined(PR_GPU_METAL)

// Metal has no descriptor heap. The device's heaps are buffers of 8-byte entries, a texture's or a sampler's
// MTLResourceID or a buffer's GPU address, which a handle indexes. Their Vulkan bindings are never used.
[[vk::binding(0, 3)]] ByteAddressBuffer prGpuResourceHeap : register(t2);
[[vk::binding(1, 3)]] ByteAddressBuffer prGpuSamplerHeap : register(t3);

T prGpuHeapLoad<T : IOpaqueDescriptor>(ByteAddressBuffer heap, uint index) {
    __target_switch {
    case metal:
        __intrinsic_asm "(reinterpret_cast<$TR const device*>($0))[$1]";
    }
}

// Returns the resource that a handle names.
T prGpuGet<T : IOpaqueDescriptor>(uint handle) {
    return prGpuHeapLoad<T>(T.kind == DescriptorKind.Sampler ? prGpuSamplerHeap : prGpuResourceHeap, handle);
}

// A typed read through the buffer's address, which the heap holds.
T prGpuLoadFrom<T>(ByteAddressBuffer heap, uint buffer, uint offset) {
    __target_switch {
    case metal:
        __intrinsic_asm "(*reinterpret_cast<$TR const device*>(reinterpret_cast<const device uchar* const device*>($0)[$1] + $2))";
    }
}

// Returns the value of type T at `offset` bytes into `buffer`.
T prGpuLoad<T>(uint buffer, uint offset) {
    return prGpuLoadFrom<T>(prGpuResourceHeap, buffer, offset);
}

#else

// Returns the resource that a handle names, from the device's descriptor heap: on Vulkan, the bindless set at set 2,
// which the backend lays out as Slang's default mapping of handles reads it (cmake/shader_library.cmake pins the set),
// and on Direct3D 12, the heaps of shader model 6.6.
T prGpuGet<T : IOpaqueDescriptor>(uint handle) {
    return defaultGetDescriptorFromHandle(DescriptorHandle<T>(uint2(handle, 0)));
}

// Returns the value of type T at `offset` bytes into `buffer`.
T prGpuLoad<T>(uint buffer, uint offset) {
    return prGpuGet<ByteAddressBuffer>(buffer).Load<T>(offset);
}

#endif

// The derivatives of a value across a fragment's neighbors, computed for each pixel of a quad where the API tells fine
// derivatives from coarse ones. Metal's are computed for each pixel.
#if defined(PR_GPU_METAL)
#define prGpuDdxFine(x) ddx(x)
#define prGpuDdyFine(x) ddy(x)
#else
#define prGpuDdxFine(x) ddx_fine(x)
#define prGpuDdyFine(x) ddy_fine(x)
#endif

// Reading the target. A fragment shader of a pipeline created with `readsTarget` reads what the pass's first color
// attachment holds at its pixel, as the draws before it left it. Fragments of one draw that cover the same pixel read
// an undefined value, unless the device has PrGpuFeatureFlag_OrderedTargetReads. The entry point declares
// PR_GPU_TARGET_PARAM after its last parameter, and reads the target with prGpuTarget(position) in its own body, where
// `position` is its SV_Position. Only a device with PrGpuFeatureFlag_TargetReads creates such a pipeline. On a target
// that has no target reads, prGpuTarget reads zero, so that the source compiles for every target.
#if defined(PR_GPU_METAL)

// Framebuffer fetch.
#define PR_GPU_TARGET_PARAM , float4 prGpuTargetColor : SV_Target0
#define prGpuTarget(position) prGpuTargetColor

#elif defined(PR_GPU_VULKAN) && defined(PR_GPU_LOCAL_READ)

// An input attachment, read locally in a dynamic rendering pass. It is pushed beside the uniforms, because a pipeline
// layout has one set of push descriptors.
[[vk::input_attachment_index(0)]] [[vk::binding(1, 1)]] SubpassInput<float4> prGpuTargetInput;
#define PR_GPU_TARGET_PARAM
#define prGpuTarget(position) prGpuTargetInput.SubpassLoad()

#else

#define PR_GPU_TARGET_PARAM
#define prGpuTarget(position) float4(0, 0, 0, 0)

#endif

// Views. A pass of several views (PrGpuPassDesc::viewCount) draws each draw once for each view, into the view's layer
// of the attachments. A vertex shader that draws for several views declares PR_GPU_VIEW_PARAM after its last parameter,
// reads its view, from zero, with prGpuView() in its own body, and declares PR_GPU_VIEW_OUTPUT after the last field of
// its output, which it fills with prGpuViewOutput(output). Such a shader draws in a pass of one view too, where its
// view is zero.
//
//     struct VertexOut {
//         float4 position : SV_Position;
//         PR_GPU_VIEW_OUTPUT
//     };
//
//     VertexOut vertexMain(uint vertex : SV_VertexID PR_GPU_VIEW_PARAM) {
//         VertexOut out;
//         out.position = mul(views[prGpuView()].viewProjection, ...);
//         prGpuViewOutput(out);
//         return out;
//     }
#if defined(PR_GPU_METAL)

// Metal amplifies each vertex for each view where the device can, and the view mapping chooses each view's layer. Where
// it cannot, the device draws each draw once for each view, and the shader writes the layer. The view's base, in a
// buffer at gpu_shared_metal.h's GpuMetalBinding_View, is what the amplification adds to: zero where Metal amplifies,
// and the view and its layer where the device draws each view.
struct PrGpuViewBase {
    uint view;
    uint layer;
};
[[vk::binding(0, 4)]] ConstantBuffer<PrGpuViewBase> prGpuViewBase : register(b20);
#define PR_GPU_VIEW_PARAM , uint prGpuAmplification : SV_ViewID
#define PR_GPU_VIEW_OUTPUT uint prGpuLayer : SV_RenderTargetArrayIndex;
#define prGpuView() (prGpuAmplification + prGpuViewBase.view)
#define prGpuViewOutput(output) (output).prGpuLayer = prGpuViewBase.layer

#else

// Vulkan's multiview and Direct3D 12's view instancing draw each view into its layer.
#define PR_GPU_VIEW_PARAM , uint prGpuViewId : SV_ViewID
#define PR_GPU_VIEW_OUTPUT
#define prGpuView() prGpuViewId
#define prGpuViewOutput(output)

#endif

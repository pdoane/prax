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
// `position` is its SV_Position.
#if defined(PR_GPU_METAL)

// Framebuffer fetch.
#define PR_GPU_TARGET_PARAM , float4 prGpuTargetColor : SV_Target0
#define prGpuTarget(position) prGpuTargetColor

#elif defined(PR_GPU_VULKAN) && defined(PR_GPU_LOCAL_READ)

// An input attachment, read locally in a dynamic rendering pass, on a device with local read. It is pushed beside the
// uniforms, because a pipeline layout has one set of push descriptors.
[[vk::input_attachment_index(0)]] [[vk::binding(1, 1)]] SubpassInput<float4> prGpuTargetInput;
#define PR_GPU_TARGET_PARAM
#define prGpuTarget(position) prGpuTargetInput.SubpassLoad()

#elif defined(PR_GPU_VULKAN)

// A copy of the target, which the backend makes before each draw over the draw's scissor, at the target's coordinates.
// It is pushed beside the uniforms, because a pipeline layout has one set of push descriptors.
[[vk::binding(1, 1)]] Texture2D<float4> prGpuTargetCopy;
#define PR_GPU_TARGET_PARAM
#define prGpuTarget(position) prGpuTargetCopy.Load(int3(int2((position).xy), 0))

#else

// A copy of the target that the backend makes before each draw, over the draw's scissor, at the same place.
struct PrGpuTargetCopy {
    uint handle;
};
ConstantBuffer<PrGpuTargetCopy> prGpuTargetCopy : register(b2);
#define PR_GPU_TARGET_PARAM
#define prGpuTarget(position) prGpuGet<Texture2D<float4>>(prGpuTargetCopy.handle).Load(int3(int2((position).xy), 0))

#endif

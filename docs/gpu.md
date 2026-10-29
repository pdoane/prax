# GPU

`lib/gpu` is the interface that every Prax renderer is written against: `lib/draw`'s device backends, canvas, the
examples and the tools. A renderer is written once, in C++ against `pr/gpu.h` and in Slang against `pr/gpu.slang`, and
each backend runs it on one graphics API.

| backend | API | on by default |
| --- | --- | --- |
| `metal` | Metal 4 | macOS (`PR_GPU_METAL`) |
| `d3d12` | Direct3D 12, shader model 6.6, enhanced barriers | Windows (`PR_GPU_D3D12`) |
| `vulkan` | Vulkan 1.3, with mutable descriptors and push descriptors | Linux and Android (`PR_GPU_VULKAN`) |

The library holds the backends that the build turns on, and builds with none of them for a host that brings its own
(`pr/gpu_backend.h`). `prGpuDeviceCreate` creates a device by a backend, or by `PrGpuBackend_Default`: the platform's
own if the build has it. A backend's header creates one over the host's native device and queue (`pr/gpu_metal.h`,
`pr/gpu_d3d12.h`, `pr/gpu_vulkan.h`). The environment variable `PR_GPU_BACKEND` (`metal`, `d3d12` or `vulkan`) chooses
the backend that Default creates, so a program and its tests run on any backend that the build has.

## The model

The interface is bindless, with explicit barriers, because Metal 4, Direct3D 12 and Vulkan all work best that way and
a 3D renderer needs it.

- **Handles are heap slots.** A buffer, a texture or a sampler is a `uint32_t` that is its identity and its slot in
  the device's heap. Creating a resource makes it visible to shaders, and there is no bind step. A shader reads a
  handle from any data that it can read, including another resource, and turns it into the resource with
  `prGpuGet<T>`. A resource that shaders write may have a second handle for writing (`prGpuBufferStorageHandle`,
  `prGpuTextureStorageHandle`), because Direct3D 12 writes through a view of its own.
- **Root data** is push data, at most 128 bytes, and uniforms, at most 64 KiB, set on the command buffer before a draw
  or dispatch. Both are copied, and handles are `uint32_t` fields of either. A pipeline has no vertex layout: shaders
  pull vertices from buffers by handle.
- **One layout for data.** A structure that a shader reads from a buffer (`prGpuLoad<T>`) lays out as C lays it out,
  with each field at a multiple of its alignment (16 bytes for a 4-vector, 8 for a 2-vector, 4 for a scalar) and no
  3-vectors, so C++ and Slang share one definition. Root data is read through the hardware's constant path, so it also
  follows constant buffer packing, under which an array's elements and a structure start at multiples of 16 bytes: its
  arrays are of 4-vectors, or of structures whose size is a multiple of 16 bytes. The shader build checks each
  library's root data against C's layout and fails on a difference.
- **The device tracks no states.** A barrier names a resource's accesses before and after it, and the caller knows
  them. Each backend derives what its API needs from the accesses: Metal 4's stage barriers, and the sync, access and
  image layout of Direct3D 12's enhanced barriers and Vulkan's synchronization2. A texture begins in the accesses of
  `ShaderRead`.
- **Lifetimes need no thought.** An object may be destroyed as soon as the work that uses it is recorded. The device
  keeps it until every command buffer that began before the destruction is done.
- **One timeline.** Each submission returns a fence, and fences rise in the order of submission. A command buffer that
  records into the host's native one is submitted through the device too, so the host never signals anything.
- **Uploads are unbounded and ordered.** `prGpuBufferUpload` and `prGpuTextureUpload` take any amount of
  data, which the device stages and copies in a command buffer of its own ahead of the next submission. Several
  uploads to one resource land as if copied in order.
- **The stream always succeeds.** A command buffer allocates per-frame data from a stream of upload memory that is
  reclaimed once its work completes, and grows as recording needs it. Running out of memory loses the device, which
  hosts already handle.
- **Device loss** is a state, not an error path. A lost device refuses new work, completes every fence, and reads back
  zeros. The host destroys every object and makes them again on a new device.

## Shaders

Shaders are Slang. `cmake/shader_library.cmake` compiles each source at build time into one library per backend that the
platform builds, which a header embeds (`pr_slang` in `cmake/build.cmake`). The library names each entry point, its stage
and, for compute, its group size, which the reflection gives, so a dispatch and its shader cannot disagree. Metal and
SPIR-V hold every entry point in one module. Direct3D 12 takes DXIL for each entry point on its own.

`pr/gpu.slang` is the shaders' side of the interface. The source compiles once per backend with `PR_GPU_METAL`,
`PR_GPU_VULKAN` or `PR_GPU_D3D12`, and the header hides what differs:

| | Metal | Direct3D 12 | Vulkan |
| --- | --- | --- | --- |
| handles | buffers of 8-byte entries that a handle indexes | the shader-visible heaps, indexed directly | the bindless set at set 2: an array of samplers and one of mutable descriptors |
| push data | a buffer in the stream | 32 root constants at `b0` | push constants |
| uniforms | a buffer in the stream | a root constant buffer at `b1` | a uniform buffer, pushed at set 1 |
| reading the target | framebuffer fetch | a copy of the target over the draw's scissor | an input attachment read locally in the rendering, or a copy of the target over the draw's scissor on a device without local read; either is pushed at set 1 |

A pipeline that reads its target (`readsTarget`, and `prGpuTarget` in the shader) reads what the draws before it left.
Where the device lacks `PrGpuFeatureFlag_OrderedTargetReads`, fragments of one draw that cover the same pixel read an
undefined value, so a renderer splits such draws, as `lib/draw`'s fragment renderer does.

## Swap chains

A swap chain presents to an application's surface (`pr/surface.h`), and its device chooses the format and color space
of its textures for what the swap chain shows. SDR is 8-bit sRGB everywhere. For HDR, Metal presents extended sRGB in
half floats, Direct3D 12 presents HDR10 to a display in HDR mode and scRGB elsewhere, and Vulkan presents scRGB where
the surface takes it and HDR10 where it takes that. A host converts its colors for
what `prGpuSwapChainInfo` reports, as the examples' presenter does.

Without `PrGpuSwapChainFlag_Threaded`, a frame presents within the window's display transaction, so a window that is
resized shows each frame at its size. Anchored at the top left, a Direct3D 12 swap chain's buffers only grow, so that a
resize within them waits for no work and allocates nothing.

## Embedding

A host engine brings its own device, and Prax's renderers run inside its frames. A device is made over the host's
device and queue. A command buffer begins over the host's native command buffer (`PrGpuCommandsOptions::native`), and a
pass can wrap a pass that the host has open (`PrGpuPassDesc::host`). Whoever begins a pass ends it. The device's work
changes the host's pipeline, root data and dynamic state. Between the device's work and its own, the host syncs
(`prGpuHostStateSync`), which ends what the device has open outside a pass, such as a Metal encoder, and returns what
the device changed, which the host sets again or marks stale in its own cache. Each backend's header says which native
objects these are. On Direct3D 12, the
device's heaps are bound on every command list that it records into, and a pass of the host's has them bound. On
Vulkan, the host shares its bindless set, its pipeline layout and its memory with the device, so that nothing is bound
again between the host's work and the device's.

## Testing

`gpu_tests` runs every test on each backend that the build has and the system has a device for, with validation, and
tests each backend's embedding with the same shaders: recording into the host's command buffers and drawing in its
passes. It runs under Metal's API validation, which it turns on with `MTL_DEBUG_LAYER` unless the environment sets it,
and tests a window's swap chain on Vulkan. The renderers'
tests compare their images with the `cpu` backend's ([draw.md](draw.md)), on the backend that `PR_GPU_BACKEND` chooses.
`PR_D3D12_DEBUG=1` and `PR_VULKAN_DEBUG=1` turn on Direct3D 12's debug layer and Vulkan's validation layer for a device
that is made without asking for validation, and write their errors to the standard error.

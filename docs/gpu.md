# GPU

`lib/gpu` is the interface that every Prax renderer is written against: `lib/draw`'s device backends, canvas, the
examples and the tools. A renderer is written once, in C++ against `pr/gpu.h` and in Slang against `pr/gpu.slang`, and
each backend runs it on one graphics API.

| backend | API | on by default |
| --- | --- | --- |
| `metal` | Metal 3, the default on macOS | macOS (`PR_GPU_METAL`) |
| `metal4` | Metal 4, kept for experiments | macOS (`PR_GPU_METAL4`) |
| `d3d12` | Direct3D 12, shader model 6.6, enhanced barriers | Windows (`PR_GPU_D3D12`) |
| `vulkan` | Vulkan 1.3, with mutable descriptors and push descriptors | Linux and Android (`PR_GPU_VULKAN`) |
| `memory` | memory, with no shaders, for renderers that draw on the CPU | every platform (`PR_GPU_MEMORY`) |
| `canvas2d` | memory and a web page's canvases, with no shaders | the web (`PR_GPU_CANVAS2D`) |

The library holds the backends that the build turns on. It builds with none of them for an application that brings
its own (`pr/gpu_backend.h`). `prGpuDeviceCreate` creates a device by a backend, or by `PrGpuBackend_Default`, which
tries the build's backends that the system supports in order: the platform's own first, and the memory backend last.
The environment variable `PR_GPU_BACKEND` (such as `metal` or `vulkan`) chooses the backend that Default creates, so a
program and its tests run on any backend in the build. A backend's header creates a device over the application's
native device and queue (`pr/gpu_metal.h`, `pr/gpu_metal4.h`, `pr/gpu_d3d12.h`, `pr/gpu_vulkan.h`).

## The model

The interface is bindless, with explicit barriers, because Metal, Direct3D 12 and Vulkan all work best that way and
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
  them. Each backend derives what its API needs from the accesses: Metal 3's chain of fences, Metal 4's stage
  barriers, and the sync, access and image layout of Direct3D 12's enhanced barriers and Vulkan's synchronization2. A
  texture begins in the accesses of `ShaderRead`.
- **Lifetimes need no thought.** An object may be destroyed as soon as the work that uses it is recorded. The device
  keeps it until every command buffer that began before the destruction is done.
- **One timeline.** Each submission returns a fence, and fences rise in the order of submission. A command buffer that
  records into the application's native one is submitted through the device too, so the application never signals anything.
- **Uploads are unbounded and ordered.** `prGpuBufferUpload` and `prGpuTextureUpload` take any amount of
  data, which the device stages and copies in a command buffer of its own ahead of the next submission. Several
  uploads to one resource land as if copied in order.
- **The stream always succeeds.** A command buffer allocates per-frame data from a stream of upload memory that is
  reclaimed once its work completes, and grows as recording needs it. Running out of memory loses the device, which
  hosts already handle.
- **Device loss** is a state, not an error path. A lost device refuses new work, completes every fence, and reads back
  zeros. The application destroys every object and makes them again on a new device.

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
| reading the target | framebuffer fetch, except in the iOS simulator | none | an input attachment read locally in the rendering, pushed at set 1, on a device with local read |

A pipeline that reads its target (`readsTarget`, and `prGpuTarget` in the shader) reads what the draws before it left,
on a device with `PrGpuFeatureFlag_TargetReads`. A device without it, such as Metal in the iOS simulator, refuses such
a pipeline, and the renderer reads what earlier draws left from a texture of its own.
Where the device lacks `PrGpuFeatureFlag_OrderedTargetReads`, fragments of one draw that cover the same pixel read an
undefined value, so a renderer splits such draws, as `lib/draw`'s fragment renderer does.

## Swap chains

A swap chain presents to an application's surface (`pr/surface.h`), and its device chooses the format and color space
of its textures for what the swap chain shows. SDR is 8-bit sRGB everywhere. For HDR, Metal presents extended sRGB in
half floats, Direct3D 12 presents HDR10 to a display in HDR mode and scRGB elsewhere, and Vulkan presents scRGB where
the surface takes it and HDR10 where it takes that. An application converts its colors for
what `prGpuSwapChainInfo` reports, as the examples' presenter does.

Without `PrGpuSwapChainFlag_Threaded`, a frame presents within the window's display transaction, so a window that is
resized shows each frame at its size. Anchored at the top left, a Direct3D 12 swap chain's buffers only grow, so that a
resize within them waits for no work and allocates nothing.

## Embedding

An application engine brings its own device, and Prax's renderers run inside its frames. A device is made over the
application's device and queue. A command buffer begins over the application's native command buffer
(`PrGpuCommandsOptions::native`), and a pass can wrap a pass that the application has open (`PrGpuPassDesc::native`).
Whoever begins a pass ends it. The device's work changes the application's pipeline, root data and dynamic state.
Between the device's work and its own, the application syncs (`prGpuNativeStateSync`), which ends what the device has
open outside a pass, such as a Metal encoder, and returns what the device changed, which the application sets again or
marks stale in its own cache. Each backend's header says which native objects these are. On Direct3D 12, the device's
heaps are bound on every command list that it records into, and a pass of the application's has them bound. On Vulkan,
the application shares its bindless set, its pipeline layout and its memory with the device, so that nothing is bound
again between the application's work and the device's.

## Native objects

Code beneath lib/gpu reaches a device's work through native objects. This is the reverse of embedding. A device names
the kinds of native object that it hands out (`prGpuDeviceHasNative`), such as Metal's encoders and textures, a
web page's canvases, or texels in memory. A native draw (`prGpuPassNativeDraw`) gets the pass's object of its kind,
such as the pass's Metal encoder or its attachment's canvas. `prGpuTextureNative` gets a texture's object.

A native draw is the same handoff as an application's sync, made within a pass. After the draw, the device binds its own
state again, and the pass's pipeline and dynamic state are unset. A GPU backend calls the draw while it records it. A
memory or canvas device runs work when it is submitted, so it calls the draw while the draw's work runs, and hands out
its textures' objects only then. A device that hands out several kinds keeps them in step, so that work through each
kind sees what the others drew. lib/draw's CPU and Canvas 2D renderers are native draws.

## Imported images

A device makes a texture of an image that was made outside lib/gpu (`prGpuTextureImport`), such as a frame that a video
decoder made, or one that a camera or another API drew. The caller writes the same code on every device. A device
reports the kinds of image that it imports in place, without a copy (`prGpuDeviceImportsInPlace`): CVPixelBuffers
backed by IOSurfaces on Metal, and a web page's images, such as a VideoFrame, on a canvas device. For an image in place,
the device takes a reference of its own, and drops it once the texture is destroyed and the work that read it is done.
It then tells the importer, which may recycle the image. A device copies an image of any other kind into a new texture
if the build can read the image's texels, and tells the importer at once.

lib/media gives each decoded frame as an image to import (`PrGpuImport`). It is the backend's own image if the backend
has one, and otherwise the frame's pixels in memory. So a frame reaches the screen without a copy on a device that
imports the backend's images in place. Code that draws on the CPU reads an image's texels with `prGpuImportLock`,
which needs no device.

An imported image must hold what it shows when it is imported. A producer whose GPU work may still be writing the image
will pass a point on its own timeline for the device to wait for, as an event of the native API and a value, once a
producer needs it.

## Testing

`gpu_tests` runs every test on each backend that the build has and the system has a device for, with validation, and
tests each backend's embedding with the same shaders: recording into the application's command buffers and drawing in its
passes. It runs under Metal's API validation, which it turns on with `MTL_DEBUG_LAYER` unless the environment sets it,
and tests a window's swap chain on Vulkan and a view's on Metal. The renderers'
tests compare their images with the `cpu` backend's ([draw.md](draw.md)), on the backend that `PR_GPU_BACKEND` chooses.
`PR_D3D12_DEBUG=1` and `PR_VULKAN_DEBUG=1` turn on Direct3D 12's debug layer and Vulkan's validation layer for a device
that is made without asking for validation, and write their errors to the standard error.

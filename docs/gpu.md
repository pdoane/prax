# GPU

`lib/gpu` is the interface that every Prax renderer is written against: `lib/draw`'s device backends, canvas, the
examples and the tools. A renderer is written once, in C++ against `pr/gpu.h` and in Slang against `pr/gpu.slang`, and
each backend runs it on one graphics API.

| backend | API | on by default |
| --- | --- | --- |
| `metal` | Metal 3, the default on macOS | macOS (`PR_GPU_METAL`) |
| `metal4` | Metal 4, kept for experiments | macOS (`PR_GPU_METAL4`) |
| `d3d12` | Direct3D 12, shader model 6.6, resource binding tier 3, enhanced barriers | Windows (`PR_GPU_D3D12`) |
| `d3d11` | Direct3D 11.1 on feature level 11_0, with slots and no bindless | Windows (`PR_GPU_D3D11`) |
| `vulkan` | Vulkan 1.3, with mutable descriptors and push descriptors | Linux and Android (`PR_GPU_VULKAN`) |
| `webgpu` | the page's WebGPU, with slots and no bindless | the web (`PR_GPU_WEBGPU`) |
| `memory` | memory, with no shaders, for renderers that draw on the CPU | every platform (`PR_GPU_MEMORY`) |
| `canvas2d` | memory and a web page's canvases, with no shaders | the web (`PR_GPU_CANVAS2D`) |

The library holds the backends that the build turns on. It builds with none of them for an application that brings
its own (`pr/gpu_backend.h`). `prGpuDeviceCreate` creates a device by a backend, or by `PrGpuBackend_Default`. Default
is the backend that the environment variable `PR_BACKEND_GPU` names, such as `metal` or `vulkan`, so a program and its
tests run on any backend in the build. Otherwise Default tries the build's backends in order, the platform's own first
and the memory backend last, and passes over a backend once a creation by it fails with NotSupported. A creation that
fails on a pool leaves a lost device, so the application's recovery from the loss creates the next backend's device.
A backend's header creates a device over the application's native device and queue (`pr/gpu_metal.h`,
`pr/gpu_metal4.h`, `pr/gpu_d3d12.h`, `pr/gpu_vulkan.h`).

## The model

The interface is bindless, with explicit barriers, because Metal, Direct3D 12 and Vulkan all work best that way and
a 3D renderer needs it.

- **Handles are heap slots.** A buffer, a texture or a sampler is a `uint32_t` that is its identity and its slot in
  the device's heap. Creating a resource makes it visible to shaders, and there is no bind step. A shader reads a
  handle from any data that it can read, including another resource, and turns it into the resource with
  `prGpuGet<T>`. A resource that shaders write may have a second handle for writing (`prGpuBufferStorageHandle`,
  `prGpuTextureStorageHandle`), because Direct3D 12 writes through a view of its own.
- **Root data** is push data, at most 128 bytes, and uniforms, at most 64 KiB, set on the command buffer before a draw
  or dispatch. Both are copied, and handles are `uint32_t` fields of either.
- **Vertices** reach the vertex shader through the pipeline's vertex layout, which reads the pass's vertex buffers. A
  vertex shader may also pull its vertices from buffers by handle or slot.
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
- **Slots serve every device.** A device without `PrGpuFeatureFlag_Bindless`, such as Direct3D 11 or WebGPU, reaches
  resources only through slots: a few buffers and textures of each kind that the command buffer binds before a draw.
  A shader source declares its slots with literal numbers. Every entry point of a source has every slot that the
  source declares, and a draw binds them all, so entry points that should not share slots are separate sources.
- **Lifetimes need no thought.** A buffer, a texture and a pipeline are reference counted: each holder takes a
  reference of its own (`prGpuTextureRetain`), and the release of the last destroys the resource. A graph holds what it
  imports for the frame. So a library that holds a resource of its caller's needs no promise about when its caller is
  done with it. A resource may be released
  as soon as the work that uses it is recorded. The device keeps it until every command buffer that began before the
  destruction is done.
- **One timeline.** Each submission returns a fence, and fences rise in the order of submission. A command buffer that
  records into the application's native one is submitted through the device too, so the application never signals anything.
- **Uploads are unbounded and ordered.** `prGpuBufferUpload` and `prGpuTextureUpload` take any amount of
  data, which the device stages and copies in a command buffer of its own ahead of the next submission. Several
  uploads to one resource land as if copied in order. A texture upload reads rows that lie any distance apart, as an
  image in memory has them, because the device packs them as it stages them. A copy between a buffer and a texture
  has no such step, so its rows lie a whole number of blocks apart.
- **A texture is levels of layers.** A cube is six layers, the faces toward +x, -x, +y, -y, +z and -z, laid out as
  every API lays out a cube map. An upload, a copy, a barrier and an attachment each name the levels and layers that
  they touch, and a view (`prGpuTextureCreateView`) is a texture of its own over some of them: one face of one level
  as a 2D texture to read back, or one level of every face as a cube to sample while a pass draws to the next level.
- **The stream always succeeds.** A command buffer allocates per-frame data from a stream of upload memory that is
  reclaimed once its work completes, and grows as recording needs it. Running out of memory loses the device, which
  hosts already handle.
- **Creation is asynchronous.** `prGpuDeviceCreate` returns the device at once. The creation runs on a task pool that
  the application provides, or during the call without one. The device does no work until it is ready. A failed
  creation leaves the device lost, so an application recovers from it as from a loss.
- **Device loss** is a state, not an error path. A lost device refuses new work, completes every fence, and reads back
  zeros. The application destroys every object and makes them again on a new device.

## Formats

`PrGpuFormat` is one set for every device, and its values never change, because a texture's record stores its format
(`PrGpuTextureRecord`): a format joins the set at its end. Its formats that are not compressed are grouped by the bits
of a channel, each with its normalized, signed, integer, float and sRGB forms where the APIs have them, and each
swizzled format beside the format that stores it. Packed formats, depth and stencil follow, and then the families of
block-compressed formats. `tools/gen/gen_gpu.py` generates each format's properties (`lib/gpu/src/gpu_formats.h`)
from a list in the enumeration's order, which it checks against pr/gpu.h.

A device has the formats that its API gives it. A backend maps the formats that it has to its API's and refuses the
rest, and `prGpuDeviceSupportsFormat` answers for a format and its uses, so a format of the set that no backend has yet
is refused everywhere until one does.

## Compressed formats

`PrGpuFormat` holds every block-compressed format that the graphics APIs share, in families that a device has as a
whole: a feature flag reports each, and `prGpuDeviceSupportsFormat` answers for one format from what the device
itself reports. A compressed format is only sampled.

| family | formats | feature |
| --- | --- | --- |
| BC | BC1, BC2, BC3 and BC7, each with an sRGB form; BC4, BC5 and BC6H, unsigned and signed | `TextureCompressionBc` |
| ETC2 | ETC2 RGB8, RGB8A1 and RGBA8, each with an sRGB form; EAC R11 and RG11, unsigned and signed | `TextureCompressionEtc2` |
| ASTC | the fourteen block sizes from 4x4 to 12x12, each with an sRGB form | `TextureCompressionAstc` |
| ASTC HDR | the same block sizes, read as floats | `TextureCompressionAstcHdr` |

| backend | BC | ETC2 | ASTC | ASTC HDR |
| --- | --- | --- | --- | --- |
| Metal | where the device says (`supportsBCTextureCompression`): every Mac, and newer Apple GPUs elsewhere | an Apple GPU | an Apple GPU from the A8 | an Apple GPU from the A13 |
| Direct3D 12 and 11 | always | never | never | never |
| Vulkan | `textureCompressionBC` | `textureCompressionETC2` | `textureCompressionASTC_LDR` | `textureCompressionASTC_HDR`; each format is then asked of the physical device |
| WebGPU | `texture-compression-bc` | `texture-compression-etc2` | `texture-compression-astc` | never: WebGPU has no such formats |
| memory, Canvas 2D | never | never | never | never |

A texture's first level is whole blocks, because Direct3D and WebGPU take nothing else, and a device refuses any other
extent as NotSupported. A level of blocks is rows of whole blocks, and the last blocks of a smaller level reach past
it where its extent is not a multiple of the block's. `prGpuFormatBlock` gives a format's block, and
`prGpuFormatRowBytes`, `prGpuFormatRows` and `prGpuFormatLevelBytes` size a level's rows, so that an upload, a copy
and a file's level are sized in one place. An upload's rows may lie any distance apart, and a copy's lie a whole
number of blocks apart.

## Swizzled formats

A swizzled format stores its texels in another format and is sampled with its channels placed otherwise, by the
hardware: Alpha (0, 0, 0, R), Luminance (R, R, R, 1), Intensity (R, R, R, R) and Mask (1, 1, 1, R) of R8, R16, R16F,
R32F, BC4 and EAC R11, and LuminanceAlpha (R, R, R, G) of Rg8, Rg16, Rg16F, Rg32F, BC5 and EAC RG11. A format of one
or two channels then serves where a texture of gray, or of gray and alpha, would otherwise take four. They are
optional, a device's feature (`TextureSwizzle`), and no backend emulates them in a shader, so that a shader reads a
texture the same way on every device: a device without the feature refuses them, and a caller chooses a format of four
channels there. `prGpuFormatSwizzle` and `prGpuFormatStored` take a swizzled format apart, and `prGpuFormatSwizzled`
puts one together. A swizzled format is of the family and the block of the format that it stores, and is only sampled.

| backend | swizzled formats |
| --- | --- |
| Metal and Metal 4 | always: the texture's swizzle (`MTLTextureSwizzleChannels`), which its views keep |
| Vulkan | always: the image view's component mapping |
| Direct3D 12 | always: the shader resource view's component mapping |
| Direct3D 11, WebGPU, memory, Canvas 2D | never |

## Texture records

A texture's description also packs into 64 bits with no pointer (`PrGpuTextureRecord`): its layout, kind, format, extent
and levels, to store ahead of the texture's bytes, in a file or in a table that says what every texture of a scene is
before any is read. `prGpuTextureRecordBytes` gives the bytes of its levels in a layout that a file stores. `lib/gpu`
knows nothing of image files.

## Shaders

Shaders are Slang. `cmake/shader_library.cmake` compiles each source at build time into one library per backend that the
platform builds, which a header embeds (`pr_slang` in `cmake/build.cmake`). The library names each entry point, its stage
and, for compute, its group size, which the reflection gives, so a dispatch and its shader cannot disagree. Metal and
SPIR-V hold every entry point in one module. Direct3D 12 takes DXIL for each entry point on its own, and Direct3D 11
takes DXBC of shader model 5.0. An entry point that reaches a resource by handle has no DXBC, so a Direct3D 11 device
refuses a pipeline of it, as a WebGPU device does.

A device without bindless runs only the entry points that reach their resources through slots. A renderer that
reaches resources by handle where it can compiles its source a second time for such devices, with a macro of its own
(`DEFINES` of `pr_slang`) that turns its handles into slots, as `lib/draw`'s fragment renderer does.
`PrGpuDeviceOptions::slotsOnly` creates such a device on a system that has bindless, on Metal, Metal 4 and Vulkan, so
that a renderer's slot path runs and is tested on any of them. The device refuses a pipeline whose entry point reaches
a resource by handle, as a device that lacks bindless does.

`pr/gpu.slang` is the shaders' side of the interface. The source compiles once per backend with `PR_GPU_METAL`,
`PR_GPU_VULKAN`, `PR_GPU_D3D12`, `PR_GPU_D3D11` or `PR_GPU_WEBGPU`, and the header hides what differs:

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

A swap chain presents to an application's surface (`pr/surface.h`), and its device chooses the format and color space of
its textures for what the swap chain shows. SDR is 8-bit sRGB everywhere. For HDR, Metal presents scRGB in half
floats, Direct3D 12 presents HDR10 to a display in HDR mode and linear scRGB elsewhere, and Vulkan presents linear scRGB
where the surface takes it and HDR10 where it takes that. An application converts its colors for what
`prGpuSwapChainInfo` reports, as the examples' presenter does.

A frame (`prGpuSwapChainAcquire`) is a render target that is only drawn to, so that the compositor reads it fastest.
A capture (`prGpuSwapChainRequestCapture`) draws the next frame into a texture of the swap chain's own instead, which
the frame's present reads back and gives to the capture's function in the swap chain's format, and does not show that
frame. `prGpuCaptureToRgba8` converts the texels to 8-bit RGBA.

A swap chain's sync says when its frames reach the display (`PrGpuSwapChainSync`): each in turn at a vertical blank,
the newest at a vertical blank, replacing a frame that waits, or at once, tearing. A swap chain reports the mode that it
has (`PrGpuSwapChainInfo::sync`), which is vsync where the platform lacks the one asked for.

| backend | vsync | latest | immediate |
| --- | --- | --- | --- |
| Metal and Metal 4 | always | never | on the Mac: the layer's `displaySyncEnabled` off |
| Vulkan | `FIFO` | `MAILBOX`, where the surface has it | `IMMEDIATE`, where the surface has it |
| Direct3D 12 and 11 | a sync interval of one | a sync interval of zero | tearing allowed, where the system allows it, to a window's own swap chain; latest elsewhere |
| WebGPU, Canvas 2D | always | never | never |

Without `PrGpuSwapChainFlag_Threaded`, a frame presents within the window's display transaction, so a window that is
resized shows each frame at its size. Anchored at the top left, a Direct3D 12 swap chain's buffers only grow, so that a
resize within them waits for no work and allocates nothing.

## Device memory

A device takes any number of buffers and textures, up to its handles. Vulkan limits a device's memory allocations, to
4096 on many systems, so the Vulkan backend does not allocate for each resource. It allocates blocks and places
resources in them (`vulkan_memory.h`, and `gpu_shared_ranges.h` for the bookkeeping, which any backend that places
resources can share).

- **Pools by memory type and content.** Buffers, the buffers that the CPU maps, and images each have their own blocks.
  A buffer therefore never shares a page with an image, which `bufferImageGranularity` forbids, and only the blocks
  of mapped buffers are mapped.
- **Blocks grow with the pool**, from 8 MiB to 64 MiB, so a small application holds little and a large scene makes few
  allocations.
- **Some resources stand alone.** A resource larger than a quarter of the largest block has an allocation of its own.
  So has one that the device requires an allocation for, and one that it prefers an allocation for while the device's
  allocations are few. The stream's chunks and the staging's blocks stand alone too: the device keeps them, and in a
  pool they would hold a scene's blocks after the scene is gone.
- **Free ranges are found by size class**, in two levels of bins, so the cost of placing a resource does not grow with
  the number of resources. A freed range merges with its free neighbors at once.
- **Blocks go back.** A block that holds nothing is freed, except the smallest such block of each pool, which is kept
  so that a resource made and destroyed repeatedly does not allocate each time.
- **Mapped memory is mapped by the block**, once, because Vulkan maps an allocation only once.
- **A frame allocates nothing.** Recording and submitting use the stream and the staging, which keep their memory.

`prGpuVulkanMemoryInfo` reports the allocations, their bytes and the bytes in use. Direct3D 12 and Metal create each
resource with memory of its own, which those APIs do not limit by count.

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

## Render graph

The device tracks no states, so something above it must know each resource's accesses. A render graph
(`pr/gpu_graph.h`) is that knowledge for one frame. A frame declares its passes and the resources that each one uses,
and the graph records them into a command buffer. It is optional: a renderer that knows its own accesses records its
barriers itself, as `lib/draw` does. `lib/render` adds the passes of its views to a graph that the application builds
([render.md](render.md#building-a-frame)).

- **A frame builds its graph again each time**, in plain code. The graph keeps nothing from one frame to the next
  except the device textures that it lends. Callbacks are C functions with a context, and the graph gives out memory
  that lives for the frame, so that a pass keeps its context there.
- **The graph holds the resources whose accesses change during a frame.** A frame imports its own textures and buffers
  with the accesses that they are in, and names the accesses to leave each result in. A resource that every pass only
  reads is bound without the graph.
- **A texture of the frame has no device texture until the graph records.** The frame describes it, and the graph
  lends it a device texture from the first recorded pass that uses it to the last. A pass asks for the device texture
  in its callback.
- **Results decide what is recorded.** A pass is recorded if it writes a resource that is left, if a recorded pass
  uses what it wrote or follows it, or if it declares no write, because its effects are then unknown to the graph.
  Every other pass is skipped.
- **Barriers are recorded just before the pass that needs them**, never earlier. A texture is tracked as a whole. A
  buffer is tracked by byte ranges: writes of ranges that do not overlap, in the same accesses, need no barrier
  between them. The graph records no barrier inside one pass or one work.
- **Fewer barriers and less memory conflict, and a priority chooses.** A device texture keeps its accesses from frame
  to frame, and returns to the graph after the last pass that uses it. Favoring barriers, a frame's texture takes a
  free device texture that is already in the accesses of its first use, so a texture that every frame uses alike needs
  no barrier, and the graph makes another device texture rather than disturb one that is left in a write. Favoring
  memory, a frame's texture takes any free device texture of its description and pays the barrier. Under either
  priority, a device texture that was left in a read is taken before a new one is made: it matches no first use, and a
  new texture would need the same barrier.
- **Dependencies come from the declared uses.** Of two passes that use the same bytes of a resource, where either one
  writes them, the later added follows the earlier. A pass may also be declared to follow another, for an effect that
  the graph cannot see. The recorded order keeps every dependency.
- **Passes are recorded in the order that they were added.** That order already keeps the dependencies that the uses
  make. A pass that is declared to follow a later added pass is recorded after it: each pass recorded is the earliest
  added whose dependencies are all recorded. So the order is deterministic and depends only on the frame's graph, and
  the frame that builds the graph decides which passes sit next to each other.
- **Misuse is reported, not fatal.** The graph lists what it finds: a pass that reads and writes one resource, a read
  of a frame's texture that no pass has written, a cycle of passes that follow each other, and a texture, buffer or
  pass of an earlier frame. It records what it can, and the application reads the list.
- **A lost device is safe.** The graph then lends no texture, and it does not record a pass that uses a resource
  without a device object, so no callback sees a zero texture that it declared.

## Testing

`gpu_tests` runs every test on each backend that the build has and the system has a device for, with validation, and
again on a device of slots only where the backend creates one. It uploads and copies blocks of every compressed format
that a device has, samples them, and compares each texel with what the format's specification decodes. It tests each
backend's embedding with the same shaders: recording into the application's command buffers and drawing in its
passes. It runs under Metal's API validation, which it turns on with `MTL_DEBUG_LAYER` unless the environment sets it,
and tests a window's swap chain on Vulkan and a view's on Metal. The renderers'
tests compare their images with the `cpu` backend's ([draw.md](draw.md)), on the backend that `PR_BACKEND_GPU` chooses.
On the web, `gpu_page_tests` runs the same tests in a page, on its WebGPU (`./dev.sh test wasm`), and any error of
WebGPU's validation fails them. A page test waits for the device as a test does anywhere, because its program suspends
while the page works (JSPI, `pr_page_tests`).
`PR_D3D12_DEBUG=1` and `PR_VULKAN_DEBUG=1` turn on Direct3D 12's debug layer and Vulkan's validation layer for a device
that is made without asking for validation, and write their errors to the standard error.

// Memory devices: buffers and textures in memory, and command buffers whose work runs when they are submitted.

#include "gpu/backends/shared/src/gpu_shared_memory.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/color_space.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"

namespace pr_impl {

namespace {

// The functions of the device's command buffers and passes, which their creation installs.
extern const PrGpuCommands kCommandsFunctions;
extern const PrGpuPass kPassFunctions;

constexpr uint32_t kMaxTextureSize = 16384;
constexpr uint8_t kMaxLevels = 15;

// A buffer's bytes, and the description that it was created with, without its name.
struct Buffer {
    PrGpuBufferDesc desc;
    uint8_t* PR_NULLABLE bytes;     // null for a free slot
};

// Which side of a texture that has a copy (GpuMemoryMirror) holds its contents.
enum {
    Held_Both,      // both sides hold the contents
    Held_Texels,    // the texels hold the contents, and the copy is stale
    Held_Mirror,    // the copy holds the contents, and the texels are stale
    Held_Cleared,   // both sides are stale, and the contents are the color `cleared`
};
typedef uint8_t Held;

// The texels of a texture and of its views, each of which holds a reference. The layers, or depth slices, of each level
// follow one another `sliceBytes` apart, with their rows `rowBytes` apart. A texture over the application's pixels has
// one level, whose rows are the pixels' own, and does not own them. For a texture that has a copy, `held` says which
// side holds its contents.
struct Memory {
    uint32_t references;
    bool owned;
    Held held;
    uint32_t mirror;                    // zero for none
    PrColor cleared;                    // the contents while `held` is Held_Cleared
    PrGpuReleaseFn PR_NULLABLE release; // called once an imported texture's memory is freed
    void* PR_NULLABLE releaseContext;
    PrGpuTextureDesc desc;
    uint8_t* bytes;
    size_t levelOffsets[kMaxLevels];
    size_t rowBytes[kMaxLevels];
    size_t sliceBytes[kMaxLevels];
};

// A texture or a view: the memory that it covers from `firstLevel` and `firstLayer`, and the description that it
// reports.
struct Texture {
    Memory* PR_NULLABLE memory;     // null for a free slot
    PrGpuTextureDesc desc;
    uint8_t firstLevel;
    uint16_t firstLayer;
};

// The work that a command buffer records, which runs when the command buffer is submitted.
enum {
    OpKind_CopyBuffer,
    OpKind_CopyTexture,
    OpKind_CopyBufferToTexture,
    OpKind_CopyTextureToBuffer,
    OpKind_Clear,
    OpKind_NativeDraw,
    OpKind_Call,
    OpKind_Done,
};
typedef uint8_t OpKind;

struct Op {
    OpKind kind;
    PrGpuBuffer buffer;
    PrGpuBuffer target;
    PrGpuTexture texture;
    PrGpuTexture targetTexture;
    uint64_t offset;
    uint64_t targetOffset;
    uint64_t size;
    uint32_t rowBytes;
    uint32_t layerBytes;
    PrGpuTextureRegion region;
    PrGpuTextureRegion targetRegion;
    PrColor color;                                       // Clear
    PrGpuNativeKind nativeKind;                          // NativeDraw
    PrGpuNativeDrawFn PR_NULLABLE draw;                  // NativeDraw
    void (*PR_NULLABLE call)(void* PR_NULLABLE context); // Call
    PrGpuDoneFn PR_NULLABLE done;                        // Done
    void* PR_NULLABLE context;                           // NativeDraw, Call and Done
};

struct Commands : GpuSharedCommands {
    uint64_t serial;                   // the order in which the command buffers began
    RawArray<Op> ops;
    RawArray<PrGpuBuffer> streams;     // the stream's buffers, freed once the work has run
};

// A pass, and its first color attachment, which native draws draw into.
struct Pass : GpuSharedPass {
    PrGpuTexture texture;
    uint8_t level;
    uint16_t layer;
};

// A buffer or texture that was destroyed while command buffers that may name it were open. It is freed once every
// command buffer that began at or before `serial` has run.
struct Retired {
    uint64_t serial;
    uint32_t id;
    bool texture;
};

// The device. Any thread may change the tables of buffers and textures, the open command buffers, and the list of what
// waits to be freed, so `mutex` guards them.
struct Device : GpuSharedDevice {
    PrMutex mutex;
    const GpuMemoryMirror* PR_NULLABLE mirror;
    RawArray<Buffer> buffers;          // at each buffer's id less one
    RawArray<uint32_t> freeBuffers;
    RawArray<Texture> textures;        // at each texture's id less one
    RawArray<uint32_t> freeTextures;
    uint32_t samplers;
    PrGpuFence submitted;
    uint64_t serial;                   // the serial of the command buffer that began last
    RawArray<uint64_t> open;           // the serials of the command buffers that are not submitted yet
    RawArray<Retired> retired;
    bool lost;
};

Device* deviceOf(PrGpuDevice* device) {
    return static_cast<Device*>(device);
}

Device* deviceOf(const PrGpuDevice* device) {
    return const_cast<Device*>(static_cast<const Device*>(device));
}

// Returns true if the device holds textures of `format`.
bool formatHeld(PrGpuFormat format) {
    switch (format) {
        case PrGpuFormat_R8:
        case PrGpuFormat_Rg8:
        case PrGpuFormat_Rgba8:
        case PrGpuFormat_Rgba8Srgb:
        case PrGpuFormat_Bgra8:
        case PrGpuFormat_Bgra8Srgb:
        case PrGpuFormat_R16F:
        case PrGpuFormat_Rg16F:
        case PrGpuFormat_Rgba16F:
        case PrGpuFormat_R32F:
        case PrGpuFormat_Rg32F:
        case PrGpuFormat_Rgba32F:
            return true;
        default:
            return false;
    }
}

uint32_t texelBytes(PrGpuFormat format) {
    return prGpuFormatBlock(format).bytes;
}

uint32_t levelSize(uint32_t size, uint32_t level) {
    uint32_t scaled = size >> level;
    return scaled != 0 ? scaled : 1;
}

// The layers of a level, or its depth slices for a 3D texture.
uint32_t levelSlices(const PrGpuTextureDesc& desc, uint32_t level) {
    return desc.kind == PrGpuTextureKind_3d ? levelSize(desc.extent.depth, level) : desc.extent.depth;
}

Texture& textureAt(Device* d, PrGpuTexture texture) {
    PR_ASSERT(texture.id != 0 && texture.id <= d->textures.count);
    return d->textures.data[texture.id - 1];
}

Buffer& bufferAt(Device* d, PrGpuBuffer buffer) {
    PR_ASSERT(buffer.id != 0 && buffer.id <= d->buffers.count);
    return d->buffers.data[buffer.id - 1];
}

// Returns a copy of the texture's entry, which the caller reads without the lock.
Texture textureOf(Device* d, PrGpuTexture texture) {
    prMutexLock(&d->mutex);
    Texture t = textureAt(d, texture);
    prMutexUnlock(&d->mutex);
    return t;
}

// Returns the first byte of row `y` of slice `slice` at `level` of the texture, in the texture's own terms.
uint8_t* texelRow(const Texture& t, uint32_t level, uint32_t slice, uint32_t y) {
    const Memory& m = *t.memory;
    uint32_t memoryLevel = t.firstLevel + level;
    uint32_t memorySlice = m.desc.kind == PrGpuTextureKind_3d ? slice : t.firstLayer + slice;
    return m.bytes + m.levelOffsets[memoryLevel] + memorySlice * m.sliceBytes[memoryLevel] +
        y * m.rowBytes[memoryLevel];
}

// Returns the pixels of one level and slice of the texture.
PrPixels pixelsOf(const Texture& t, uint32_t level, uint32_t slice) {
    return PrPixels{
        .data = texelRow(t, level, slice, 0),
        .stride = t.memory->rowBytes[t.firstLevel + level],
        .extent = {levelSize(t.desc.extent.width, level), levelSize(t.desc.extent.height, level)},
    };
}

// The first slice of a region: its depth offset in a 3D texture, and its first layer in any other.
uint32_t firstSlice(const Texture& t, const PrGpuTextureRegion& region) {
    return t.memory->desc.kind == PrGpuTextureKind_3d ? region.z : region.firstLayer;
}

// Copies the texels of `region` of `t` to or from rows `rowBytes` apart and slices `layerBytes` apart. `toTexture` says
// which direction. A zero `rowBytes` or `layerBytes` lays out the rows or slices tightly.
void copyRegion(const Texture& t, const PrGpuTextureRegion& region, uint8_t* other, uint32_t rowBytes,
        uint32_t layerBytes, bool toTexture) {
    size_t width = size_t(region.extent.width) * texelBytes(t.desc.format);
    size_t rows = rowBytes != 0 ? rowBytes : width;
    size_t slices = layerBytes != 0 ? layerBytes : rows * region.extent.height;
    uint32_t first = firstSlice(t, region);
    for (uint32_t s = 0; s < region.extent.depth; ++s) {
        for (uint32_t y = 0; y < region.extent.height; ++y) {
            uint8_t* texels = texelRow(t, region.level, first + s, region.y + y) +
                size_t(region.x) * texelBytes(t.desc.format);
            uint8_t* bytes = other + s * slices + y * rows;
            if (toTexture)
                prMemCopy(texels, bytes, width);
            else
                prMemCopy(bytes, texels, width);
        }
    }
}

// Writes `color` as one texel of `format`. A Srgb format holds the sRGB encoding of the linear color.
void encodeTexel(PrGpuFormat format, PrColor color, uint8_t* out) {
    float channels[4] = {color.r, color.g, color.b, color.a};
    if (format == PrGpuFormat_Rgba8Srgb || format == PrGpuFormat_Bgra8Srgb) {
        for (int c = 0; c < 3; ++c)
            channels[c] = prColorTransferEncode(PrColorTransfer_Srgb, channels[c]);
    }
    if (format == PrGpuFormat_Bgra8 || format == PrGpuFormat_Bgra8Srgb) {
        float red = channels[0];
        channels[0] = channels[2];
        channels[2] = red;
    }
    uint32_t count = 4;
    if (format == PrGpuFormat_R8 || format == PrGpuFormat_R16F || format == PrGpuFormat_R32F)
        count = 1;
    else if (format == PrGpuFormat_Rg8 || format == PrGpuFormat_Rg16F || format == PrGpuFormat_Rg32F)
        count = 2;
    for (uint32_t c = 0; c < count; ++c) {
        float value = channels[c];
        switch (format) {
            case PrGpuFormat_R16F:
            case PrGpuFormat_Rg16F:
            case PrGpuFormat_Rgba16F: {
                PrFloat16 half = prFloat16FromFloat(value);
                prMemCopy(out + c * 2, &half, 2);
                break;
            }
            case PrGpuFormat_R32F:
            case PrGpuFormat_Rg32F:
            case PrGpuFormat_Rgba32F:
                prMemCopy(out + c * 4, &value, 4);
                break;
            default: {
                float clamped = value < 0 ? 0 : value > 1 ? 1 : value;
                out[c] = uint8_t(clamped * 255 + 0.5f);
                break;
            }
        }
    }
}

// Fills one level and layer of the texture with `color`.
void clearTexture(const Texture& t, uint8_t level, uint16_t layer, PrColor color) {
    uint8_t texel[16];
    encodeTexel(t.desc.format, color, texel);
    uint32_t bytes = texelBytes(t.desc.format);
    PrPixels pixels = pixelsOf(t, level, layer);
    for (uint32_t y = 0; y < pixels.extent.y; ++y) {
        uint8_t* row = pixels.data + y * pixels.stride;
        for (uint32_t x = 0; x < pixels.extent.x; ++x)
            prMemCopy(row + size_t(x) * bytes, texel, bytes);
    }
}

// Returns true if a copy may hold the memory's texels.
bool mirrorable(const Device* d, const Memory& m) {
    return d->mirror != nullptr && m.desc.kind == PrGpuTextureKind_2d && m.desc.levels == 1 && m.desc.extent.depth == 1 &&
        d->mirror->holds(m.desc.format);
}

// Returns the pixels of the memory's first level and layer.
PrPixels memoryPixels(const Memory& m) {
    return PrPixels{.data = m.bytes, .stride = m.rowBytes[0], .extent = {m.desc.extent.width, m.desc.extent.height}};
}

// Brings the memory's texels up to date, for work on the texels.
void syncTexels(Device* d, Memory& m) {
    if (m.held == Held_Mirror) {
        d->mirror->read(m.mirror, memoryPixels(m), m.desc.format);
        m.held = Held_Both;
    } else if (m.held == Held_Cleared) {
        Texture whole{.memory = &m, .desc = m.desc};
        clearTexture(whole, 0, 0, m.cleared);
        m.held = Held_Texels;
    }
}

// Records that work on the texels changed them.
void wroteTexels(Memory& m) {
    if (m.mirror != 0)
        m.held = Held_Texels;
}

// Brings the memory's copy up to date for work through the copy, and makes the copy if there is none. Returns the copy,
// or zero if no copy can hold the memory.
uint32_t syncMirror(Device* d, Memory& m) {
    if (m.mirror == 0) {
        if (!mirrorable(d, m))
            return 0;
        m.mirror = d->mirror->create(PrUVec2{m.desc.extent.width, m.desc.extent.height});
        if (m.mirror == 0)
            return 0;
        m.held = Held_Texels;
    }
    if (m.held == Held_Texels) {
        d->mirror->write(m.mirror, prPixelsView(memoryPixels(m)), m.desc.format);
        m.held = Held_Both;
    } else if (m.held == Held_Cleared) {
        d->mirror->clear(m.mirror, m.cleared, m.desc.format);
        m.held = Held_Mirror;
    }
    return m.mirror;
}

// Returns the memory that a texture covers, which its work runs on.
Memory& memoryAt(Device* d, PrGpuTexture texture) {
    return *textureAt(d, texture).memory;
}

// Returns the texture, with its texels up to date for work on the texels that may change them.
Texture& writableTexture(Device* d, PrGpuTexture texture) {
    Texture& t = textureAt(d, texture);
    syncTexels(d, *t.memory);
    wroteTexels(*t.memory);
    return t;
}

// Returns the texture, with its texels up to date for work on the texels that only reads them.
Texture& readableTexture(Device* d, PrGpuTexture texture) {
    Texture& t = textureAt(d, texture);
    syncTexels(d, *t.memory);
    return t;
}

// Runs one step of submitted work. The caller holds the mutex, which the step releases while it calls out, so that the
// callee may call the device.
void run(Device* d, const Op& op) {
    switch (op.kind) {
        case OpKind_CopyBuffer:
            prMemMove(bufferAt(d, op.target).bytes + op.targetOffset, bufferAt(d, op.buffer).bytes + op.offset,
                size_t(op.size));
            break;
        case OpKind_CopyTexture: {
            Texture& from = readableTexture(d, op.texture);
            Texture& to = writableTexture(d, op.targetTexture);
            size_t width = size_t(op.region.extent.width) * texelBytes(from.desc.format);
            uint32_t fromFirst = firstSlice(from, op.region);
            uint32_t toFirst = firstSlice(to, op.targetRegion);
            for (uint32_t s = 0; s < op.region.extent.depth; ++s) {
                for (uint32_t y = 0; y < op.region.extent.height; ++y) {
                    uint8_t* into = texelRow(to, op.targetRegion.level, toFirst + s, op.targetRegion.y + y) +
                        size_t(op.targetRegion.x) * texelBytes(to.desc.format);
                    const uint8_t* source = texelRow(from, op.region.level, fromFirst + s, op.region.y + y) +
                        size_t(op.region.x) * texelBytes(from.desc.format);
                    prMemMove(into, source, width);
                }
            }
            break;
        }
        case OpKind_CopyBufferToTexture:
            copyRegion(writableTexture(d, op.targetTexture), op.targetRegion, bufferAt(d, op.buffer).bytes + op.offset,
                op.rowBytes, op.layerBytes, true);
            break;
        case OpKind_CopyTextureToBuffer:
            copyRegion(readableTexture(d, op.texture), op.region, bufferAt(d, op.target).bytes + op.targetOffset,
                op.rowBytes, op.layerBytes, false);
            break;
        case OpKind_Clear: {
            // A texture that has a copy is cleared on the side that is read first.
            Memory& m = memoryAt(d, op.texture);
            if (m.mirror != 0) {
                m.held = Held_Cleared;
                m.cleared = op.color;
            } else {
                clearTexture(textureAt(d, op.texture), op.region.level, op.region.firstLayer, op.color);
            }
            break;
        }
        case OpKind_NativeDraw: {
            if (op.nativeKind == PrGpuNativeKind_Pixels) {
                Texture t = writableTexture(d, op.texture);
                PrGpuNativePixels target{pixelsOf(t, op.region.level, op.region.firstLayer), t.desc.format};
                prMutexUnlock(&d->mutex);
                op.draw(op.context, &target);
                prMutexLock(&d->mutex);
                break;
            }
            Memory& m = memoryAt(d, op.texture);
            uint32_t mirror = syncMirror(d, m);
            PR_ASSERT(mirror != 0 && "a native draw's attachment is a texture that a copy holds");
            m.held = Held_Mirror;
            prMutexUnlock(&d->mutex);
            op.draw(op.context, &mirror);
            prMutexLock(&d->mutex);
            break;
        }
        case OpKind_Call:
            prMutexUnlock(&d->mutex);
            op.call(op.context);
            prMutexLock(&d->mutex);
            break;
        case OpKind_Done:
            prMutexUnlock(&d->mutex);
            op.done(op.context, 0);
            prMutexLock(&d->mutex);
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The device

void destroy(PrGpuDevice* device) {
    Device* d = deviceOf(device);
    gpuSharedDeviceReset(d);
    PR_ASSERT(d->open.count == 0 && d->retired.count == 0);
    PR_ASSERT(d->buffers.count == d->freeBuffers.count && d->textures.count == d->freeTextures.count);
    d->open.reset();
    d->retired.reset();
    d->buffers.reset();
    d->freeBuffers.reset();
    d->textures.reset();
    d->freeTextures.reset();
    prMutexReset(&d->mutex);
    prMemoryFree(d);
}

bool isLost(const PrGpuDevice* device) {
    return deviceOf(device)->lost;
}

void simulateLoss(PrGpuDevice* device) {
    deviceOf(device)->lost = true;
}

// Work runs when it is submitted, so every fence that a submission returned has been reached.
bool isComplete(const PrGpuDevice*, PrGpuFence) {
    return true;
}

void wait(PrGpuDevice*, PrGpuFence) {}

void waitIdle(PrGpuDevice*) {}

bool supportsFormat(const PrGpuDevice*, PrGpuFormat format, PrGpuTextureUsageFlags usage) {
    constexpr PrGpuTextureUsageFlags kHeld = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget;
    return formatHeld(format) && (usage & ~kHeld) == 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Buffers

PrStatus bufferCreate(PrGpuDevice* device, const PrGpuBufferDesc* desc, PrGpuBuffer* out) {
    Device* d = deviceOf(device);
    uint8_t* bytes = static_cast<uint8_t*>(prMemoryAllocZeroed(desc->size != 0 ? size_t(desc->size) : 1));
    PrGpuBufferDesc kept = *desc;
    kept.name = PrStringView{};
    prMutexLock(&d->mutex);
    uint32_t id = 0;
    if (d->freeBuffers.count != 0) {
        id = d->freeBuffers.last();
        d->freeBuffers.pop();
    } else {
        d->buffers.push(Buffer{});
        id = d->buffers.count;
    }
    d->buffers.data[id - 1] = Buffer{.desc = kept, .bytes = bytes};
    prMutexUnlock(&d->mutex);
    *out = PrGpuBuffer{id};
    return PrStatus_Ok;
}

// Frees a buffer's bytes and its slot. The caller holds the mutex.
void freeBuffer(Device* d, uint32_t id) {
    Buffer& b = d->buffers.data[id - 1];
    prMemoryFree(b.bytes);
    b = Buffer{};
    d->freeBuffers.push(id);
}

// Frees a texture's slot, and its memory once no view holds it. The caller holds the mutex.
void freeTexture(Device* d, uint32_t id) {
    Texture& t = d->textures.data[id - 1];
    Memory* memory = t.memory;
    t = Texture{};
    d->freeTextures.push(id);
    if (--memory->references != 0)
        return;
    if (memory->mirror != 0)
        d->mirror->destroy(memory->mirror);
    if (memory->release != nullptr)
        memory->release(memory->releaseContext);
    if (memory->owned)
        prMemoryFree(memory->bytes);
    prMemoryFree(memory);
}

// Frees a destroyed buffer or texture: at once if no command buffer is open, or else once the open ones have run. The
// caller holds the mutex.
void retire(Device* d, uint32_t id, bool texture) {
    if (d->open.count != 0) {
        d->retired.push(Retired{d->serial, id, texture});
    } else if (texture) {
        freeTexture(d, id);
    } else {
        freeBuffer(d, id);
    }
}

// Frees what was waiting for command buffers that have all run. The caller holds the mutex.
void collect(Device* d) {
    uint64_t oldest = UINT64_MAX;
    for (uint64_t serial : d->open.span())
        oldest = serial < oldest ? serial : oldest;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < d->retired.count; ++i) {
        Retired r = d->retired.data[i];
        if (r.serial >= oldest)
            d->retired.data[kept++] = r;
        else if (r.texture)
            freeTexture(d, r.id);
        else
            freeBuffer(d, r.id);
    }
    d->retired.count = kept;
}

void bufferDestroy(PrGpuDevice* device, PrGpuBuffer buffer) {
    Device* d = deviceOf(device);
    prMutexLock(&d->mutex);
    PR_ASSERT(bufferAt(d, buffer).bytes != nullptr);
    retire(d, buffer.id, false);
    prMutexUnlock(&d->mutex);
}

PrGpuBufferDesc bufferDesc(const PrGpuDevice* device, PrGpuBuffer buffer) {
    Device* d = deviceOf(device);
    prMutexLock(&d->mutex);
    PrGpuBufferDesc desc = bufferAt(d, buffer).desc;
    prMutexUnlock(&d->mutex);
    return desc;
}

// No shader reads or writes a buffer, so its handle is its id.
uint32_t bufferStorageHandle(const PrGpuDevice*, PrGpuBuffer buffer) {
    return buffer.id;
}

void* bufferMap(const PrGpuDevice* device, PrGpuBuffer buffer) {
    Device* d = deviceOf(device);
    prMutexLock(&d->mutex);
    void* bytes = bufferAt(d, buffer).bytes;
    prMutexUnlock(&d->mutex);
    return bytes;
}

void bufferUpload(PrGpuDevice* device, PrGpuBuffer buffer, uint64_t offset, const void* data, uint64_t size) {
    prMemCopy(static_cast<uint8_t*>(bufferMap(device, buffer)) + offset, data, size_t(size));
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures

// Adds a texture or view to the table, and returns its id.
uint32_t addTexture(Device* d, const Texture& texture) {
    prMutexLock(&d->mutex);
    uint32_t id = 0;
    if (d->freeTextures.count != 0) {
        id = d->freeTextures.last();
        d->freeTextures.pop();
    } else {
        d->textures.push(Texture{});
        id = d->textures.count;
    }
    d->textures.data[id - 1] = texture;
    prMutexUnlock(&d->mutex);
    return id;
}

PrStatus textureCreate(PrGpuDevice* device, const PrGpuTextureDesc* desc, PrGpuTexture* out) {
    Device* d = deviceOf(device);
    if (!supportsFormat(device, desc->format, desc->usage) || desc->samples != 1)
        return PrStatus_NotSupported;
    if (desc->levels > kMaxLevels)
        return PrStatus_Limit;
    Memory* memory = allocZeroed<Memory>();
    memory->references = 1;
    memory->owned = true;
    memory->desc = *desc;
    memory->desc.name = PrStringView{};
    size_t size = 0;
    for (uint32_t level = 0; level < desc->levels; ++level) {
        memory->levelOffsets[level] = size;
        memory->rowBytes[level] = size_t(levelSize(desc->extent.width, level)) * texelBytes(desc->format);
        memory->sliceBytes[level] = memory->rowBytes[level] * levelSize(desc->extent.height, level);
        size += memory->sliceBytes[level] * levelSlices(memory->desc, level);
    }
    memory->bytes = static_cast<uint8_t*>(prMemoryAllocZeroed(size));
    *out = PrGpuTexture{addTexture(d, Texture{.memory = memory, .desc = memory->desc})};
    return PrStatus_Ok;
}

PrStatus textureCreateOverPixels(PrGpuDevice* device, PrGpuFormat format, PrPixels pixels,
        PrGpuTextureUsageFlags usage, PrGpuTexture* out) {
    Device* d = deviceOf(device);
    PrPixelFormat matched{};
    if (!prGpuFormatToPixelFormat(format, &matched) || !supportsFormat(device, format, usage))
        return PrStatus_NotSupported;
    if (pixels.extent.x > kMaxTextureSize || pixels.extent.y > kMaxTextureSize)
        return PrStatus_Limit;
    Memory* memory = allocZeroed<Memory>();
    memory->references = 1;
    memory->desc = prGpuTextureDesc2d(format, pixels.extent, usage);
    memory->bytes = pixels.data;
    memory->rowBytes[0] = pixels.stride;
    memory->sliceBytes[0] = pixels.stride * pixels.extent.y;
    *out = PrGpuTexture{addTexture(d, Texture{.memory = memory, .desc = memory->desc})};
    return PrStatus_Ok;
}

PrStatus textureCreateView(PrGpuDevice* device, PrGpuTexture texture, PrGpuTextureKind kind,
        PrGpuSubresources subresources, PrGpuTexture* out) {
    Device* d = deviceOf(device);
    prMutexLock(&d->mutex);
    Texture of = textureAt(d, texture);
    ++of.memory->references;
    prMutexUnlock(&d->mutex);
    uint8_t levels = subresources.levels != 0 ? subresources.levels : uint8_t(of.desc.levels - subresources.firstLevel);
    PrGpuTextureDesc desc = of.desc;
    desc.kind = kind;
    desc.levels = levels;
    desc.extent.width = levelSize(of.desc.extent.width, subresources.firstLevel);
    desc.extent.height = levelSize(of.desc.extent.height, subresources.firstLevel);
    if (of.desc.kind == PrGpuTextureKind_3d)
        desc.extent.depth = levelSize(of.desc.extent.depth, subresources.firstLevel);
    else
        desc.extent.depth =
            subresources.layers != 0 ? subresources.layers : of.desc.extent.depth - subresources.firstLayer;
    Texture view{
        .memory = of.memory,
        .desc = desc,
        .firstLevel = uint8_t(of.firstLevel + subresources.firstLevel),
        .firstLayer = uint16_t(of.firstLayer + subresources.firstLayer),
    };
    *out = PrGpuTexture{addTexture(d, view)};
    return PrStatus_Ok;
}

void textureDestroy(PrGpuDevice* device, PrGpuTexture texture) {
    Device* d = deviceOf(device);
    gpuSharedTextureForget(d, texture);
    prMutexLock(&d->mutex);
    PR_ASSERT(textureAt(d, texture).memory != nullptr);
    retire(d, texture.id, true);
    prMutexUnlock(&d->mutex);
}

PrGpuTextureDesc textureDesc(const PrGpuDevice* device, PrGpuTexture texture) {
    return textureOf(deviceOf(device), texture).desc;
}

uint32_t textureStorageHandle(const PrGpuDevice*, PrGpuTexture texture) {
    return texture.id;
}

// A texture's native object: its texels, which the caller may write as well as read, or its copy.
bool textureNative(PrGpuDevice* device, PrGpuTexture texture, PrGpuNativeKind kind, void* out) {
    Device* d = deviceOf(device);
    prMutexLock(&d->mutex);
    bool held = true;
    if (kind == PrGpuNativeKind_Pixels) {
        // A texture that only shaders sample is never drawn into, so its texels are only read.
        bool drawn = (textureAt(d, texture).desc.usage & PrGpuTextureUsageFlag_RenderTarget) != 0;
        Texture t = drawn ? writableTexture(d, texture) : readableTexture(d, texture);
        *static_cast<PrGpuNativePixels*>(out) = PrGpuNativePixels{pixelsOf(t, 0, 0), t.desc.format};
    } else {
        uint32_t mirror = syncMirror(d, memoryAt(d, texture));
        *static_cast<uint32_t*>(out) = mirror;
        held = mirror != 0;
    }
    prMutexUnlock(&d->mutex);
    return held;
}

void textureUpload(PrGpuDevice* device, PrGpuTexture texture, const PrGpuTextureRegion* region, const void* data,
        uint32_t rowBytes, uint32_t layerBytes) {
    Device* d = deviceOf(device);
    prMutexLock(&d->mutex);
    Texture t = writableTexture(d, texture);
    prMutexUnlock(&d->mutex);
    copyRegion(t, *region, static_cast<uint8_t*>(const_cast<void*>(data)), rowBytes, layerBytes, true);
}

// No shader samples, so a sampler is only its id.
PrStatus samplerCreate(PrGpuDevice* device, const PrGpuSamplerDesc*, PrGpuSampler* out) {
    Device* d = deviceOf(device);
    prMutexLock(&d->mutex);
    *out = PrGpuSampler{++d->samplers};
    prMutexUnlock(&d->mutex);
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shaders and pipelines, which the device does not run

PrStatus shaderLibraryCreate(PrGpuDevice*, PrGpuShaderEntry*, uint32_t, PrGpuShaderLibrary* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    return PrStatus_NotSupported;
}

PrStatus graphicsPipelineCreate(PrGpuDevice*, const PrGpuGraphicsPipelineDesc*, const PrGpuShaderEntry*,
        const PrGpuShaderEntry* PR_NULLABLE, PrGpuPipeline* out) {
    *out = PrGpuPipeline{};
    return PrStatus_NotSupported;
}

PrStatus computePipelineCreate(PrGpuDevice*, const PrGpuShaderLibrary*, const PrGpuShaderEntry*, PrStringView,
        PrGpuPipeline* out) {
    *out = PrGpuPipeline{};
    return PrStatus_NotSupported;
}

void pipelineDestroy(PrGpuDevice*, PrGpuPipeline) {}

PrGpuExtent pipelineGroupSize(const PrGpuDevice*, PrGpuPipeline) {
    return PrGpuExtent{};
}

// ---------------------------------------------------------------------------------------------------------------------
// Command buffers

Commands* commandsOf(PrGpuCommands* commands) {
    return static_cast<Commands*>(commands);
}

void record(PrGpuCommands* commands, const Op& op) {
    commandsOf(commands)->ops.push(op);
}

bool commandsBegin(PrGpuDevice* device, const PrGpuCommandsOptions* options, PrGpuCommands* PR_NULLABLE* PR_NONNULL out) {
    // The device has no native command buffers for an application to record into.
    PR_ASSERT(options->native == nullptr);
    (void)options;
    Device* d = deviceOf(device);
    Commands* commands = allocZeroed<Commands>();
    static_cast<PrGpuCommands&>(*commands) = kCommandsFunctions;
    commands->device = device;
    prMutexLock(&d->mutex);
    commands->serial = ++d->serial;
    d->open.push(commands->serial);
    prMutexUnlock(&d->mutex);
    *out = commands;
    return true;
}

PrGpuFence commandsSubmit(PrGpuCommands* commands) {
    Commands* c = commandsOf(commands);
    gpuSharedSubmitted(c);
    Device* d = deviceOf(c->device);
    prMutexLock(&d->mutex);
    for (uint32_t i = 0; i < c->ops.count; ++i) {
        // A lost device runs no more work, though it still reports what is done.
        if (!d->lost || c->ops.data[i].kind == OpKind_Done)
            run(d, c->ops.data[i]);
    }
    for (uint32_t i = 0; i < c->streams.count; ++i)
        retire(d, c->streams.data[i].id, false);
    d->open.removeFirst(c->serial);
    collect(d);
    PrGpuFence fence = ++d->submitted;
    prMutexUnlock(&d->mutex);
    c->ops.reset();
    c->streams.reset();
    prMemoryFree(c);
    return fence;
}

void* commandsStream(PrGpuCommands* commands, uint64_t size, PrGpuBuffer* buffer, uint64_t* offset) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    PrGpuBufferDesc desc{.size = size, .memory = PrGpuMemory_Upload};
    bufferCreate(device, &desc, buffer);
    commandsOf(commands)->streams.push(*buffer);
    *offset = 0;
    return bufferMap(device, *buffer);
}

// Root data and pipelines are for draws and dispatches, which the device does not record.
void commandsSetPush(PrGpuCommands*, const void*, uint32_t) {}

void commandsSetUniforms(PrGpuCommands*, const void*, uint32_t) {}

void commandsSetPipeline(PrGpuCommands*, PrGpuPipeline) {
    PR_ASSERT(false && "a memory device has no pipelines");
}

void commandsDispatch(PrGpuCommands* commands, PrGpuExtent) {
    gpuSharedCheckOutsidePass(commands);
    PR_ASSERT(false && "a memory device runs no dispatches");
}

void commandsDispatchIndirect(PrGpuCommands* commands, PrGpuBuffer, uint64_t) {
    gpuSharedCheckOutsidePass(commands);
    PR_ASSERT(false && "a memory device runs no dispatches");
}

void commandsCopyBuffer(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset,
        PrGpuBuffer target, uint64_t targetOffset, uint64_t size) {
    gpuSharedCheckOutsidePass(commands);
    record(commands, Op{
        .kind = OpKind_CopyBuffer,
        .buffer = source,
        .target = target,
        .offset = sourceOffset,
        .targetOffset = targetOffset,
        .size = size,
    });
}

void commandsCopyTexture(PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    gpuSharedCheckOutsidePass(commands);
    record(commands, Op{
        .kind = OpKind_CopyTexture,
        .texture = source,
        .targetTexture = target,
        .region = *sourceRegion,
        .targetRegion = *targetRegion,
    });
}

void commandsCopyBufferToTexture(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset,
        uint32_t rowBytes, uint32_t layerBytes, PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    gpuSharedCheckOutsidePass(commands);
    record(commands, Op{
        .kind = OpKind_CopyBufferToTexture,
        .buffer = source,
        .targetTexture = target,
        .offset = sourceOffset,
        .rowBytes = rowBytes,
        .layerBytes = layerBytes,
        .targetRegion = *targetRegion,
    });
}

void commandsCopyTextureToBuffer(PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes,
        uint32_t layerBytes) {
    gpuSharedCheckOutsidePass(commands);
    record(commands, Op{
        .kind = OpKind_CopyTextureToBuffer,
        .target = target,
        .texture = source,
        .targetOffset = targetOffset,
        .rowBytes = rowBytes,
        .layerBytes = layerBytes,
        .region = *sourceRegion,
    });
}

void commandsOnDone(PrGpuCommands* commands, PrGpuDoneFn done, void* PR_NULLABLE context) {
    record(commands, Op{.kind = OpKind_Done, .done = done, .context = context});
}

// The work runs in the order that it was recorded, so groups and barriers have nothing to do.
void commandsPushGroup(PrGpuCommands*, PrStringView) {}

void commandsPopGroup(PrGpuCommands*) {}

void commandsBufferBarrier(PrGpuCommands* commands, PrGpuBuffer, PrGpuAccessFlags, PrGpuAccessFlags) {
    gpuSharedCheckOutsidePass(commands);
}

void commandsTextureBarrier(PrGpuCommands* commands, PrGpuTexture, PrGpuSubresources, PrGpuAccessFlags,
        PrGpuAccessFlags) {
    gpuSharedCheckOutsidePass(commands);
}

void commandsGlobalBarrier(PrGpuCommands* commands, PrGpuAccessFlags, PrGpuAccessFlags) {
    gpuSharedCheckOutsidePass(commands);
}

PrGpuNativeStateFlags nativeStateSync(PrGpuCommands*) {
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Passes, which load, clear and run native draws

PrGpuPass* passBegin(PrGpuCommands* commands, const PrGpuPassDesc* desc) {
    Commands* c = commandsOf(commands);
    gpuSharedPassBegan(c);
    // The device has no native passes for an application to open.
    PR_ASSERT(desc->native == nullptr);
    Device* d = deviceOf(c->device);
    Pass* pass = allocZeroed<Pass>();
    static_cast<PrGpuPass&>(*pass) = kPassFunctions;
    pass->commands = c;
    pass->formats.colorCount = desc->colorCount;
    pass->formats.samples = 1;
    for (uint8_t i = 0; i < desc->colorCount; ++i) {
        const PrGpuAttachment& a = desc->color[i];
        PrGpuTextureDesc t = textureOf(d, a.texture).desc;
        pass->formats.color[i] = t.format;
        pass->extent = PrUVec2{levelSize(t.extent.width, a.level), levelSize(t.extent.height, a.level)};
        if (i == 0) {
            pass->texture = a.texture;
            pass->level = a.level;
            pass->layer = a.layer;
        }
        if (a.load == PrGpuLoad_Clear) {
            record(commands, Op{
                .kind = OpKind_Clear,
                .texture = a.texture,
                .region = PrGpuTextureRegion{.level = a.level, .firstLayer = a.layer},
                .color = desc->clearColor[i],
            });
        }
    }
    return pass;
}

void passEnd(PrGpuPass* pass) {
    gpuSharedPassEnded(static_cast<GpuSharedPass*>(pass)->commands);
    prMemoryFree(pass);
}

// A copy holds only a texture's first level and layer, so a native draw through the copy draws only into those.
void passNativeDraw(PrGpuPass* base, PrGpuNativeKind kind, PrGpuNativeDrawFn draw, void* PR_NULLABLE context) {
    Pass* pass = static_cast<Pass*>(base);
    PR_ASSERT(kind == PrGpuNativeKind_Pixels || (pass->level == 0 && pass->layer == 0));
    record(pass->commands, Op{
        .kind = OpKind_NativeDraw, .texture = pass->texture,
            .region = PrGpuTextureRegion{.level = pass->level, .firstLayer = pass->layer}, .nativeKind = kind, .draw = draw,
            .context = context,
    });
}

void passSetViewport(PrGpuPass*, PrRect, float, float) {}

void passSetScissor(PrGpuPass*, PrIRect) {}

void passSetBlendConstant(PrGpuPass*, PrColor) {}

void passSetStencilReference(PrGpuPass*, uint8_t) {}

void passSetIndexBuffer(PrGpuPass*, PrGpuBuffer, uint64_t, bool) {}

void passDraw(PrGpuPass*, uint32_t, uint32_t, uint32_t, uint32_t) {
    PR_ASSERT(false && "a memory device runs no draws");
}

void passDrawIndexed(PrGpuPass*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
    PR_ASSERT(false && "a memory device runs no draws");
}

void passDrawIndirect(PrGpuPass*, PrGpuBuffer, uint64_t, uint32_t, uint32_t) {
    PR_ASSERT(false && "a memory device runs no draws");
}

void passDrawIndexedIndirect(PrGpuPass*, PrGpuBuffer, uint64_t, uint32_t, uint32_t) {
    PR_ASSERT(false && "a memory device runs no draws");
}

// ---------------------------------------------------------------------------------------------------------------------
// Swap chains, which the device lacks until its backend creates its own

PrStatus swapChainCreate(PrGpuDevice*, const PrAppSurface*, const PrGpuSwapChainDesc*,
        PrGpuSwapChain* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    return PrStatus_NotSupported;
}

// The functions of the device's command buffers and passes.
constexpr PrGpuCommands kCommandsFunctions = {
    .deviceFn = gpuSharedCommandsDevice,
    .submitFn = commandsSubmit,
    .streamFn = commandsStream,
    .setPushFn = commandsSetPush,
    .setUniformsFn = commandsSetUniforms,
    .setPipelineFn = commandsSetPipeline,
    .dispatchFn = commandsDispatch,
    .dispatchIndirectFn = commandsDispatchIndirect,
    .copyBufferFn = commandsCopyBuffer,
    .copyTextureFn = commandsCopyTexture,
    .copyBufferToTextureFn = commandsCopyBufferToTexture,
    .copyTextureToBufferFn = commandsCopyTextureToBuffer,
    .generateMipmapsFn = gpuSharedGenerateMipmaps,
    .onDoneFn = commandsOnDone,
    .pushGroupFn = commandsPushGroup,
    .popGroupFn = commandsPopGroup,
    .bufferBarrierFn = commandsBufferBarrier,
    .textureBarrierFn = commandsTextureBarrier,
    .globalBarrierFn = commandsGlobalBarrier,
    .nativeStateSyncFn = nativeStateSync,
    .passBeginFn = passBegin,
};

constexpr PrGpuPass kPassFunctions = {
    .commandsFn = gpuSharedPassCommands,
    .formatsFn = gpuSharedPassFormats,
    .extentFn = gpuSharedPassExtent,
    .endFn = passEnd,
    .nativeDrawFn = passNativeDraw,
    .setViewportFn = passSetViewport,
    .setScissorFn = passSetScissor,
    .setBlendConstantFn = passSetBlendConstant,
    .setStencilReferenceFn = passSetStencilReference,
    .setIndexBufferFn = passSetIndexBuffer,
    .drawFn = passDraw,
    .drawIndexedFn = passDrawIndexed,
    .drawIndirectFn = passDrawIndirect,
    .drawIndexedIndirectFn = passDrawIndexedIndirect,
};

} // namespace

PrGpuDevice* gpuMemoryCreate(PrStringView backend, const GpuMemoryMirror* PR_NULLABLE mirror) {
    Device* d = allocZeroed<Device>();
    gpuSharedDeviceInit(d);
    prMutexInit(&d->mutex);
    d->mirror = mirror;
    d->destroyFn = destroy;
    d->isLostFn = isLost;
    d->simulateLossFn = simulateLoss;
    d->isCompleteFn = isComplete;
    d->waitFn = wait;
    d->waitIdleFn = waitIdle;
    d->supportsFormatFn = supportsFormat;
    d->bufferCreateFn = bufferCreate;
    d->bufferDestroyFn = bufferDestroy;
    d->bufferDescFn = bufferDesc;
    d->bufferStorageHandleFn = bufferStorageHandle;
    d->bufferMapFn = bufferMap;
    d->bufferUploadFn = bufferUpload;
    d->textureCreateFn = textureCreate;
    d->textureCreateViewFn = textureCreateView;
    d->textureDestroyFn = textureDestroy;
    d->textureDescFn = textureDesc;
    d->textureStorageHandleFn = textureStorageHandle;
    d->textureUploadFn = textureUpload;
    d->textureCreateOverPixelsFn = textureCreateOverPixels;
    d->textureNativeFn = textureNative;
    d->samplerCreate = samplerCreate;
    d->shaderLibraryCreateFn = shaderLibraryCreate;
    d->graphicsPipelineCreateFn = graphicsPipelineCreate;
    d->computePipelineCreateFn = computePipelineCreate;
    d->pipelineDestroyFn = pipelineDestroy;
    d->pipelineGroupSizeFn = pipelineGroupSize;
    d->commandsBeginFn = commandsBegin;
    d->swapChainCreateFn = swapChainCreate;
    d->info = PrGpuDeviceInfo{
        .adapter = "memory",
        .backend = backend,
        .maxTextureSize = kMaxTextureSize,
        .maxResourceHandles = UINT32_MAX,
        .maxSamplerHandles = UINT32_MAX,
    };
    d->nativeKinds = 1u << PrGpuNativeKind_Pixels | (mirror != nullptr ? 1u << mirror->kind : 0u);
    return d;
}

void gpuMemoryAdoptMirror(PrGpuDevice* device, PrGpuTexture texture, uint32_t mirror) {
    Device* d = deviceOf(device);
    prMutexLock(&d->mutex);
    Memory& m = memoryAt(d, texture);
    PR_ASSERT(m.mirror == 0 && mirrorable(d, m));
    m.mirror = mirror;
    m.held = Held_Both;
    prMutexUnlock(&d->mutex);
}

PrStatus gpuMemoryImport(PrGpuDevice* device, PrUVec2 extent, PrGpuFormat format, uint32_t mirror,
        PrGpuReleaseFn PR_NULLABLE release, void* PR_NULLABLE context, PrGpuTexture* out) {
    Device* d = deviceOf(device);
    PrGpuTextureDesc desc = prGpuTextureDesc2d(format, extent, PrGpuTextureUsageFlag_Sampled);
    PrStatus status = textureCreate(device, &desc, out);
    if (status != PrStatus_Ok)
        return status;
    prMutexLock(&d->mutex);
    Memory& m = memoryAt(d, *out);
    m.mirror = mirror;
    m.held = Held_Mirror;
    m.release = release;
    m.releaseContext = context;
    prMutexUnlock(&d->mutex);
    return PrStatus_Ok;
}

void gpuMemoryRecordCall(PrGpuCommands* commands, void (*call)(void* PR_NULLABLE context), void* PR_NULLABLE context) {
    record(commands, Op{.kind = OpKind_Call, .call = call, .context = context});
}

} // namespace pr_impl

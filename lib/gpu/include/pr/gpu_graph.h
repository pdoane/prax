// Render graphs: a frame's passes and the resources that each one uses, declared and then recorded into a command
// buffer. The graph knows every pass's accesses, so it records the barriers that a device leaves to its caller
// (pr/gpu.h), skips the passes whose results nothing uses, and lends device textures to the textures that live for one
// frame.
//
// A frame builds its graph again each time, in plain code: prGpuGraphBegin, the resources, the passes and their uses,
// then prGpuGraphRecord. Nothing of one frame's graph carries over to the next except the device textures that the
// graph lends.
//
// The graph holds the resources whose accesses change during a frame. A resource that every pass only reads, such as a
// mesh's vertices, needs no barrier, and a pass binds it without telling the graph.
//
// A pass depends on the earlier passes that it shares a resource with, where either of the two writes the resource. The
// graph records the passes in the order that they were added, except that a pass which follows a later added pass
// (prGpuGraphFollowPass) is recorded after it.
//
// The graph tracks a texture as a whole and a buffer by byte ranges. It records every barrier just before the pass that
// needs it, and never earlier.
//
// The graph checks what it is told, and reports what it finds as a list of errors (prGpuGraphError). An error never
// makes a call unsafe: the graph records what it can.
//
// One thread uses a graph at a time. Every call stays safe on a lost device: the graph then lends no texture, and it
// records no pass that uses a resource without a texture or buffer of the device.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/gpu.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Graphs

// A render graph of one device, which holds the frame that is being built and the device textures that it lends.
typedef struct PrGpuGraph PrGpuGraph;

// A texture, a buffer or a pass of the frame that is being built. The value is valid until the next prGpuGraphBegin.
// Zero is none, and a call that is given none does nothing. A call that adds to the graph ignores a value of an earlier
// frame, and reports PrGpuGraphErrorKind_StaleId.
typedef struct PrGpuGraphTexture {
    uint32_t id;
} PrGpuGraphTexture;
typedef struct PrGpuGraphBuffer {
    uint32_t id;
} PrGpuGraphBuffer;
typedef struct PrGpuGraphPass {
    uint32_t id;
} PrGpuGraphPass;

// The maximum numbers of a frame's passes, and of its textures and buffers together.
enum {
    kPrGpuGraphMaxPasses = 65535,
    kPrGpuGraphMaxResources = 65535,
};

// What a graph favors where fewer barriers and less memory conflict. The priority decides which device texture a
// frame's texture takes.
//
// A device texture keeps its accesses from one frame to the next. It goes back to the graph after the last pass that
// uses it, in the accesses of that pass, and another texture of the same frame may then take it.
enum {
    // The fewest barriers. A frame's texture takes a free device texture of its description that is already in the
    // accesses of the texture's first use, and needs no barrier. So a texture that every frame uses alike, and ends in
    // the accesses that it began in, needs no barrier after its first frame. Without such a device texture, the frame's
    // texture takes a free one that was left in accesses that only read, and pays one barrier. A first use writes, so
    // that device texture could match no first use, and a new one would need the same barrier. Otherwise the graph
    // makes a device texture.
    PrGpuGraphPriority_Barriers,

    // The least memory. A frame's texture takes a free device texture of its description, one that is already in the
    // accesses of its first use if there is one, and pays a barrier for any other. The graph makes a device texture
    // only if none is free.
    PrGpuGraphPriority_Memory,
};
typedef uint8_t PrGpuGraphPriority;

// How a graph is created.
typedef struct PrGpuGraphOptions {
    PrGpuGraphPriority priority;
} PrGpuGraphOptions;

// Returns the default options: PrGpuGraphPriority_Barriers.
PrGpuGraphOptions prGpuGraphOptionsDefault(void);

// Creates a graph for `device`, which outlives it. A null `options` means the defaults.
PrGpuGraph* prGpuGraphCreate(PrGpuDevice* device, const PrGpuGraphOptions* PR_NULLABLE options);

// Frees the graph, its frame, and the device textures that it lends. The device's work that uses them may still be
// running.
void prGpuGraphDestroy(PrGpuGraph* PR_NULLABLE graph);

// Begins a frame's graph, which replaces the last one. It invalidates the memory that prGpuGraphAlloc gave out and
// keeps the storage for the new frame. It destroys the device textures that no frame has taken for several frames.
void prGpuGraphBegin(PrGpuGraph* graph);

// Allocates `size` bytes for the frame, aligned to 16 bytes and valid until the next prGpuGraphBegin or the graph's
// destruction. A pass's callback keeps its context here, so a frame is built without managing the lifetime of what its
// callbacks read.
void* prGpuGraphAlloc(PrGpuGraph* graph, size_t size);

// ---------------------------------------------------------------------------------------------------------------------
// Resources

// Add a resource of the caller's, which is in `accesses` when the graph is recorded. The graph holds a reference to the
// resource (prGpuTextureRetain) until it records the frame, or until the next frame begins or the graph is destroyed if
// it never records it, so the caller may release its own once the call returns. A zero texture or buffer, as a failed
// creation leaves, is added too: no pass that uses it is recorded.
PrGpuGraphTexture prGpuGraphImportTexture(PrGpuGraph* graph, PrGpuTexture texture, PrGpuAccessFlags accesses);
PrGpuGraphBuffer prGpuGraphImportBuffer(PrGpuGraph* graph, PrGpuBuffer buffer, PrGpuAccessFlags accesses);

// Set the accesses that the recording leaves an imported resource in, through a barrier after the last pass if the
// resource is in other accesses then. A resource that is left is a result of the frame, so the passes that write it are
// recorded. An imported resource that is not left stays in the accesses of the last recorded pass that uses it. Only an
// imported texture is left: a frame's texture is a result through the pass that reads it into one.
void prGpuGraphLeaveTexture(PrGpuGraph* graph, PrGpuGraphTexture texture, PrGpuAccessFlags accesses);
void prGpuGraphLeaveBuffer(PrGpuGraph* graph, PrGpuGraphBuffer buffer, PrGpuAccessFlags accesses);

// Adds a texture of `desc` that lives for this frame, with undefined contents. The graph lends it a device texture
// while it records. The description's name names a device texture that the graph makes for it.
PrGpuGraphTexture prGpuGraphAddTexture(PrGpuGraph* graph, const PrGpuTextureDesc* desc);

// Returns the device's texture, which a pass's shaders name by its handle. An imported texture has one at any time. A
// frame's texture has one only while the graph records, from the first recorded pass that uses it to the last, so a
// pass asks for it in its callback. Returns zero at any other time, for none or a value of an earlier frame, and for a
// frame's texture that the device could not make.
PrGpuTexture prGpuGraphTexture(const PrGpuGraph* graph, PrGpuGraphTexture texture);

// Returns the device's buffer of a buffer that the frame imported, for a pass of another module's that reads or writes
// a buffer that it was handed as the graph's. Returns zero for none or a value of an earlier frame.
PrGpuBuffer prGpuGraphBuffer(const PrGpuGraph* graph, PrGpuGraphBuffer buffer);

// ---------------------------------------------------------------------------------------------------------------------
// Passes

// An attachment of a pass: a texture, or one level and layer of it, and what the pass does with its contents.
typedef struct PrGpuGraphAttachment {
    PrGpuGraphTexture texture;
    PrGpuGraphTexture resolve;  // the target of PrGpuStore_Resolve and PrGpuStore_ResolveKeep, or none
    uint8_t level;
    uint16_t layer;
    PrGpuLoad load;
    PrGpuStore store;
} PrGpuGraphAttachment;

// The attachments of a pass that draws, and the values that the pass clears them to.
typedef struct PrGpuGraphPassDesc {
    PrStringView name;                                      // a name for debuggers and captures, or empty
    uint8_t colorCount;
    PrGpuGraphAttachment color[kPrGpuMaxColorAttachments];  // by color attachment
    PrColor clearColor[kPrGpuMaxColorAttachments];          // the values that Clear writes, by color attachment
    PrGpuGraphAttachment depthStencil;                      // no texture for none
    PrGpuLoad stencilLoad;
    PrGpuStore stencilStore;
    float clearDepth;
    uint8_t clearStencil;
} PrGpuGraphPassDesc;

// Returns a pass over one color attachment, which it loads as `load` says, clears to `clear` if it clears it, and
// keeps, as prGpuPassDescColor describes a pass of the device.
PrGpuGraphPassDesc prGpuGraphPassDescColor(PrGpuGraphTexture texture, PrGpuLoad load, PrColor clear);

// Called while the graph records, to record a pass's draws into `pass`, which the graph begins and ends.
typedef void (*PrGpuGraphDrawFn)(void* PR_NULLABLE context, PrGpuPass* pass);

// Called while the graph records, to record work outside any pass into `commands`.
typedef void (*PrGpuGraphRecordFn)(void* PR_NULLABLE context, PrGpuCommands* commands);

// Adds a pass that draws to attachments. The graph calls `draw` if it records the pass. The pass writes each attachment
// and each resolve target, which the graph declares, and the caller declares every other use (prGpuGraphUseTexture).
// The graph copies the description's name.
PrGpuGraphPass prGpuGraphAddPass(PrGpuGraph* graph, const PrGpuGraphPassDesc* desc, PrGpuGraphDrawFn draw,
    void* PR_NULLABLE context);

// Adds work that records outside any pass, such as dispatches and copies, in a group named `name`, which the graph
// copies. The graph calls `record` if it records the work. Work that declares no write has effects that the graph does
// not see, so it is always recorded. The graph records no barrier within one work: work that needs a barrier between
// two of its own dispatches is two works.
PrGpuGraphPass prGpuGraphAddWork(PrGpuGraph* graph, PrStringView name, PrGpuGraphRecordFn record, void* PR_NULLABLE context);

// Declare that a pass reads or writes a resource in `accesses`, which hold a write (PrGpuAccessFlag_Write) or reads,
// and not both. Several declarations by one pass for one resource add their accesses together.
void prGpuGraphUseTexture(PrGpuGraph* graph, PrGpuGraphPass pass, PrGpuGraphTexture texture, PrGpuAccessFlags accesses);
void prGpuGraphUseBuffer(PrGpuGraph* graph, PrGpuGraphPass pass, PrGpuGraphBuffer buffer, PrGpuAccessFlags accesses);

// Declares that a pass reads or writes `size` bytes of a buffer from `offset`, in `accesses`. Two uses of ranges that
// do not overlap make no dependency between their passes, and two writes of such ranges in the same accesses need no
// barrier between them. A second declaration by one pass for one buffer widens the use to the whole buffer.
void prGpuGraphUseBufferRange(PrGpuGraph* graph, PrGpuGraphPass pass, PrGpuGraphBuffer buffer,
    PrGpuAccessFlags accesses, uint64_t offset, uint64_t size);

// Declares that `pass` depends on `earlier` through an effect that the graph does not see: `pass` is recorded after
// `earlier`, and `earlier` is recorded if `pass` is. `earlier` may have been added after `pass`.
void prGpuGraphFollowPass(PrGpuGraph* graph, PrGpuGraphPass pass, PrGpuGraphPass earlier);

// ---------------------------------------------------------------------------------------------------------------------
// Recording

// What recording a graph did.
typedef struct PrGpuGraphStats {
    uint32_t passes;        // the passes and works recorded
    uint32_t barriers;      // the barriers recorded
    uint32_t textures;      // the device textures lent to the frame's textures
    uint64_t textureBytes;  // the bytes of those textures' first levels
} PrGpuGraphStats;

// Records the frame's graph into `commands`, outside any pass, and ends the frame: nothing more is added until the next
// prGpuGraphBegin.
//
// A pass is recorded if a result of the frame depends on it: it declares no write, it writes a resource that is left,
// or a recorded pass depends on it by using what it wrote or by following it (prGpuGraphFollowPass). Every other pass
// is skipped, and its callback is not called.
//
// Between two passes that use bytes of one resource, where the earlier added of the two writes them, the later added
// depends on the earlier. A pass that writes also follows the earlier added passes that read the bytes it writes.
// Passes that only read one resource do not depend on each other.
//
// The passes are recorded in the order that they were added, which keeps every dependency that the uses make. Only
// prGpuGraphFollowPass can name a pass that was added later, and the order then changes as far as that dependency
// needs: each pass recorded is the earliest added whose dependencies are all recorded. The order depends only on the
// frame's graph, so equal graphs record in equal orders.
//
// A barrier brings a resource to the accesses of the pass that needs it, just before that pass. A barrier for a read
// also names the accesses of the reads that follow, up to the next write, so that one barrier serves them all.
PrGpuGraphStats prGpuGraphRecord(PrGpuGraph* graph, PrGpuCommands* commands);

// ---------------------------------------------------------------------------------------------------------------------
// Errors

// The misuse that a graph reports.
enum {
    // A pass declared both a read and a write of one resource. The graph treats the use as a write.
    PrGpuGraphErrorKind_ReadWrite,

    // A recorded pass reads a frame's texture that no pass recorded before it has written. The pass reads undefined
    // contents.
    PrGpuGraphErrorKind_UnwrittenRead,

    // The passes that the pass follows (prGpuGraphFollowPass) depend on the pass in turn. The graph records the pass
    // ahead of them.
    PrGpuGraphErrorKind_Cycle,

    // A call took a texture, a buffer or a pass that is not of the frame being built. The graph ignored the call.
    PrGpuGraphErrorKind_StaleId,
};
typedef uint8_t PrGpuGraphErrorKind;

// One misuse of the frame's graph.
typedef struct PrGpuGraphError {
    PrGpuGraphErrorKind kind;
    PrGpuGraphPass pass;    // the pass at fault, or none for a call that named no pass of the frame
    PrStringView name;      // the name of that pass, valid until the next prGpuGraphBegin
} PrGpuGraphError;

// Return the errors that the frame's graph has reported since prGpuGraphBegin, in the order that the graph found them.
// Building reports PrGpuGraphErrorKind_ReadWrite and _StaleId, and recording reports the others.
uint32_t prGpuGraphErrorCount(const PrGpuGraph* graph);
PrGpuGraphError prGpuGraphError(const PrGpuGraph* graph, uint32_t index);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

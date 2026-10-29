// Draw list internals: freezing lists that other resources reference, and the recorded commands.

#pragma once

#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/draw_commands.h"
#include "pr/draw_list.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A group that composites in sRGB-linear, which the list records as groups that composite in sRGB
// (prDrawListPushGroup), and whose pop closes them together.
struct DrawLoweredGroup {
    uint32_t depth;             // the number of open pushes once the group is pushed
    bool nested;                // an inner group holds the group's other effects
};

// A resource of the host's that a list holds (prDrawListHold).
struct DrawHeld {
    void (*release)(void* PR_NULLABLE context);
    void* PR_NULLABLE context;
};

// Commands and all the data that they point to.
struct DrawRecording {
    Array<PrDrawCommand> commands;
    Array<uint8_t*> blocks;           // side storage, in blocks that never move
    size_t blockUsed;                 // bytes used of the last block
    size_t blockSize;                 // the last block's size
    Array<PrPath*> paths;             // the clones that commands point to
    Array<const PrDrawImage*> images; // holds one reference to each image
    Array<const PrFont*> fonts;
    Array<const PrDrawList*> lists;
    Array<const PrDrawFilter*> filters;
    Array<DrawHeld> held;
};

// A recording of draw commands.
struct DrawList {
    mutable Atomic<uint32_t> references;    // changes behind const, as holders retain and release a frozen list

    // The value of prDrawListId, or 0 after a change until it is next requested. Freezing assigns it, so the id of a
    // list that threads share never changes.
    mutable uint64_t id;

    bool frozen;                    // another list references this list
    bool writesPixels;              // once frozen: prDrawListWritesPixels
    bool readsBackdrop;             // once frozen: prDrawListReadsBackdrop
    PrRect bounds;                  // once frozen
    DrawRecording recording;
    Array<PrDrawCommandKind> open;   // innermost last; a clip of any kind is stored as PushClipRect
    Array<DrawLoweredGroup> lowered; // the open groups that composite in sRGB-linear, innermost last

    // The brush and stroke copied last, in side storage. A command whose brush or stroke equals one of them shares the
    // copy, so that commands recorded in one paint store it once.
    const PrDrawBrush* PR_NULLABLE lastBrush;
    const PrDrawStroke* PR_NULLABLE lastStroke;
};

// Freezes the list. It must not change again.
void drawListFreeze(DrawList* list);

// Returns the commands in recording order. They stay valid until the list changes.
const Array<PrDrawCommand>& drawListCommands(const DrawList* list);

// Adds or releases a reference to a frozen list that a command, a source or a filter holds.
void drawListRetain(const DrawList* list);
void drawListRelease(const DrawList* PR_NULLABLE list);

} // namespace pr_impl

// C opaque types
struct PrDrawList : pr_impl::DrawList {};

PR_ASSUME_NONNULL_END

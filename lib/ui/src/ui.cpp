// Immediate-mode interaction: input routing, focus, capture, scrolling and retained state for a layout tree that the
// application declares in full every round.

#include "pr/ui.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/integer.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_unit.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// Marks the absence of an entry in a list of states or in the queue.
constexpr uint32_t kUiNone = 0xffffffffu;

// The pointer buttons the context tracks.
constexpr uint32_t kUiButtons = 8;

// The maximum number of nodes that a hit test returns to the router, topmost first. It is enough to pass every
// transparent node over a point.
constexpr uint32_t kUiHitCapacity = 64;

// The data that the context holds for a node, in a column indexed by the node's slot (prLayoutNodeSlot). Each part is
// stamped with the round that wrote it. The entry belongs to a node only while `node` is that node's handle.
struct UiSlot {
    PrLayoutNode node;
    uint32_t flagsRound;            // the round that set `flags`
    uint32_t hoverRound;            // the round whose hover chain includes the node
    uint32_t focusRound;            // the round whose focus chain includes the node
    uint32_t pressRound;            // the round whose press chain includes the node
    uint32_t cursorRound;           // the round that requested `cursor`
    uint32_t state;                 // the node's first state, or kUiNone
    double hoverSince;              // the time the pointer last came over the node

    // Whether the node or a node it was created under is disabled, as of the round that resolved it.
    uint32_t disabledRound;
    bool disabled;

    // The signals of the round that last signaled the node, and their owner.
    uint32_t signalRound;
    uint32_t signals;
    const void* PR_NULLABLE signalOwner;

    // The node's first context, if the round that provided it is the current round.
    uint32_t contextRound;
    uint32_t context;

    PrUiFlags flags;
    PrAppCursor cursor;
};

// Storage that a node provides to the nodes created under it for one round (prUiProvide).
struct UiContext {
    const void* owner;
    uint64_t key;
    void* data;
    uint32_t next;                  // the node's next context, or kUiNone
};

// A block of the storage of the round's contexts.
struct UiBlock {
    uint8_t* data;
    uint32_t size;
};

// Storage that a node keeps for an owner and a key (prUiState).
struct UiState {
    PrLayoutNode node;
    const void* owner;
    uint64_t key;
    uint32_t size;
    uint32_t round;                 // the round that last asked for the storage
    uint32_t next;                  // the node's next state, or kUiNone
    void* data;
    PrUiStateDestroyFn PR_NULLABLE destroy;
};

// The value of a tween (prUiTween).
struct UiTween {
    float from;
    float target;
    float value;
    double start;                   // the time the target last changed
    bool started;
};

// A queued event. Its text starts at `textStart` in the queue's text buffer.
struct UiQueued {
    PrAppEvent event;
    uint32_t textStart;
};

// An event routed to a node in the current round.
struct UiRouted {
    PrLayoutNode node;
    PrAppEvent event;
};

// A key a node claimed in a round (prUiKeyPressed), or captured (prUiKeyCaptured).
struct UiClaim {
    PrLayoutNode node;
    PrAppKey key;
    PrAppModifierFlags modifiers;
    bool capture;
};

// The state of one pointer button: the node that its press captured, and its drag.
struct UiButton {
    PrLayoutNode capture;
    PrLayoutNode node;              // the node that owns the drag, kept through the round that ends it
    PrLayoutNode pressed;           // the node that captured the press, kept until the next press
    double pressTime;
    double repeatTime;              // the time up to which repeats of the press were last counted
    PrVec2 origin;
    PrVec2 position;
    PrVec2 delta;                   // motion in the current round
    float reach;                    // the squared distance of the farthest the pointer has been from the origin
    PrAppModifierFlags modifiers;   // modifiers held at the press
    uint32_t presses;               // the presses of the sequence that the press ends, counting it
    bool dragging;
    bool released;                  // true if the current round ended the drag with a release
    bool cancelled;
};

// Wheel deltas that the current round routed to a node with PrUiFlag_Wheel.
struct UiWheel {
    PrLayoutNode node;
    PrVec2 delta;
};

// A payload drag (prUiDragStart). A press on the source arms it, and the payload is carried once the pointer moves past
// the threshold.
struct UiCarry {
    PrUiPayload payload;
    float threshold;
    bool armed;
    bool carried;
};

// A group that the context keeps (prUiGroup), with its recipe's type, owner, hash and bytes. The entry holds a
// reference to its group and to its base.
struct UiGroupEntry {
    const void* type;
    const void* PR_NULLABLE owner;
    uint32_t hash;
    uint32_t offset;                // where the recipe's bytes start in UiGroups::bytes
    uint32_t size;
    uint32_t used;                  // the round of the last lookup that found the group
    PrLayoutGroupIndex group;
    PrLayoutGroupIndex base;
    PrLayoutGroup kind;
};

// The groups that recipes made. The context creates these groups in the tree without interning them, so this index is
// the only place where a group is found by its value.
struct UiGroups {
    HashIndex index;
    Array<UiGroupEntry> entries;
    Array<uint8_t> bytes;
};

// The number of rounds without a lookup after which a group is released.
constexpr uint32_t kUiGroupAge = 64;

// The number of lookups that the memo keeps for one slot of the preorder: the lookups that a round makes before its
// next creation.
constexpr uint32_t kUiMemoCalls = 4;

// What the memo holds for one lookup: the part's type, owner and bytes, and its record. The entry holds a reference to
// its record and to its base.
struct UiMemoEntry {
    const void* PR_NULLABLE type;   // null if the entry holds nothing
    const void* PR_NULLABLE owner;
    uint32_t size;                  // the number of the part's bytes, which start at `offset` in UiMemo::bytes
    uint32_t offset;
    uint32_t capacity;              // the number of bytes reserved at `offset`
    PrLayoutStyleIndex style;
    PrLayoutStyleIndex base;
};

// The memo: the parts that the last frame's lookups named, and the records that they returned, indexed by where in the
// round each lookup was made.
struct UiMemo {
    Array<UiMemoEntry> entries;     // by slot, kUiMemoCalls to a slot, then by the lookup's place among the slot's
    Array<uint8_t> bytes;
    uint32_t slot;                  // the slot of the round's last lookup, or kPrLayoutFrameCursorEnd
    uint32_t calls;                 // the number of lookups made at `slot` before the last
    uint32_t at;                    // the entry that prUiMemoKeep fills for the last lookup, or kUiNone
};

// The function that receives the edit gestures of the controls under a node (prUiSetEditSink).
struct UiEditSink {
    PrUiEditFn PR_NULLABLE fn;
    void* PR_NULLABLE context;
};

// The owner of the storage that lib/ui itself provides on nodes (prUiProvide), and the keys of that storage.
constexpr char kUiOwner = 0;
enum : uint64_t {
    UiContextKey_EditSink = 1,
};

// The overlay's key, and the stack level that puts it above everything in flow.
constexpr uint64_t kUiOverlayKey = 0x6f7665726c6179ull;
constexpr int32_t kUiOverlayLevel = 1000;

// The greatest number of duplicate keys that a round outlines, and the width in px and the colors of an outline.
constexpr uint32_t kUiDuplicatesShown = 8;
constexpr float kUiDuplicateWidth = 2;
constexpr PrColor kUiDuplicateBorder = {1, 0.25f, 0.25f, 1};
constexpr PrColor kUiDuplicateFill = {1, 0.25f, 0.25f, 0.15f};

// A context: the data it holds for the tree's nodes, its rounds, and the input that it routes.
struct Ui {
    PrLayoutTree* tree;
    PrApp* PR_NULLABLE app;            // whose clipboard widgets use
    PrAppDisplay display;              // the display of the frame that runs, or of the last frame
    PrAppUserPreferences preferences;

    // --- Nodes ---

    Array<UiSlot> slots;            // by slot
    Array<UiState> states;          // indexed by state; freed entries are listed in freeStates
    Array<uint32_t> freeStates;

    // The round's contexts, and the blocks of their storage, which the next round reuses.
    Array<UiContext> contexts;
    Array<UiBlock> blocks;
    uint32_t block;                 // the block that the next context's storage comes from
    uint32_t blockUsed;             // bytes used of that block

    // --- Rounds ---

    PrUiRound round;
    uint32_t roundId;               // incremented as each round begins, starting at 1
    uint32_t built;                 // the round whose tree is committed, or 0 before the first round
    bool building;
    bool roundDisables;             // the round has disabled a node, so its nodes resolve disabled through parents
    bool frameRequested;
    bool tweening;
    bool repeating;
    double time;

    // --- Input ---

    Array<UiQueued> queue;
    Array<char> queueText;
    uint32_t consumed;              // number of queued events routed in the running frame
    Array<UiRouted> routed;         // events routed in the current round, grouped by node once routing ends
    Array<PrAppEvent> events;       // the events of `routed`, in the same order
    bool transitioned;              // true if the current round routed a press, a release or a key press

    // --- Pointer ---

    bool hasPointer;
    PrVec2 pointer;
    PrLayoutNode hovered;           // the node under the pointer, where the hover chain starts
    UiButton buttons[kUiButtons];
    PrLayoutNode clicked[kUiButtons];
    bool pressedRound[kUiButtons];  // true if the button was pressed in the current round
    PrRect textInput;               // the caret of the text being edited, if hasTextInput
    bool hasTextInput;
    PrAppCursor cursor;
    Array<UiWheel> wheels;
    UiCarry payload;
    PrLayoutNode dropOver;          // the innermost of the previous round's drop targets under the pointer
    PrLayoutNode dropped;           // the drop target in the current round, or none
    PrUiPayload droppedPayload;
    Array<PrLayoutNode> dropTargets;     // drop targets made by the current round's build (prUiIsDropTarget)
    Array<PrLayoutNode> lastDropTargets; // drop targets made by the previous build; drops in this round land on them

    // --- Keys and focus ---

    Array<UiClaim> claims;          // keys claimed by the current round's build (prUiKeyPressed)
    Array<UiClaim> lastClaims;      // keys claimed by the previous build; key presses in this round are routed by them
    UiClaim keyed;                  // the claim that received this round's key press; its node is none if there is none
    PrLayoutNode focus;
    PrLayoutNode focusRequest;
    bool focusRequested;
    bool focusVisible;
    PrLayoutNode keyActivated;       // the node activated by Enter or Space
    Array<PrLayoutNode> activations; // nodes activated in this round, which prUiActivate marked in the previous round
    Array<PrLayoutNode> nextActivations;

    // --- Records ---

    UiGroups groups;
    UiMemo memo;

    // --- Overlay ---

    PrLayoutStyleIndex overlayStyle; // the overlay's record, or 0 before a round first asks for the overlay
    PrLayoutNode overlay;
    uint32_t overlayRound;           // the round that created `overlay`
};

// Returns the entry for the slot of `node`. If the entry belonged to another node, it is reset for `node`.
UiSlot& slotOf(Ui* ui, PrLayoutNode node) {
    uint32_t slot = prLayoutNodeSlot(node);
    while (ui->slots.count <= slot)
        ui->slots.emplace() = UiSlot{.state = kUiNone};
    UiSlot& s = ui->slots.data[slot];
    if (s.node != node) {
        // The slot's previous node was erased. Its states stay listed until they are freed, because no round asks for
        // them.
        uint32_t state = s.state;
        s = UiSlot{.node = node, .state = state};
    }
    return s;
}

// Returns the entry for the slot of `node` if it belongs to the node, or null.
const UiSlot* PR_NULLABLE findSlot(const Ui* ui, PrLayoutNode node) {
    if (node == kPrLayoutNodeNone)
        return nullptr;
    uint32_t slot = prLayoutNodeSlot(node);
    if (slot >= ui->slots.count || ui->slots.data[slot].node != node)
        return nullptr;
    return &ui->slots.data[slot];
}

// Returns the flags set on `node` by the round that built the committed tree. Routing reads these flags.
PrUiFlags builtFlags(const Ui* ui, PrLayoutNode node) {
    const UiSlot* s = findSlot(ui, node);
    return s != nullptr && s->flagsRound == ui->built ? s->flags : PrUiFlags(0);
}

// Returns true if the node has `flag` and is not disabled, according to the flags of the committed tree.
bool takes(const Ui* ui, PrLayoutNode node, PrUiFlags flag) {
    PrUiFlags flags = builtFlags(ui, node);
    return (flags & flag) != 0 && (flags & PrUiFlag_Disabled) == 0;
}

// Returns true if `node`, which the current round created, or a node it was created under is disabled. The answer is
// kept on the node for the round, so a subtree resolves each of its nodes once.
bool disabledWithin(Ui* ui, PrLayoutNode node) {
    if (!ui->roundDisables || node == kPrLayoutNodeNone)
        return false;
    UiSlot& s = slotOf(ui, node);
    if (s.disabledRound == ui->roundId)
        return s.disabled;
    bool own = s.flagsRound == ui->roundId && (s.flags & PrUiFlag_Disabled) != 0;
    bool disabled = own || disabledWithin(ui, prLayoutTreeFrameParent(ui->tree, node));
    UiSlot& resolved = slotOf(ui, node);
    resolved.disabledRound = ui->roundId;
    resolved.disabled = disabled;
    return disabled;
}

// Returns `size` bytes of zeroed storage, aligned to 16, that stay in place until the round ends.
void* allocContext(Ui* ui, uint32_t size) {
    constexpr uint32_t kBlockSize = 4096;
    size = alignUp(size, 16);
    while (ui->block < ui->blocks.count && ui->blockUsed + size > ui->blocks.data[ui->block].size) {
        ++ui->block;
        ui->blockUsed = 0;
    }
    if (ui->block == ui->blocks.count) {
        uint32_t blockSize = size > kBlockSize ? size : kBlockSize;
        ui->blocks.push(UiBlock{.data = static_cast<uint8_t*>(prMemoryAlloc(blockSize)), .size = blockSize});
        ui->blockUsed = 0;
    }
    uint8_t* data = ui->blocks.data[ui->block].data + ui->blockUsed;
    ui->blockUsed += size;
    memZero(data, size);
    return data;
}

PrLayoutNode parentOf(const Ui* ui, PrLayoutNode node) {
    return prLayoutNodeIsValid(ui->tree, node) ? prLayoutNodeParent(ui->tree, node) : kPrLayoutNodeNone;
}

// Returns true if `node` is `ancestor` or one of its descendants in the committed tree.
bool within(const Ui* ui, PrLayoutNode node, PrLayoutNode ancestor) {
    for (PrLayoutNode at = node; at != kPrLayoutNodeNone; at = parentOf(ui, at)) {
        if (at == ancestor)
            return true;
    }
    return false;
}

// Returns the innermost node, starting at `node` and walking up its ancestors, that has `flag` and is not disabled, or
// none.
PrLayoutNode innermost(const Ui* ui, PrLayoutNode node, PrUiFlags flag) {
    for (PrLayoutNode at = node; at != kPrLayoutNodeNone; at = parentOf(ui, at)) {
        if (takes(ui, at, flag))
            return at;
    }
    return kPrLayoutNodeNone;
}

// Returns the node under `point`, in viewport px, using the paint order of the committed tree. The result is the
// topmost node that lets the pointer through neither itself nor through an ancestor.
PrLayoutNode hitAt(const Ui* ui, PrVec2 point) {
    if (ui->built == 0 || prLayoutTreeRoot(ui->tree) == kPrLayoutNodeNone)
        return kPrLayoutNodeNone;
    PrLayoutNode hits[kUiHitCapacity];
    uint32_t count = prLayoutTreeHitTest(ui->tree, prLayoutVecFromPx(point), hits, kUiHitCapacity);
    if (count > kUiHitCapacity)
        count = kUiHitCapacity;
    for (uint32_t i = 0; i < count; ++i) {
        PrLayoutNode node = hits[i];
        if ((builtFlags(ui, node) & PrUiFlag_Transparent) != 0)
            continue;
        bool through = false;
        for (PrLayoutNode at = node; at != kPrLayoutNodeNone && !through; at = parentOf(ui, at))
            through = (builtFlags(ui, at) & PrUiFlag_SubtreeTransparent) != 0;
        if (!through)
            return node;
    }
    return kPrLayoutNodeNone;
}

// Stamps the current round on the hover chain, from the node under the pointer up to the root.
void stampHover(Ui* ui) {
    for (PrLayoutNode at = ui->hovered; at != kPrLayoutNodeNone; at = parentOf(ui, at)) {
        UiSlot& s = slotOf(ui, at);
        if (s.hoverRound + 1 != ui->roundId)
            s.hoverSince = ui->time;
        s.hoverRound = ui->roundId;
    }
}

void stampFocus(Ui* ui) {
    for (PrLayoutNode at = ui->focus; at != kPrLayoutNodeNone; at = parentOf(ui, at))
        slotOf(ui, at).focusRound = ui->roundId;
}

void deliver(Ui* ui, PrLayoutNode node, const PrAppEvent& event) {
    if (node != kPrLayoutNodeNone)
        ui->routed.push(UiRouted{node, event});
}

bool isPointerEvent(PrAppEventKind kind) {
    return kind == PrAppEventKind_PointerMove || kind == PrAppEventKind_PointerDown ||
        kind == PrAppEventKind_PointerUp || kind == PrAppEventKind_Scroll;
}

// Returns true if an event of this kind changes the answers to a round's queries, so that a second such event in the
// same round would be lost.
bool isTransition(PrAppEventKind kind) {
    return kind == PrAppEventKind_PointerDown || kind == PrAppEventKind_PointerUp || kind == PrAppEventKind_KeyDown;
}

// Moves the drag of `b` to `position`.
void moveDrag(UiButton& b, PrVec2 position) {
    b.delta = b.delta + position - b.position;
    b.position = position;
    float dx = position.x - b.origin.x;
    float dy = position.y - b.origin.y;
    if (dx * dx + dy * dy > b.reach)
        b.reach = dx * dx + dy * dy;
}

// Ends the drag of `b`, by a release or a cancellation.
void endDrag(UiButton& b, bool released) {
    if (!b.dragging)
        return;
    b.dragging = false;
    b.released = released;
    b.cancelled = !released;
}

// Starts carrying an armed payload once the primary drag has gone past its threshold.
void carry(Ui* ui) {
    UiCarry& p = ui->payload;
    const UiButton& b = ui->buttons[0];
    if (!p.armed || p.carried || !b.dragging)
        return;
    if (b.reach >= p.threshold * p.threshold)
        p.carried = true;
}

// Returns the innermost node, starting at `node` and walking up its ancestors, that was a drop target in the previous
// round and is not the payload's source. Returns none if there is no such node.
PrLayoutNode dropTargetOf(const Ui* ui, PrLayoutNode node) {
    for (PrLayoutNode at = node; at != kPrLayoutNodeNone; at = parentOf(ui, at)) {
        if (at == ui->payload.payload.source)
            continue;
        for (uint32_t i = 0; i < ui->lastDropTargets.count; ++i) {
            if (ui->lastDropTargets.data[i] == at)
                return at;
        }
    }
    return kPrLayoutNodeNone;
}

// Gives the wheel deltas, in px, to the scroll containers and wheel nodes from `node` outward. Each one takes as much
// as it can.
void scroll(Ui* ui, PrLayoutNode node, PrVec2 delta, const PrAppEvent& event) {
    for (PrLayoutNode at = node; at != kPrLayoutNodeNone && (delta.x != 0 || delta.y != 0); at = parentOf(ui, at)) {
        if (takes(ui, at, PrUiFlag_Wheel)) {
            for (uint32_t i = 0; i < ui->wheels.count; ++i) {
                if (ui->wheels.data[i].node == at) {
                    ui->wheels.data[i].delta.x += delta.x;
                    ui->wheels.data[i].delta.y += delta.y;
                    deliver(ui, at, event);
                    return;
                }
            }
            ui->wheels.push(UiWheel{at, delta});
            deliver(ui, at, event);
            return;
        }
        if (!takes(ui, at, PrUiFlag_Scrollable))
            continue;
        PrLayoutScrollInfo info{};
        if (!prLayoutNodeScrollInfo(ui->tree, at, &info) || !info.scrollContainer)
            continue;
        PrLayoutVec offset = info.offset;
        PrLayoutVec want = offset;
        PrLayoutVec by = prLayoutVecFromPx(delta);
        if ((info.userAxes & PrLayoutScrollFlag_X) != 0) {
            int32_t x = offset.x.raw + by.x.raw;
            x = x < info.range.left.raw ? info.range.left.raw : x > info.range.right.raw ? info.range.right.raw : x;
            want.x = PrLayoutUnit{x};
        }
        if ((info.userAxes & PrLayoutScrollFlag_Y) != 0) {
            int32_t y = offset.y.raw + by.y.raw;
            y = y < info.range.top.raw ? info.range.top.raw : y > info.range.bottom.raw ? info.range.bottom.raw : y;
            want.y = PrLayoutUnit{y};
        }
        if (want.x.raw == offset.x.raw && want.y.raw == offset.y.raw)
            continue;
        prLayoutNodeSetScroll(ui->tree, at, want);
        deliver(ui, at, event);
        PrVec2 took = prLayoutVecToPx(PrLayoutVec{
            PrLayoutUnit{want.x.raw - offset.x.raw},
            PrLayoutUnit{want.y.raw - offset.y.raw},
        });
        delta.x -= took.x;
        delta.y -= took.y;
    }
}

// Returns the node after `node` in the preorder of the committed tree, within the subtree of `scope`, wrapping to
// `scope`. If `reverse` is true, returns the previous node instead.
PrLayoutNode stepPreorder(const Ui* ui, PrLayoutNode node, PrLayoutNode scope, bool reverse) {
    PrLayoutTree* tree = ui->tree;
    if (!reverse) {
        PrLayoutNode child = prLayoutNodeFirstChild(tree, node);
        if (child != kPrLayoutNodeNone)
            return child;
        for (PrLayoutNode at = node; at != scope; at = prLayoutNodeParent(tree, at)) {
            PrLayoutNode next = prLayoutNodeNextSibling(tree, at);
            if (next != kPrLayoutNodeNone)
                return next;
        }
        return scope;
    }
    if (node == scope) {
        PrLayoutNode last = scope;
        for (PrLayoutNode child = prLayoutNodeLastChild(tree, last); child != kPrLayoutNodeNone;
                child = prLayoutNodeLastChild(tree, last))
            last = child;
        return last;
    }
    PrLayoutNode previous = prLayoutNodePreviousSibling(tree, node);
    if (previous == kPrLayoutNodeNone)
        return prLayoutNodeParent(tree, node);
    for (PrLayoutNode child = prLayoutNodeLastChild(tree, previous); child != kPrLayoutNodeNone;
            child = prLayoutNodeLastChild(tree, previous))
        previous = child;
    return previous;
}

// Returns the next focusable node after `from` within `scope`, in the order of the committed tree, wrapping at the
// ends. If `reverse` is true, returns the previous one. Returns none if there is no such node.
PrLayoutNode focusStep(const Ui* ui, PrLayoutNode from, PrLayoutNode scope, bool reverse) {
    if (scope == kPrLayoutNodeNone)
        scope = prLayoutTreeRoot(ui->tree);
    if (scope == kPrLayoutNodeNone || !prLayoutNodeIsValid(ui->tree, scope))
        return kPrLayoutNodeNone;
    PrLayoutNode start = from != kPrLayoutNodeNone && within(ui, from, scope) ? from : scope;
    PrLayoutNode at = start;
    for (;;) {
        at = stepPreorder(ui, at, scope, reverse);
        if (takes(ui, at, PrUiFlag_Focusable))
            return at;
        if (at == start)
            return takes(ui, start, PrUiFlag_Focusable) && start != from ? start : kPrLayoutNodeNone;
    }
}

// Returns the innermost focus scope that encloses `node`, or none if the scope is the whole tree.
PrLayoutNode focusScopeOf(const Ui* ui, PrLayoutNode node) {
    for (PrLayoutNode at = parentOf(ui, node); at != kPrLayoutNodeNone; at = parentOf(ui, at)) {
        if ((builtFlags(ui, at) & PrUiFlag_FocusScope) != 0)
            return at;
    }
    return kPrLayoutNodeNone;
}

// Returns the claim of `at` for the key press `e`, as a capture or as a plain claim, or null if there is none.
const UiClaim* PR_NULLABLE claimOf(const Ui* ui, PrLayoutNode at, const PrAppEvent& e, bool capture) {
    for (uint32_t i = 0; i < ui->lastClaims.count; ++i) {
        const UiClaim& c = ui->lastClaims.data[i];
        if (c.node == at && c.key == e.key && c.modifiers == e.modifiers && c.capture == capture)
            return &c;
    }
    return nullptr;
}

// Routes a key press. It goes to the outermost capture on the focused node or its ancestors, then to the nearest claim
// on them, then to focus traversal, then to activation.
void routeKey(Ui* ui, const PrAppEvent& e) {
    // A modifier key alone does not show the focus.
    bool modifier = e.key == PrAppKey_Shift || e.key == PrAppKey_Control || e.key == PrAppKey_Alt ||
        e.key == PrAppKey_Meta || e.key == PrAppKey_CapsLock || e.key == PrAppKey_Fn;
    ui->focusVisible = ui->focusVisible || !modifier;
    PrLayoutNode start = ui->focus != kPrLayoutNodeNone ? ui->focus : prLayoutTreeRoot(ui->tree);
    const UiClaim* captured = nullptr;
    for (PrLayoutNode at = start; at != kPrLayoutNodeNone; at = parentOf(ui, at)) {
        if ((builtFlags(ui, at) & PrUiFlag_Disabled) == 0) {
            if (const UiClaim* c = claimOf(ui, at, e, true))
                captured = c;
        }
    }
    if (captured != nullptr) {
        ui->keyed = *captured;
        deliver(ui, captured->node, e);
        return;
    }
    for (PrLayoutNode at = start; at != kPrLayoutNodeNone; at = parentOf(ui, at)) {
        if ((builtFlags(ui, at) & PrUiFlag_Disabled) != 0)
            continue;
        if (const UiClaim* c = claimOf(ui, at, e, false)) {
            ui->keyed = *c;
            deliver(ui, at, e);
            return;
        }
    }
    if (e.key == PrAppKey_Escape && ui->payload.carried) {
        endDrag(ui->buttons[0], false);
        ui->payload = UiCarry{};
        return;
    }
    if (e.key == PrAppKey_Tab && (e.modifiers & ~PrAppModifierFlag_Shift) == 0) {
        bool reverse = (e.modifiers & PrAppModifierFlag_Shift) != 0;
        PrLayoutNode next = focusStep(ui, ui->focus, focusScopeOf(ui, ui->focus), reverse);
        if (next != kPrLayoutNodeNone)
            ui->focus = next;
        return;
    }
    bool activates = e.key == PrAppKey_Enter || e.key == ' ';
    if (activates && e.modifiers == 0 && takes(ui, ui->focus, PrUiFlag_Pressable)) {
        ui->keyActivated = ui->focus;
        deliver(ui, ui->focus, e);
        return;
    }
    deliver(ui, ui->focus, e);
}

// Routes one event against the committed tree.
void route(Ui* ui, const PrAppEvent& e) {
    if (isPointerEvent(e.kind)) {
        ui->hasPointer = true;
        ui->pointer = e.position;
        ui->hovered = hitAt(ui, e.position);
    }
    switch (e.kind) {
        case PrAppEventKind_PointerMove: {
            PrLayoutNode to = ui->hovered;
            for (uint32_t b = 0; b < kUiButtons; ++b) {
                UiButton& button = ui->buttons[b];
                if (button.dragging)
                    moveDrag(button, e.position);
                if (button.capture != kPrLayoutNodeNone)
                    to = button.capture;
            }
            carry(ui);
            deliver(ui, to, e);
            break;
        }
        case PrAppEventKind_PointerDown: {
            if (e.button >= kUiButtons)
                break;
            ui->focusVisible = false;
            ui->pressedRound[e.button] = true;
            for (PrLayoutNode at = ui->hovered; at != kPrLayoutNodeNone; at = parentOf(ui, at))
                slotOf(ui, at).pressRound = ui->roundId;
            PrLayoutNode target = innermost(ui, ui->hovered, PrUiFlag_Pressable);
            UiButton& b = ui->buttons[e.button];
            // A multi-click continues only on the node that it began on.
            bool sequence = target != kPrLayoutNodeNone && target == b.pressed && e.clicks > 1;
            uint32_t presses = sequence ? e.clicks : 1;
            b = UiButton{
                .capture = target,
                .node = target,
                .pressed = target,
                .pressTime = ui->time,
                .repeatTime = ui->time,
                .origin = e.position,
                .position = e.position,
                .modifiers = e.modifiers,
                .presses = presses,
                .dragging = target != kPrLayoutNodeNone,
            };
            if (e.button == 0)
                ui->focus = innermost(ui, ui->hovered, PrUiFlag_Focusable);
            deliver(ui, target != kPrLayoutNodeNone ? target : ui->hovered, e);
            break;
        }
        case PrAppEventKind_PointerUp: {
            if (e.button >= kUiButtons)
                break;
            UiButton& b = ui->buttons[e.button];
            if (b.dragging)
                moveDrag(b, e.position);
            bool carried = e.button == 0 && ui->payload.carried;
            if (carried) {
                ui->dropped = dropTargetOf(ui, ui->hovered);
                ui->droppedPayload = ui->payload.payload;
            } else if (b.capture != kPrLayoutNodeNone && within(ui, ui->hovered, b.capture)) {
                ui->clicked[e.button] = b.capture;
            }
            if (e.button == 0)
                ui->payload = UiCarry{};
            deliver(ui, b.capture != kPrLayoutNodeNone ? b.capture : ui->hovered, e);
            endDrag(b, true);
            b.capture = kPrLayoutNodeNone;
            break;
        }
        case PrAppEventKind_Scroll:
            scroll(ui, ui->hovered, prAppEventScrollPixels(&e, kPrAppScrollLineHeight), e);
            break;
        case PrAppEventKind_KeyDown:
            routeKey(ui, e);
            break;
        case PrAppEventKind_KeyUp:
        case PrAppEventKind_Text:
        case PrAppEventKind_Clipboard:
        case PrAppEventKind_CompositionUpdate:
        case PrAppEventKind_CompositionEnd:
            deliver(ui, ui->focus, e);
            break;
        default:
            break;
    }
}

// Frees the states that the current round did not ask for, including those of erased nodes.
void sweepStates(Ui* ui) {
    for (uint32_t i = 0; i < ui->states.count; ++i) {
        UiState& state = ui->states.data[i];
        if (state.data == nullptr || state.round == ui->roundId)
            continue;
        // Unlink the state from its node's list, if the node's slot still lists it.
        uint32_t slot = prLayoutNodeSlot(state.node);
        if (slot < ui->slots.count) {
            uint32_t* link = &ui->slots.data[slot].state;
            while (*link != kUiNone && *link != i)
                link = &ui->states.data[*link].next;
            if (*link == i)
                *link = state.next;
        }
        if (state.destroy != nullptr)
            state.destroy(state.data);
        prMemoryFree(state.data);
        state = UiState{};
        ui->freeStates.push(i);
    }
}

// Returns the state of `node` for `owner` and `key`, creating zeroed storage if there is none.
UiState& stateOf(Ui* ui, PrLayoutNode node, const void* owner, uint64_t key, uint32_t size) {
    UiSlot& s = slotOf(ui, node);
    for (uint32_t at = s.state; at != kUiNone;) {
        UiState& state = ui->states.data[at];
        if (state.node == node && state.owner == owner && state.key == key) {
            PR_ASSERT(state.size == size);
            state.round = ui->roundId;
            return state;
        }
        at = state.next;
    }
    uint32_t index = ui->states.count;
    if (ui->freeStates.count != 0) {
        index = ui->freeStates.last();
        ui->freeStates.pop();
    } else {
        ui->states.emplace();
    }
    UiSlot& entry = slotOf(ui, node);
    ui->states.data[index] = UiState{
        .node = node,
        .owner = owner,
        .key = key,
        .size = size,
        .round = ui->roundId,
        .next = entry.state,
        .data = prMemoryAllocZeroed(size < 16 ? 16 : size),
    };
    entry.state = index;
    return ui->states.data[index];
}

// Releases the references that a kept group holds.
void releaseGroup(Ui* ui, const UiGroupEntry& e) {
    prLayoutGroupRelease(ui->tree, e.kind, e.group);
    if (e.base != 0)
        prLayoutGroupRelease(ui->tree, e.kind, e.base);
}

// Releases the references that a memo entry holds, and empties it.
void releaseMemoEntry(Ui* ui, UiMemoEntry& e) {
    prLayoutStyleRelease(ui->tree, e.style);
    if (e.base != 0)
        prLayoutStyleRelease(ui->tree, e.base);
    e.type = nullptr;
    e.owner = nullptr;
}

// Keeps the groups for which `keep` returns true, and releases the others. The kept entries and their bytes are
// compacted, and the index is rebuilt.
template <typename Keep>
void filterGroups(Ui* ui, Keep keep) {
    UiGroups& groups = ui->groups;
    uint32_t kept = 0;
    uint32_t bytes = 0;
    groups.index.clear();
    for (uint32_t i = 0; i < groups.entries.count; ++i) {
        UiGroupEntry e = groups.entries.data[i];
        if (!keep(e)) {
            releaseGroup(ui, e);
            continue;
        }
        memMove(groups.bytes.data + bytes, groups.bytes.data + e.offset, e.size);
        e.offset = bytes;
        bytes += e.size;
        groups.index.insert(e.hash, kept);
        groups.entries.data[kept++] = e;
    }
    groups.entries.count = kept;
    groups.bytes.count = bytes;
}

// Releases the groups that no lookup has found for kUiGroupAge rounds. Runs once in that many rounds.
void ageGroups(Ui* ui) {
    if (ui->roundId % kUiGroupAge != 0)
        return;
    // A group that a record still names stays, so that no recipe's group is made again under a second index while the
    // first is in use.
    filterGroups(ui, [&](const UiGroupEntry& e) {
        return ui->roundId - e.used < kUiGroupAge || prLayoutGroupReferences(ui->tree, e.kind, e.group) != 1;
    });
}

// Makes the group of `recipe` and creates it in the tree. The recipe's make writes the kind's value, which starts as
// layout's default.
PrLayoutGroupIndex makeGroup(Ui* ui, const PrUiGroupRecipe& recipe) {
    PrLayoutTree* tree = ui->tree;
    switch (recipe.kind) {
        case PrLayoutGroup_Font: {
            PrLayoutFontStyle value = prLayoutFontStyleDefault();
            recipe.make(recipe.owner, recipe.bytes, &value);
            return prLayoutFontStyleCreate(tree, &value);
        }
        case PrLayoutGroup_Color: {
            PrLayoutColorStyle value = prLayoutColorStyleDefault();
            recipe.make(recipe.owner, recipe.bytes, &value);
            return prLayoutColorStyleCreate(tree, &value);
        }
        case PrLayoutGroup_Box: {
            PrLayoutBoxStyle value = prLayoutBoxStyleDefault();
            recipe.make(recipe.owner, recipe.bytes, &value);
            return prLayoutBoxStyleCreate(tree, &value);
        }
        case PrLayoutGroup_Decoration: {
            PrLayoutDecorationStyle value = prLayoutDecorationStyleDefault();
            recipe.make(recipe.owner, recipe.bytes, &value);
            return prLayoutDecorationStyleCreate(tree, &value);
        }
        case PrLayoutGroup_Effect: {
            PrLayoutEffectStyle value = prLayoutEffectStyleDefault();
            recipe.make(recipe.owner, recipe.bytes, &value);
            return prLayoutEffectStyleCreate(tree, &value);
        }
        case PrLayoutGroup_Flex: {
            PrLayoutFlexStyle value = prLayoutFlexStyleDefault();
            recipe.make(recipe.owner, recipe.bytes, &value);
            return prLayoutFlexStyleCreate(tree, &value);
        }
        case PrLayoutGroup_Table: {
            PrLayoutTableStyle value = prLayoutTableStyleDefault();
            recipe.make(recipe.owner, recipe.bytes, &value);
            return prLayoutTableStyleCreate(tree, &value);
        }
        case PrLayoutGroup_Grid: {
            PrLayoutGridStyle value = prLayoutGridStyleDefault();
            recipe.make(recipe.owner, recipe.bytes, &value);
            return prLayoutGridStyleCreate(tree, &value);
        }
        default:
            PR_FATAL("a group recipe of no kind");
    }
}

// Makes the overlay's record, a layer that covers the whole root above everything in flow, and returns it with a
// reference that the context holds.
PrLayoutStyleIndex makeOverlayStyle(Ui* ui) {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    box.position = PrLayoutPosition_Absolute;
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        box.inset[side] = prLayoutLengthPx(0);
    box.zIndexed = 1;
    PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
    effects.zIndex = kUiOverlayLevel;
    PrLayoutStyle record = prLayoutStyleDefault();
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleCreate(ui->tree, &box);
    record.groups[PrLayoutGroup_Effect] = prLayoutEffectStyleCreate(ui->tree, &effects);
    PrLayoutStyleIndex style = prLayoutStyleIntern(ui->tree, &record);
    prLayoutStyleRetain(ui->tree, style);
    return style;
}

// Sets the input flags of a node that the current round created (prUiSetFlags).
void setFlags(Ui* ui, PrLayoutNode node, PrUiFlags flags) {
    PR_ASSERT(ui->building);
    if (ui->roundDisables && disabledWithin(ui, prLayoutTreeFrameParent(ui->tree, node)))
        flags |= PrUiFlag_Disabled;
    ui->roundDisables = ui->roundDisables || (flags & PrUiFlag_Disabled) != 0;
    UiSlot& s = slotOf(ui, node);
    s.flags = flags;
    s.flagsRound = ui->roundId;
    s.disabledRound = ui->roundId;
    s.disabled = (flags & PrUiFlag_Disabled) != 0;
}

// Returns the round's overlay, which the first call in a round creates (prUiOverlay).
PrLayoutNode overlayOf(Ui* ui) {
    PR_ASSERT(ui->building);
    if (ui->overlayRound == ui->roundId)
        return ui->overlay;
    PrLayoutNode root = kPrLayoutNodeNone;
    prLayoutTreeFrameRoots(ui->tree, &root, 1);
    PR_ASSERT(root != kPrLayoutNodeNone);
    if (ui->overlayStyle == 0)
        ui->overlayStyle = makeOverlayStyle(ui);
    ui->overlay = prLayoutTreeFrameElement(ui->tree, root, kUiOverlayKey, ui->overlayStyle);
    setFlags(ui, ui->overlay, PrUiFlag_Transparent);
    ui->overlayRound = ui->roundId;
    return ui->overlay;
}

// Outlines `node` in the overlay, if the node is in the tree and had a box at the last layout. `origin` is the top left
// of the overlay's box at the last layout, in viewport px.
void outlineDuplicate(Ui* ui, PrLayoutNode node, PrVec2 origin) {
    if (!prLayoutNodeIsValid(ui->tree, node))
        return;
    PrRect r = prLayoutRectToPx(prLayoutNodeBounds(ui->tree, node, PrLayoutCoordinates_Viewport));
    if (r.right <= r.left && r.bottom <= r.top)
        return;
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    box.boxSizing = PrLayoutBoxSizing_BorderBox;
    box.position = PrLayoutPosition_Absolute;
    box.inset[PrRectSide_Left] = prLayoutLengthPx(r.left - origin.x);
    box.inset[PrRectSide_Top] = prLayoutLengthPx(r.top - origin.y);
    box.width = prLayoutLengthPx(r.right - r.left);
    box.height = prLayoutLengthPx(r.bottom - r.top);
    PrLayoutDecorationStyle decoration = prLayoutDecorationStyleDefault();
    decoration.background = prColor16FromColor(kUiDuplicateFill);
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        box.border[side] = prLayoutUnitFromPx(kUiDuplicateWidth);
        decoration.borderColor[side] = prColor16FromColor(kUiDuplicateBorder);
        decoration.borderStyle[side] = PrDrawBorderStyle_Solid;
    }
    PrLayoutStyle record = prLayoutStyleDefault();
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(ui->tree, &box);
    record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(ui->tree, &decoration);
    PrLayoutStyleIndex style = prLayoutStyleIntern(ui->tree, &record);
    PrLayoutNode outline = prLayoutTreeFrameElement(ui->tree, overlayOf(ui), 0, style);
    setFlags(ui, outline, PrUiFlag_SubtreeTransparent);
}

// Outlines both nodes of each key that the last commit reports as given twice (prLayoutTreeDuplicateKeys). A key given
// twice is a caller's error, which every application sees whichever library made the nodes.
void outlineDuplicates(Ui* ui) {
    PrLayoutDuplicateKey duplicates[kUiDuplicatesShown];
    uint32_t count = prLayoutTreeDuplicateKeys(ui->tree, duplicates, kUiDuplicatesShown);
    if (count == 0)
        return;
    PrRect overlay = prLayoutRectToPx(prLayoutNodeBounds(ui->tree, overlayOf(ui), PrLayoutCoordinates_Viewport));
    PrVec2 origin{overlay.left, overlay.top};
    for (uint32_t i = 0; i < count && i < kUiDuplicatesShown; ++i) {
        outlineDuplicate(ui, duplicates[i].first, origin);
        outlineDuplicate(ui, duplicates[i].second, origin);
    }
}

// Returns the identity of `base` that the keys made under it mix: its key if it has one, its node otherwise, and 0 for
// no base. The two kinds of identity are mixed with different values, so that a key never equals a node.
uint64_t baseIdentity(const Ui* ui, PrLayoutNode base) {
    if (base == kPrLayoutNodeNone)
        return 0;
    uint64_t key = prLayoutNodeKey(ui->tree, base);
    return key != 0 ? hashMixKeys(key, 1) : hashMixKeys(base, 2);
}

// Begins a round. Takes over what the previous round passed to the next, and finds the node under the pointer in the
// committed tree.
void beginRound(Ui* ui, uint32_t index, const PrUiFrameDesc* desc) {
    ++ui->roundId;
    ui->roundDisables = false;
    ui->round = PrUiRound{.time = desc->time, .delta = index == 0 ? desc->delta : 0, .index = index};
    ui->time = desc->time;
    ui->routed.clear();
    ui->wheels.clear();
    ui->contexts.clear();
    ui->block = 0;
    ui->blockUsed = 0;
    ui->transitioned = false;
    ui->keyed = UiClaim{};
    ui->keyActivated = kPrLayoutNodeNone;
    ui->dropped = kPrLayoutNodeNone;
    ui->tweening = false;
    for (uint32_t b = 0; b < kUiButtons; ++b) {
        ui->clicked[b] = kPrLayoutNodeNone;
        ui->pressedRound[b] = false;
        UiButton& button = ui->buttons[b];
        button.delta = PrVec2{};
        button.released = false;
        button.cancelled = false;
        if (!button.dragging)
            button.node = kPrLayoutNodeNone;
    }
    Array<PrLayoutNode> swap = ui->activations;
    ui->activations = ui->nextActivations;
    ui->nextActivations = swap;
    ui->nextActivations.clear();
    ui->hovered = ui->hasPointer ? hitAt(ui, ui->pointer) : kPrLayoutNodeNone;
    // The memo's positions start again with the round's first lookup.
    ui->memo.slot = kPrLayoutFrameCursorEnd;
    ui->memo.calls = 0;
    ui->memo.at = kUiNone;
    ageGroups(ui);
}

// Routes queued events until the round reaches its second transition.
void routeQueue(Ui* ui) {
    if (ui->built == 0)
        return;
    while (ui->consumed < ui->queue.count) {
        const UiQueued& queued = ui->queue.data[ui->consumed];
        bool transition = isTransition(queued.event.kind);
        if (transition && ui->transitioned)
            break;
        ui->transitioned = ui->transitioned || transition;
        PrAppEvent event = queued.event;
        if (event.text.size != 0)
            event.text.data = ui->queueText.data + queued.textStart;
        ++ui->consumed;
        route(ui, event);
    }
}

// Ends a round's build. Resolves focus, capture and the cursor against the nodes that the build declared, and frees the
// states that it did not ask for.
void endRound(Ui* ui) {
    auto declaredTaking = [&](PrLayoutNode node, PrUiFlags flag) {
        const UiSlot* s = findSlot(ui, node);
        return s != nullptr && s->flagsRound == ui->roundId && (s->flags & flag) != 0 && (s->flags & PrUiFlag_Disabled) == 0;
    };
    if (ui->focusRequested) {
        ui->focus = declaredTaking(ui->focusRequest, PrUiFlag_Focusable) ? ui->focusRequest : kPrLayoutNodeNone;
        ui->focusRequested = false;
    }
    if (!declaredTaking(ui->focus, PrUiFlag_Focusable))
        ui->focus = kPrLayoutNodeNone;
    for (uint32_t b = 0; b < kUiButtons; ++b) {
        UiButton& button = ui->buttons[b];
        if (button.capture != kPrLayoutNodeNone && !declaredTaking(button.capture, PrUiFlag_Pressable)) {
            button.capture = kPrLayoutNodeNone;
            endDrag(button, false);
            if (b == 0)
                ui->payload = UiCarry{};
        }
    }
    // While a node holds the primary press, its cursor stays wherever the pointer goes, as a drag's does.
    ui->cursor = PrAppCursor_Arrow;
    PrLayoutNode from = ui->buttons[0].capture != kPrLayoutNodeNone ? ui->buttons[0].capture : ui->hovered;
    for (PrLayoutNode at = from; at != kPrLayoutNodeNone; at = parentOf(ui, at)) {
        const UiSlot* s = findSlot(ui, at);
        if (s != nullptr && s->cursorRound == ui->roundId) {
            ui->cursor = s->cursor;
            break;
        }
    }
    sweepStates(ui);
    Array<UiClaim> claims = ui->lastClaims;
    ui->lastClaims = ui->claims;
    ui->claims = claims;
    ui->claims.clear();
    Array<PrLayoutNode> targets = ui->lastDropTargets;
    ui->lastDropTargets = ui->dropTargets;
    ui->dropTargets = targets;
    ui->dropTargets.clear();
    ui->dropOver = ui->payload.carried ? dropTargetOf(ui, ui->hovered) : kPrLayoutNodeNone;
}

// Orders the round's routed events by node, so that the events of each node form one contiguous range. Each node's
// events stay in the order they were routed.
void groupRouted(Ui* ui) {
    Array<UiRouted>& routed = ui->routed;
    for (uint32_t i = 1; i < routed.count; ++i) {
        UiRouted moving = routed.data[i];
        uint32_t at = i;
        // A node's first event determines where its group goes, so the groups stay in the order of their first events.
        uint32_t first = 0;
        while (first < at && routed.data[first].node != moving.node)
            ++first;
        if (first == at)
            continue;
        uint32_t end = first;
        while (end < at && routed.data[end].node == moving.node)
            ++end;
        memMove(&routed.data[end + 1], &routed.data[end], size_t(at - end) * sizeof(UiRouted));
        routed.data[end] = moving;
    }
    ui->events.clear();
    for (uint32_t i = 0; i < routed.count; ++i)
        ui->events.push(routed.data[i].event);
}

// Removes the events that the frame routed from the queue, and keeps the events after them.
void compactQueue(Ui* ui) {
    uint32_t kept = 0;
    uint32_t text = 0;
    for (uint32_t i = ui->consumed; i < ui->queue.count; ++i) {
        UiQueued queued = ui->queue.data[i];
        uint32_t size = uint32_t(queued.event.text.size);
        if (size != 0)
            memMove(ui->queueText.data + text, ui->queueText.data + queued.textStart, size);
        queued.textStart = text;
        text += size;
        ui->queue.data[kept++] = queued;
    }
    ui->queue.count = kept;
    ui->queueText.resize(text);
    ui->consumed = 0;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrUi : Ui {};

PrUi* prUiCreate(PrLayoutTree* tree) {
    PrUi* ui = allocZeroed<PrUi>();
    ui->tree = tree;
    ui->display = prAppDisplayDefault();
    ui->cursor = PrAppCursor_Arrow;
    return ui;
}

void prUiDestroy(PrUi* PR_NULLABLE ui) {
    if (ui == nullptr)
        return;
    for (uint32_t i = 0; i < ui->states.count; ++i) {
        const UiState& state = ui->states.data[i];
        if (state.destroy != nullptr)
            state.destroy(state.data);
        prMemoryFree(state.data);
    }
    ui->slots.reset();
    ui->states.reset();
    ui->freeStates.reset();
    ui->queue.reset();
    ui->queueText.reset();
    ui->routed.reset();
    ui->events.reset();
    ui->wheels.reset();
    ui->dropTargets.reset();
    ui->lastDropTargets.reset();
    ui->claims.reset();
    ui->lastClaims.reset();
    ui->activations.reset();
    ui->nextActivations.reset();
    ui->contexts.reset();
    for (uint32_t i = 0; i < ui->blocks.count; ++i)
        prMemoryFree(ui->blocks.data[i].data);
    ui->blocks.reset();
    for (uint32_t i = 0; i < ui->groups.entries.count; ++i)
        releaseGroup(ui, ui->groups.entries.data[i]);
    ui->groups.entries.reset();
    ui->groups.bytes.reset();
    ui->groups.index.reset();
    for (uint32_t i = 0; i < ui->memo.entries.count; ++i) {
        if (ui->memo.entries.data[i].type != nullptr)
            releaseMemoEntry(ui, ui->memo.entries.data[i]);
    }
    ui->memo.entries.reset();
    ui->memo.bytes.reset();
    if (ui->overlayStyle != 0)
        prLayoutStyleRelease(ui->tree, ui->overlayStyle);
    prMemoryFree(ui);
}

PrLayoutTree* prUiTree(const PrUi* ui) {
    return ui->tree;
}

void prUiSetApp(PrUi* ui, PrApp* PR_NULLABLE app) {
    ui->app = app;
}

PrApp* PR_NULLABLE prUiApp(const PrUi* ui) {
    return ui->app;
}

void prUiPushEvent(PrUi* ui, const PrAppEvent* event) {
    PR_ASSERT(!ui->building);
    UiQueued queued{.event = *event, .textStart = ui->queueText.count};
    queued.event.text = PrStringView{nullptr, event->text.size};
    if (event->text.size != 0)
        ui->queueText.append(event->text.data, uint32_t(event->text.size));
    ui->queue.push(queued);
}

uint32_t prUiFrame(PrUi* ui, const PrUiFrameDesc* desc, PrUiBuildFn build, void* PR_NULLABLE context) {
    PR_ASSERT(!ui->building);
    PR_ASSERT(desc->display.pixelRatio > 0);
    ui->display = desc->display;
    ui->preferences = desc->preferences;
    if (prLayoutTreeTargetPixelRatio(ui->tree) != desc->display.pixelRatio)
        prLayoutTreeSetTargetPixelRatio(ui->tree, desc->display.pixelRatio);
    uint32_t limit = desc->maxRounds != 0 ? desc->maxRounds : 1;
    ui->frameRequested = false;
    ui->repeating = false;
    ui->hasTextInput = false;
    uint32_t rounds = 0;
    bool settle = false;
    PrLayoutVec extent = prLayoutVecFromPx(desc->extent);
    do {
        beginRound(ui, rounds, desc);
        routeQueue(ui);
        // The hover and focus chains reflect the pointer and focus as the round's input left them.
        stampHover(ui);
        stampFocus(ui);
        groupRouted(ui);
        prLayoutTreeBeginFrame(ui->tree);
        ui->building = true;
        build(context, ui, &ui->round);
        outlineDuplicates(ui);
        ui->building = false;
        endRound(ui);
        // The first root that the build created is the viewport's.
        PrLayoutRoot viewport{kPrLayoutNodeNone, extent};
        prLayoutTreeFrameRoots(ui->tree, &viewport.node, 1);
        prLayoutTreeLayout(ui->tree, &viewport, 1);
        ui->built = ui->roundId;
        settle = ui->transitioned;
        ++rounds;
    } while (rounds < limit && (ui->consumed < ui->queue.count || settle));
    compactQueue(ui);
    return rounds;
}

const PrUiRound* prUiCurrentRound(const PrUi* ui) {
    PR_ASSERT(ui->building);
    return &ui->round;
}

PrAppDisplay prUiDisplay(const PrUi* ui) {
    return ui->display;
}

PrAppUserPreferences prUiUserPreferences(const PrUi* ui) {
    return ui->preferences;
}

void prUiRequestFrame(PrUi* ui) {
    ui->frameRequested = true;
}

void prUiRequestMotion(PrUi* ui, PrLayoutNode node) {
    PrLayoutRect bounds = prLayoutNodeBounds(ui->tree, node, PrLayoutCoordinates_Viewport);
    bool laidOut = bounds.right.raw > bounds.left.raw || bounds.bottom.raw > bounds.top.raw;
    PrLayoutRect shown = prLayoutNodeShownBounds(ui->tree, node);
    if (!laidOut || shown.right.raw > shown.left.raw)
        prUiRequestFrame(ui);
}

PrUiKey prUiKey(const PrUi* ui, PrLayoutNode base, PrStringView name) {
    return hashMixKeys(baseIdentity(ui, base), hashBytes(name.data, name.size, 0x6e616d65ull));
}

PrUiKey prUiKeyIndex(const PrUi* ui, PrLayoutNode base, uint64_t index) {
    return hashMixKeys(baseIdentity(ui, base), hashMixKeys(index, 0x696e646578ull));
}

PrUiKey prUiKeyIndex2(const PrUi* ui, PrLayoutNode base, uint64_t first, uint64_t second) {
    return hashMixKeys(baseIdentity(ui, base), hashMixKeys(hashMixKeys(first, second), 0x696e64657832ull));
}

void prUiSetFlags(PrUi* ui, PrLayoutNode node, PrUiFlags flags) {
    setFlags(ui, node, flags);
}

void prUiAddFlags(PrUi* ui, PrLayoutNode node, PrUiFlags flags) {
    prUiSetFlags(ui, node, prUiFlags(ui, node) | flags);
}

PrUiFlags prUiFlags(const PrUi* ui, PrLayoutNode node) {
    const UiSlot* s = findSlot(ui, node);
    return s != nullptr && s->flagsRound == ui->roundId ? s->flags : PrUiFlags(0);
}

PrLayoutNode prUiElement(PrUi* ui, PrLayoutNode parent, PrUiKey key, PrLayoutStyleIndex style, PrUiFlags flags) {
    PrLayoutNode node = prLayoutTreeFrameElement(ui->tree, parent, key, style);
    if (flags != 0)
        prUiSetFlags(ui, node, flags);
    return node;
}

bool prUiIsDisabled(PrUi* ui, PrLayoutNode node) {
    PR_ASSERT(ui->building);
    return disabledWithin(ui, node);
}

PrLayoutNode prUiOverlay(PrUi* ui) {
    return overlayOf(ui);
}

void* prUiProvide(PrUi* ui, PrLayoutNode node, const void* owner, uint64_t key, uint32_t size) {
    PR_ASSERT(ui->building);
    UiSlot& s = slotOf(ui, node);
    if (s.contextRound != ui->roundId) {
        s.contextRound = ui->roundId;
        s.context = kUiNone;
    }
    for (uint32_t at = s.context; at != kUiNone; at = ui->contexts.data[at].next)
        PR_ASSERT(ui->contexts.data[at].owner != owner || ui->contexts.data[at].key != key);
    void* data = allocContext(ui, size);
    ui->contexts.push(UiContext{.owner = owner, .key = key, .data = data, .next = s.context});
    slotOf(ui, node).context = ui->contexts.count - 1;
    return data;
}

void* PR_NULLABLE prUiContext(const PrUi* ui, PrLayoutNode node, const void* owner, uint64_t key,
        PrLayoutNode* PR_NULLABLE provider) {
    PR_ASSERT(ui->building);
    for (PrLayoutNode at = node; at != kPrLayoutNodeNone; at = prLayoutTreeFrameParent(ui->tree, at)) {
        const UiSlot* s = findSlot(ui, at);
        if (s == nullptr || s->contextRound != ui->roundId)
            continue;
        for (uint32_t c = s->context; c != kUiNone; c = ui->contexts.data[c].next) {
            const UiContext& context = ui->contexts.data[c];
            if (context.owner == owner && context.key == key) {
                if (provider != nullptr)
                    *provider = at;
                return context.data;
            }
        }
    }
    if (provider != nullptr)
        *provider = kPrLayoutNodeNone;
    return nullptr;
}

void prUiSignal(PrUi* ui, PrLayoutNode node, const void* owner, uint32_t signals) {
    PR_ASSERT(ui->building);
    UiSlot& s = slotOf(ui, node);
    if (s.signalRound != ui->roundId) {
        s.signalRound = ui->roundId;
        s.signals = 0;
        s.signalOwner = owner;
    }
    PR_ASSERT(s.signalOwner == owner);
    s.signals |= signals;
}

bool prUiSignaled(const PrUi* ui, PrLayoutNode node, const void* owner, uint32_t signals) {
    const UiSlot* s = findSlot(ui, node);
    return s != nullptr && s->signalRound == ui->roundId && s->signalOwner == owner && (s->signals & signals) != 0;
}

bool prUiPointer(const PrUi* ui, PrVec2* out) {
    *out = ui->pointer;
    return ui->hasPointer;
}

bool prUiLocalPoint(const PrUi* ui, PrLayoutNode node, PrVec2 point, PrVec2* out) {
    PrLayoutVec local{};
    if (!prLayoutNodeLocalPoint(ui->tree, node, prLayoutVecFromPx(point), &local))
        return false;
    *out = prLayoutVecToPx(local);
    return true;
}

bool prUiIsHovered(const PrUi* ui, PrLayoutNode node) {
    const UiSlot* s = findSlot(ui, node);
    return s != nullptr && s->hoverRound == ui->roundId;
}

double prUiHoverSeconds(const PrUi* ui, PrLayoutNode node) {
    const UiSlot* s = findSlot(ui, node);
    return s != nullptr && s->hoverRound == ui->roundId ? ui->time - s->hoverSince : 0;
}

void prUiSetCursor(PrUi* ui, PrLayoutNode node, PrAppCursor cursor) {
    UiSlot& s = slotOf(ui, node);
    s.cursor = cursor;
    s.cursorRound = ui->roundId;
}

void prUiSetTextInput(PrUi* ui, PrRect caret) {
    ui->hasTextInput = true;
    ui->textInput = caret;
}

PrAppOutput prUiOutput(const PrUi* ui) {
    return PrAppOutput{
        .cursor = ui->cursor,
        .wantsFrame = ui->frameRequested || ui->tweening || ui->repeating || ui->nextActivations.count != 0 ||
            ui->queue.count != 0,
        .hasTextInput = ui->hasTextInput,
        .textInput = ui->textInput,
    };
}

bool prUiIsPressed(const PrUi* ui, PrLayoutNode node, uint8_t button) {
    return button < kUiButtons && node != kPrLayoutNodeNone && ui->buttons[button].capture == node;
}

bool prUiClicked(const PrUi* ui, PrLayoutNode node, uint8_t button) {
    return button < kUiButtons && node != kPrLayoutNodeNone && ui->clicked[button] == node;
}

PrAppModifierFlags prUiPressModifiers(const PrUi* ui, uint8_t button) {
    return button < kUiButtons ? ui->buttons[button].modifiers : PrAppModifierFlags(0);
}

uint32_t prUiPressCount(const PrUi* ui, uint8_t button) {
    return button < kUiButtons ? ui->buttons[button].presses : 0;
}

bool prUiPressedOutside(const PrUi* ui, PrLayoutNode node, uint8_t button) {
    if (button >= kUiButtons || !ui->pressedRound[button])
        return false;
    const UiSlot* s = findSlot(ui, node);
    return s == nullptr || s->pressRound != ui->roundId;
}

uint32_t prUiPressRepeats(PrUi* ui, PrLayoutNode node, uint8_t button, double delay, double interval) {
    if (!prUiIsPressed(ui, node, button) || interval <= 0)
        return 0;
    UiButton& b = ui->buttons[button];
    ui->repeating = true;
    auto ticks = [&](double at) -> double {
        double since = at - b.pressTime - delay;
        return since < 0 ? 0 : 1 + double(uint64_t(since / interval));
    };
    double from = ticks(b.repeatTime);
    double to = ticks(ui->time);
    b.repeatTime = ui->time;
    return to > from ? uint32_t(to - from) : 0;
}

bool prUiActivated(const PrUi* ui, PrLayoutNode node) {
    if (node == kPrLayoutNodeNone || (prUiFlags(ui, node) & PrUiFlag_Disabled) != 0)
        return false;
    if (ui->clicked[0] == node || ui->keyActivated == node)
        return true;
    for (uint32_t i = 0; i < ui->activations.count; ++i) {
        if (ui->activations.data[i] == node)
            return true;
    }
    return false;
}

void prUiActivate(PrUi* ui, PrLayoutNode node) {
    ui->nextActivations.push(node);
    ui->frameRequested = true;
}

bool prUiDrag(const PrUi* ui, PrLayoutNode node, uint8_t button, float threshold, PrUiDrag* out) {
    *out = PrUiDrag{};
    if (button >= kUiButtons || node == kPrLayoutNodeNone || ui->buttons[button].node != node)
        return false;
    const UiButton& b = ui->buttons[button];
    if (!b.dragging && !b.released && !b.cancelled)
        return false;
    *out = PrUiDrag{
        .origin = b.origin,
        .position = b.position,
        .delta = b.delta,
        .active = b.dragging,
        .moved = b.reach > threshold * threshold,
        .released = b.released,
        .cancelled = b.cancelled,
    };
    return true;
}

bool prUiWheel(const PrUi* ui, PrLayoutNode node, PrVec2* out) {
    for (uint32_t i = 0; i < ui->wheels.count; ++i) {
        if (ui->wheels.data[i].node == node) {
            *out = ui->wheels.data[i].delta;
            return true;
        }
    }
    *out = PrVec2{};
    return false;
}

void prUiDragStart(PrUi* ui, PrLayoutNode source, float threshold, uint64_t kind, uint64_t value) {
    if (!prUiIsPressed(ui, source, 0) || ui->payload.armed)
        return;
    ui->payload = UiCarry{
        .payload = {.kind = kind, .value = value, .source = source},
        .threshold = threshold,
        .armed = true,
    };
    carry(ui);
}

const PrUiPayload* PR_NULLABLE prUiDragging(const PrUi* ui) {
    return ui->payload.carried ? &ui->payload.payload : nullptr;
}

bool prUiIsDropTarget(PrUi* ui, PrLayoutNode node) {
    ui->dropTargets.push(node);
    return node != kPrLayoutNodeNone && ui->dropOver == node;
}

const PrUiPayload* PR_NULLABLE prUiDropped(const PrUi* ui, PrLayoutNode node) {
    return node != kPrLayoutNodeNone && ui->dropped == node ? &ui->droppedPayload : nullptr;
}

bool prUiKeyPressed(PrUi* ui, PrLayoutNode node, PrAppKey key, PrAppModifierFlags modifiers) {
    PR_ASSERT(ui->building);
    ui->claims.push(UiClaim{node, key, modifiers, false});
    return ui->keyed.node == node && node != kPrLayoutNodeNone && ui->keyed.key == key &&
        ui->keyed.modifiers == modifiers && !ui->keyed.capture;
}

bool prUiKeyCaptured(PrUi* ui, PrLayoutNode node, PrAppKey key, PrAppModifierFlags modifiers) {
    PR_ASSERT(ui->building);
    ui->claims.push(UiClaim{node, key, modifiers, true});
    return ui->keyed.node == node && node != kPrLayoutNodeNone && ui->keyed.key == key &&
        ui->keyed.modifiers == modifiers && ui->keyed.capture;
}

PrUiEvents prUiEvents(const PrUi* ui, PrLayoutNode node) {
    uint32_t first = 0;
    while (first < ui->routed.count && ui->routed.data[first].node != node)
        ++first;
    uint32_t end = first;
    while (end < ui->routed.count && ui->routed.data[end].node == node)
        ++end;
    if (first == end)
        return PrUiEvents{nullptr, 0};
    return PrUiEvents{&ui->events.data[first], end - first};
}

PrLayoutNode prUiFocus(const PrUi* ui) {
    return ui->focus;
}

void prUiSetFocus(PrUi* ui, PrLayoutNode node) {
    if (node == kPrLayoutNodeNone) {
        ui->focus = kPrLayoutNodeNone;
        ui->focusRequested = false;
        return;
    }
    ui->focusRequest = node;
    ui->focusRequested = true;
}

bool prUiIsFocusWithin(const PrUi* ui, PrLayoutNode node) {
    const UiSlot* s = findSlot(ui, node);
    return s != nullptr && s->focusRound == ui->roundId && ui->focus != kPrLayoutNodeNone;
}

bool prUiIsFocusVisible(const PrUi* ui) {
    return ui->focusVisible;
}

PrLayoutNode prUiFocusMove(PrUi* ui, PrLayoutNode scope, bool reverse) {
    PrLayoutNode next = focusStep(ui, ui->focus, scope, reverse);
    prUiSetFocus(ui, next);
    return next;
}

void* prUiState(PrUi* ui, PrLayoutNode node, const void* owner, uint64_t key, uint32_t size) {
    return stateOf(ui, node, owner, key, size).data;
}

void* prUiStateWithDestroy(PrUi* ui, PrLayoutNode node, const void* owner, uint64_t key, uint32_t size,
        PrUiStateDestroyFn destroy) {
    UiState& state = stateOf(ui, node, owner, key, size);
    state.destroy = destroy;
    return state.data;
}

float prUiTween(PrUi* ui, PrLayoutNode node, const void* owner, uint64_t key, float target, double duration,
        const PrLayoutEasing* PR_NULLABLE easing) {
    UiTween* t = static_cast<UiTween*>(stateOf(ui, node, owner, key, sizeof(UiTween)).data);
    if (!t->started || duration <= 0) {
        *t = UiTween{.from = target, .target = target, .value = target, .start = ui->time, .started = true};
        return target;
    }
    if (t->target != target) {
        t->from = t->value;
        t->target = target;
        t->start = ui->time;
    }
    double progress = (ui->time - t->start) / duration;
    if (progress >= 1) {
        t->value = target;
        return target;
    }
    PrLayoutEasing linear = prLayoutEasingLinear();
    double eased = prLayoutEase(easing != nullptr ? easing : &linear, progress < 0 ? 0 : progress, false);
    t->value = float(double(t->from) + (double(target) - double(t->from)) * eased);
    ui->tweening = true;
    return t->value;
}

void prUiSetEditSink(PrUi* ui, PrLayoutNode node, PrUiEditFn PR_NULLABLE fn, void* PR_NULLABLE context) {
    PrLayoutNode provider = kPrLayoutNodeNone;
    void* found = prUiContext(ui, node, &kUiOwner, UiContextKey_EditSink, &provider);
    void* data;
    if (found != nullptr && provider == node)
        data = found;
    else
        data = prUiProvide(ui, node, &kUiOwner, UiContextKey_EditSink, uint32_t(sizeof(UiEditSink)));
    *static_cast<UiEditSink*>(data) = UiEditSink{.fn = fn, .context = fn != nullptr ? context : nullptr};
}

void prUiEdit(PrUi* ui, PrLayoutNode control, PrUiEditPhase phase) {
    const UiEditSink* sink = static_cast<const UiEditSink*>(prUiContext(ui, control, &kUiOwner, UiContextKey_EditSink, nullptr));
    if (sink != nullptr && sink->fn != nullptr)
        sink->fn(sink->context, control, phase);
}

PrLayoutGroupIndex prUiGroup(PrUi* ui, const PrUiGroupRecipe* recipe) {
    UiGroups& groups = ui->groups;
    uint64_t seed = hashBytes(&recipe->owner, sizeof recipe->owner, reinterpret_cast<uintptr_t>(recipe->type));
    uint32_t hash = uint32_t(hashBytes(recipe->bytes, recipe->size, seed));
    uint32_t at = groups.index.find(hash, [&](uint32_t i) {
        const UiGroupEntry& e = groups.entries.data[i];
        return e.type == recipe->type && e.owner == recipe->owner && e.size == recipe->size &&
            memEquals(groups.bytes.data + e.offset, recipe->bytes, recipe->size);
    });
    if (at != kHashMissing) {
        UiGroupEntry& e = groups.entries.data[at];
        e.used = ui->roundId;
        return e.group;
    }
    PrLayoutGroupIndex group = makeGroup(ui, *recipe);
    prLayoutGroupRetain(ui->tree, recipe->kind, group);
    if (recipe->base != 0)
        prLayoutGroupRetain(ui->tree, recipe->kind, recipe->base);
    uint32_t offset = groups.bytes.count;
    groups.bytes.append(static_cast<const uint8_t*>(recipe->bytes), recipe->size);
    groups.index.insert(hash, groups.entries.count);
    groups.entries.push(UiGroupEntry{
        .type = recipe->type,
        .owner = recipe->owner,
        .hash = hash,
        .offset = offset,
        .size = recipe->size,
        .used = ui->roundId,
        .group = group,
        .base = recipe->base,
        .kind = recipe->kind,
    });
    return group;
}

const void* PR_NULLABLE prUiMemoFind(PrUi* ui, const void* type, const void* PR_NULLABLE owner, PrLayoutStyleIndex* out) {
    PR_ASSERT(ui->building);
    UiMemo& memo = ui->memo;
    uint32_t slot = prLayoutTreeFrameCursor(ui->tree);
    memo.calls = slot == memo.slot ? memo.calls + 1 : 0;
    memo.slot = slot;
    memo.at = kUiNone;
    if (slot == kPrLayoutFrameCursorEnd || memo.calls >= kUiMemoCalls)
        return nullptr;
    uint32_t at = slot * kUiMemoCalls + memo.calls;
    while (memo.entries.count <= at)
        memo.entries.push(UiMemoEntry{});
    memo.at = at;
    const UiMemoEntry& e = memo.entries.data[at];
    if (e.type != type || e.owner != owner)
        return nullptr;
    *out = e.style;
    return memo.bytes.data + e.offset;
}

PrLayoutStyleIndex prUiMemoKeep(PrUi* ui, const PrUiPart* part, PrLayoutStyleIndex style, PrLayoutStyleIndex base) {
    UiMemo& memo = ui->memo;
    if (memo.at == kUiNone)
        return style;
    UiMemoEntry& e = memo.entries.data[memo.at];
    memo.at = kUiNone;
    prLayoutStyleRetain(ui->tree, style);
    if (base != 0)
        prLayoutStyleRetain(ui->tree, base);
    if (e.type != nullptr)
        releaseMemoEntry(ui, e);
    if (e.capacity < part->size) {
        e.offset = memo.bytes.count;
        e.capacity = part->size;
        memo.bytes.reserve(memo.bytes.count + part->size);
        memo.bytes.count += part->size;
    }
    memCopy(memo.bytes.data + e.offset, part->bytes, part->size);
    e.type = part->type;
    e.owner = part->owner;
    e.size = part->size;
    e.style = style;
    e.base = base;
    return style;
}

void prUiForget(PrUi* ui, const void* owner) {
    PR_ASSERT(!ui->building);
    filterGroups(ui, [&](const UiGroupEntry& e) { return e.owner != owner; });
    for (uint32_t i = 0; i < ui->memo.entries.count; ++i) {
        UiMemoEntry& e = ui->memo.entries.data[i];
        if (e.type != nullptr && e.owner == owner)
            releaseMemoEntry(ui, e);
    }
}

void prUiReveal(PrUi* ui, PrLayoutNode node) {
    if (prLayoutNodeIsValid(ui->tree, node))
        prUiRevealRect(ui, node, prLayoutNodeBounds(ui->tree, node, PrLayoutCoordinates_Viewport));
}

void prUiRevealRect(PrUi* ui, PrLayoutNode node, PrLayoutRect rect) {
    PrLayoutVec offset{};
    while ((node = prLayoutNodeRevealStep(ui->tree, node, &rect, &offset)) != kPrLayoutNodeNone)
        prLayoutNodeSetScroll(ui->tree, node, offset);
}

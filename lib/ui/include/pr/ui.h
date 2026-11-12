// Immediate-mode interaction: input routing, focus, pointer capture, scrolling and retained state for a layout tree
// that the application declares in full every round (pr/layout_frame.h). It also holds what the component libraries
// over it share, so that several libraries can build one tree: keys, the overlay, edit gestures and records.
//
// A context is bound to one tree. Each round has two steps. First, the context routes queued input against the tree
// committed by the previous round, using hit testing in paint order. Then it calls the application's build function,
// which creates the tree again, sets each node's input flags, and queries the input that reached each node. A node that
// the frame matches to a committed node keeps its handle, so the handle the build holds is the same handle that input
// was routed to. During a build, queries report the input routed in the current round, and geometry queries report the
// previous round's layout.
//
// A context and its tree must be used from one thread at a time.

#pragma once

#include "pr/app.h"
#include "pr/app_display.h"
#include "pr/app_user_preferences.h"
#include "pr/base.h"
#include "pr/layout_animation.h"
#include "pr/layout_frame.h"
#include "pr/layout_tree.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Contexts

// Input routing and retained state for one layout tree.
typedef struct PrUi PrUi;

// Creates a context for `tree`. The tree must outlive the context.
PrUi* prUiCreate(PrLayoutTree* tree);

// Destroys the context and any input still queued.
void prUiDestroy(PrUi* PR_NULLABLE ui);

// Returns the tree that the context routes input against.
PrLayoutTree* prUiTree(const PrUi* ui);

// Sets the application whose clipboard the context's widgets cut, copy and paste through, or none, or returns it.
// Without one, a cut or a copy leaves the clipboard as it was, and a paste pastes nothing.
void prUiSetApp(PrUi* ui, PrApp* PR_NULLABLE app);
PrApp* PR_NULLABLE prUiApp(const PrUi* ui);

// Copies an event and its text into the queue for the next frame. Must not be called during a build.
void prUiPushEvent(PrUi* ui, const PrAppEvent* event);

// ---------------------------------------------------------------------------------------------------------------------
// Frames and rounds

// The timing of one round. All rounds in a frame share the frame's time. Only the first round carries the frame's
// delta.
typedef struct PrUiRound {
    double time;                    // seconds, from the host's monotonic clock
    double delta;                   // seconds since the previous frame in the first round, and zero in later rounds
    uint32_t index;                 // position of this round within the frame, starting at 0
} PrUiRound;

// Declares the whole tree for one round. The tree's frame is already open when the function is called. The function
// creates every node, the first that it creates with no parent being the root that the round lays out in the viewport,
// and may update application state in response to the round's input. It must not run a frame, and must not keep
// pointers returned by event queries after it returns.
typedef void (*PrUiBuildFn)(void* PR_NULLABLE context, PrUi* ui, const PrUiRound* round);

// Parameters for one frame.
typedef struct PrUiFrameDesc {
    double time;                    // seconds, from the host's monotonic clock
    double delta;                   // seconds since the previous frame
    PrVec2 extent;                  // viewport size in px, used for each round's layout
    uint32_t maxRounds;             // upper limit on rounds in the frame; 0 means 1

    // The display that the frame is shown on, whose pixel ratio the tree lays out for, and the user's preferences. A
    // build reads them through prUiDisplay and prUiUserPreferences.
    PrAppDisplay display;
    PrAppUserPreferences preferences;
} PrUiFrameDesc;

// Runs a frame of up to `desc->maxRounds` rounds. Each round routes input, calls `build`, and lays out the tree.
//
// A round routes queued events in order and stops before an event that would make a query ambiguous: a second press or
// release of the same button, or a second key press. After a round that routes a press, a release or a key press, the
// next round routes nothing. This ensures the final tree of the frame reflects the state that the input produced. The
// first round of a new context routes nothing, because there is no layout to hit test yet. Input left over when the
// round limit is reached stays queued for the next frame.
//
// The host paints the tree after this returns. Returns the number of rounds run.
uint32_t prUiFrame(PrUi* ui, const PrUiFrameDesc* desc, PrUiBuildFn build, void* PR_NULLABLE context);

// Returns the round being built. Valid only during a build.
const PrUiRound* prUiCurrentRound(const PrUi* ui);

// Returns the display and the user's preferences of the frame that runs, or of the last frame between frames. Before
// the first frame they are prAppDisplayDefault and the neutral preferences.
PrAppDisplay prUiDisplay(const PrUi* ui);
PrAppUserPreferences prUiUserPreferences(const PrUi* ui);

// Requests another frame, which prUiOutput reports until the next frame begins.
void prUiRequestFrame(PrUi* ui);

// Requests another frame for an animation on `node` that runs without input, such as a spinner's, if the node was shown
// at the last layout (prLayoutNodeShownBounds) or has not been laid out yet. A node that is scrolled or clipped out of
// view requests no frames until it is shown again.
void prUiRequestMotion(PrUi* ui, PrLayoutNode node);

// Requests a frame at `time`, on the clock of the frame's time, which prUiOutput reports until the next frame begins.
// The earliest request of a frame stands. Nothing runs until then, so this is the form for what changes at a known
// time.
void prUiRequestFrameAt(PrUi* ui, double time);

// Requests a frame at `time` for a change on `node` that runs without input, such as a caret's blink, under the
// condition of prUiRequestMotion.
void prUiRequestMotionAt(PrUi* ui, PrLayoutNode node, double time);

// ---------------------------------------------------------------------------------------------------------------------
// Node keys
//
// Most nodes need no key: left zero, a node is identified by its tree index among its parent's unkeyed children of its
// kind. A key gives a node an identity that holds while its siblings change, for rows that reorder and panels that come
// and go (pr/layout_frame.h). Every library on a context makes its keys with prUiKey and prUiKeyIndex. Keys made under
// different bases differ, so one library's items never collide with another's, and an item of one library can be keyed
// under a container of another.
//
// A key that a frame gives twice is a caller's error. Layout makes the second node without the key and reports both
// nodes (prLayoutTreeDuplicateKeys), and each round of the next frame outlines both in the overlay.

// A node's key, which prUiKey, prUiKeyIndex and prUiKeyIndex2 make. Zero is no key. Nonzero keys are unique in the
// whole tree, so a number chosen for one parent collides with the same number chosen for another. A key made under a
// base is local to that base, and a base of kPrLayoutNodeNone makes a key for the whole tree.
typedef uint64_t PrUiKey;

// Returns a key made from `name`, or from `index`, under the node `base`. Keys made under one base differ for different
// names and indices, and keys made from one name differ for different bases. A base that has a key is identified by its
// key. A base without one is identified by its node, which stays the same while layout matches the node from round to
// round. A base of kPrLayoutNodeNone makes a key for the whole tree, for a node that keeps its identity wherever it is
// created. The result is never zero.
PrUiKey prUiKey(const PrUi* ui, PrLayoutNode base, PrStringView name);
PrUiKey prUiKeyIndex(const PrUi* ui, PrLayoutNode base, uint64_t index);

// Returns a key made from two numbers under the node `base`, for a library's several kinds of items of one base: the
// kind and the item. It differs from the keys of prUiKey and prUiKeyIndex under the base.
PrUiKey prUiKeyIndex2(const PrUi* ui, PrLayoutNode base, uint64_t first, uint64_t second);

// ---------------------------------------------------------------------------------------------------------------------
// Nodes

// The input a node accepts.
//
// A disabled node keeps its identity and retained state but receives no presses, focus or keys. It still blocks the
// pointer from reaching nodes beneath it. The nodes created under a disabled node are disabled too, so a container
// disables what it holds. A transparent node lets the pointer pass through to nodes beneath it. A subtree-transparent
// node does the same for itself and all of its descendants.
enum {
    PrUiFlag_Pressable = 1u << 0,           // receives presses and clicks, and captures the pointer while pressed
    PrUiFlag_Focusable = 1u << 1,           // receives focus from a primary press or from Tab traversal
    PrUiFlag_Disabled = 1u << 2,
    PrUiFlag_Scrollable = 1u << 3,          // a scroll container that the wheel scrolls
    PrUiFlag_Wheel = 1u << 4,               // receives wheel deltas through prUiWheel instead, as a canvas to pan
    PrUiFlag_FocusScope = 1u << 5,          // Tab traversal wraps within this node's subtree
    PrUiFlag_Transparent = 1u << 6,
    PrUiFlag_SubtreeTransparent = 1u << 7,
};
typedef uint8_t PrUiFlags;

// Sets the input flags of a node created in the current round. Flags do not carry over between rounds: a node has no
// flags until the round that creates it sets them. A node created under a disabled node takes PrUiFlag_Disabled as
// well, so a container sets its flags before it creates what it holds.
void prUiSetFlags(PrUi* ui, PrLayoutNode node, PrUiFlags flags);

// Adds `flags` to those that the current round set on a node, as a widget built on another widget adds the input that
// it takes.
void prUiAddFlags(PrUi* ui, PrLayoutNode node, PrUiFlags flags);

// Returns the flags that the current round set on a node, or 0 if none were set.
PrUiFlags prUiFlags(const PrUi* ui, PrLayoutNode node);

// Creates an element with prLayoutTreeFrameElement and sets its input flags.
PrLayoutNode prUiElement(PrUi* ui, PrLayoutNode parent, PrUiKey key, PrLayoutStyleIndex style, PrUiFlags flags);

// Returns true if a node created in the current round is disabled, by its own flags or those of a node that it was
// created under.
bool prUiIsDisabled(PrUi* ui, PrLayoutNode node);

// ---------------------------------------------------------------------------------------------------------------------
// Overlay

// Returns the round's overlay, a layer that covers the viewport's root and paints above everything in flow. The pointer
// passes through the overlay to what is beneath it, except where a node in the overlay takes it. Every library creates
// what floats over the application under the overlay, such as a menu, a tooltip or a drag's ghost.
//
// The first call in a round creates the overlay under the first root that the round created, which must already exist.
PrLayoutNode prUiOverlay(PrUi* ui);

// ---------------------------------------------------------------------------------------------------------------------
// Contexts and signals
//
// A container gives what is created under it a context: what a tab strip's tabs share, or the menu that a menu's items
// close. A widget tells its caller what happened to it by a signal on its node, which the caller asks of the node that
// the widget returned. Both are kept for the round, under an owner, as prUiState keeps storage: `owner` is any address
// that the calling library owns.

// Returns zeroed storage of `size` bytes, aligned to 16, that `node` provides under `owner` and `key` to the nodes
// created under it in the current round. The storage stays in place until the round ends. A node provides one context
// for each owner and key.
void* prUiProvide(PrUi* ui, PrLayoutNode node, const void* owner, uint64_t key, uint32_t size);

// Returns the context under `owner` and `key` that `node` or the nearest node it was created under provides in the
// current round, or null if none does. Writes the node that provides it to `provider`, if `provider` is not null.
void* PR_NULLABLE prUiContext(const PrUi* ui, PrLayoutNode node, const void* owner, uint64_t key,
    PrLayoutNode* PR_NULLABLE provider);

// Marks `signals`, bits that `owner` defines, as having happened to the node in the current round. A node takes the
// signals of one owner in a round.
void prUiSignal(PrUi* ui, PrLayoutNode node, const void* owner, uint32_t signals);

// Returns true if any of `signals` happened to the node in the current round under `owner`.
bool prUiSignaled(const PrUi* ui, PrLayoutNode node, const void* owner, uint32_t signals);

// ---------------------------------------------------------------------------------------------------------------------
// Pointer

// Writes the most recently routed pointer position, in viewport px, to `out`. Returns false if no pointer event has
// arrived yet.
bool prUiPointer(const PrUi* ui, PrVec2* out);

// Maps `point`, in viewport px, into the node's own coordinates as of the last layout: px from the top left of its
// first box, through every transform between them (prLayoutNodeLocalPoint). A widget measures a position within a box
// this way, such as a slider's value under the pointer, so that it holds under a rotation, a scale or a projection.
// Returns false if the node has no box, or if the point does not reach the box's plane.
bool prUiLocalPoint(const PrUi* ui, PrLayoutNode node, PrVec2 point, PrVec2* out);

// Returns true if the pointer is over the node or one of its descendants in the current round.
bool prUiIsHovered(const PrUi* ui, PrLayoutNode node);

// Returns how many seconds the pointer has stayed over the node or its descendants, or zero if it is not over them.
double prUiHoverSeconds(const PrUi* ui, PrLayoutNode node);

// Requests a cursor for while the pointer is over the node or its descendants. If several nodes request a cursor, the
// one closest to the node under the pointer wins. While a node holds the primary button's press, the cursor is the one
// closest to that node instead, wherever the pointer is.
void prUiSetCursor(PrUi* ui, PrLayoutNode node, PrAppCursor cursor);

// Sets where the caret of the text being edited stands in this frame, in logical px of the window.
void prUiSetTextInput(PrUi* ui, PrRect caret);

// Returns what the context reports to the window, which a frame handler returns or merges with its other surfaces: the
// cursor resolved in the last round, or the arrow if no node requested one, whether it wants another frame because a
// tween is moving, a press is repeating, an activation is pending, input is still queued, or a frame was requested, and
// the caret of the text being edited. Call between frames.
PrAppOutput prUiOutput(const PrUi* ui);

// Returns true if the node has captured the pointer with a press of `button`. Button 0 is the primary button.
bool prUiIsPressed(const PrUi* ui, PrLayoutNode node, uint8_t button);

// Returns true if `button` was released over the node in the current round, and the press of that button was captured
// by the same node.
bool prUiClicked(const PrUi* ui, PrLayoutNode node, uint8_t button);

// Returns the modifiers held at the most recent press of `button`. A click reports the modifiers of the press that
// started it.
PrAppModifierFlags prUiPressModifiers(const PrUi* ui, uint8_t button);

// Returns the number of presses in the sequence that the most recent press of `button` ends: 1 for a single press, 2
// for the second press of a double press, and so on. A press continues the sequence if the application reports a
// multi-click (PrAppPointerEvent::clicks) and it lands on the node that captured the press before it.
uint32_t prUiPressCount(const PrUi* ui, uint8_t button);

// Returns true if `button` was pressed in the current round outside the node and its descendants.
bool prUiPressedOutside(const PrUi* ui, PrLayoutNode node, uint8_t button);

// Returns how many auto-repeat ticks occurred in the current round while the node held a press of `button`. The first
// tick occurs `delay` seconds after the press, and each later tick `interval` seconds after the previous one. While a
// press is repeating, the context requests frames.
uint32_t prUiPressRepeats(PrUi* ui, PrLayoutNode node, uint8_t button, double delay, double interval);

// Returns true if the node was activated in the current round. A node is activated by a primary click, by Enter or
// Space while it has focus, or by prUiActivate. A disabled node is never activated.
bool prUiActivated(const PrUi* ui, PrLayoutNode node);

// Marks a node to be activated in the next round, and requests a frame.
void prUiActivate(PrUi* ui, PrLayoutNode node);

// The state of a pointer drag by a node that captured the pointer.
//
// `delta` is the motion in the current round. `moved` becomes true once the pointer has gone past the drag threshold,
// and stays true for the rest of the drag. `released` and `cancelled` are set only in the round that ends the drag.
typedef struct PrUiDrag {
    PrVec2 origin;
    PrVec2 position;
    PrVec2 delta;
    bool active;
    bool moved;
    bool released;
    bool cancelled;
} PrUiDrag;

// Writes the state of the drag of `button` captured by the node to `out`. `threshold` is the distance in px from the
// origin at which `moved` becomes true. Returns true while the drag is active, and in the round that ends it.
bool prUiDrag(const PrUi* ui, PrLayoutNode node, uint8_t button, float threshold, PrUiDrag* out);

// Writes the wheel deltas, in px, routed in the current round to a node with PrUiFlag_Wheel. Returns false if none were
// routed to it.
bool prUiWheel(const PrUi* ui, PrLayoutNode node, PrVec2* out);

// ---------------------------------------------------------------------------------------------------------------------
// Payloads

// The data a drag-and-drop operation carries. `kind` and `value` are chosen by the application. `source` is the node
// the drag started from.
typedef struct PrUiPayload {
    uint64_t kind;
    uint64_t value;
    PrLayoutNode source;
} PrUiPayload;

// Starts a drag-and-drop operation from a primary press held by `source`. The payload is picked up once the pointer
// moves `threshold` px, or immediately if `threshold` is 0. Escape cancels the drag, and losing pointer capture ends it
// without a drop. The release that ends the drag does not produce a click.
void prUiDragStart(PrUi* ui, PrLayoutNode source, float threshold, uint64_t kind, uint64_t value);

// Returns the payload being dragged, or null if there is none. The pointer is valid until the drag ends.
const PrUiPayload* PR_NULLABLE prUiDragging(const PrUi* ui);

// Makes the node a drop target for the current round. Returns true if it was the innermost drop target under the
// pointer in the previous round. A payload is never offered to its own source.
bool prUiIsDropTarget(PrUi* ui, PrLayoutNode node);

// Returns the payload dropped on the node in the current round, or null. A drop goes to the innermost drop target under
// the pointer at release.
const PrUiPayload* PR_NULLABLE prUiDropped(const PrUi* ui, PrLayoutNode node);

// ---------------------------------------------------------------------------------------------------------------------
// Keys, text and focus

// Claims `key` with exactly `modifiers` for the node, and returns true if such a key press was routed to the node in
// the current round.
//
// Claims made in one round decide routing in the next. A key press goes to the nearest node, starting at the focused
// node and walking up through its ancestors, that claimed the key. If nothing is focused, the walk starts at the root.
// If no node claimed the key, it is used for focus traversal and activation. A key press that the input method takes
// for a composition (PrAppKeyboardEvent::composing) is neither claimed nor captured. It goes to the focused node.
bool prUiKeyPressed(PrUi* ui, PrLayoutNode node, PrAppKey key, PrAppModifierFlags modifiers);

// Captures `key` with exactly `modifiers` for the node, and returns true if such a key press was routed to the node in
// the current round. A capture routes ahead of every claim: a key press goes to the outermost node, among the focused
// node and its ancestors, that captured the key, before any node claims it. An application captures the few keys that
// it reserves for itself, which then work even inside a text field that would claim them.
bool prUiKeyCaptured(PrUi* ui, PrLayoutNode node, PrAppKey key, PrAppModifierFlags modifiers);

// The input events routed to a node in the current round, in order. Pointer events go to the node that captured the
// pointer, or otherwise to the node under the pointer. Key and text events go to the focused node. The array is valid
// until the build returns.
typedef struct PrUiEvents {
    const PrAppEvent* PR_NULLABLE events;
    uint32_t count;
} PrUiEvents;
PrUiEvents prUiEvents(const PrUi* ui, PrLayoutNode node);

// Returns the focused node, or kPrLayoutNodeNone.
PrLayoutNode prUiFocus(const PrUi* ui);

// Requests focus for a node, or clears focus if `node` is kPrLayoutNodeNone. The request takes effect at the end of the
// round. If the node is focusable and not disabled in that round, it receives focus. Otherwise focus is cleared.
void prUiSetFocus(PrUi* ui, PrLayoutNode node);

// Returns true if the focused node is this node or one of its descendants.
bool prUiIsFocusWithin(const PrUi* ui, PrLayoutNode node);

// Returns true if focus should be drawn with a visible ring. A key press other than a modifier key's turns this on, and
// a pointer press turns it off.
bool prUiIsFocusVisible(const PrUi* ui);

// Moves focus to the next focusable node in tree order, or to the previous one if `reverse` is true, wrapping at the
// ends. The search is limited to the subtree of `scope`, or the whole tree if `scope` is kPrLayoutNodeNone. The move
// takes effect at the end of the round, as with prUiSetFocus. Returns the node that will receive focus.
PrLayoutNode prUiFocusMove(PrUi* ui, PrLayoutNode scope, bool reverse);

// ---------------------------------------------------------------------------------------------------------------------
// Retained state

// Returns zero-initialized storage of `size` bytes, aligned to 16, attached to the node under `owner` and `key`.
//
// The storage persists as long as every round asks for it, and is freed at the end of the first round that does not.
// `owner` is any address that the calling library owns, which keeps keys from different libraries apart. `size` must be
// the same on every call. The returned pointer is stable until the end of the round.
void* prUiState(PrUi* ui, PrLayoutNode node, const void* owner, uint64_t key, uint32_t size);

// Called with retained storage just before the storage is freed.
typedef void (*PrUiStateDestroyFn)(void* state);

// Returns storage as prUiState does, for state that holds resources. `destroy` releases them when the storage is freed,
// which also happens when the context is destroyed.
void* prUiStateWithDestroy(PrUi* ui, PrLayoutNode node, const void* owner, uint64_t key, uint32_t size,
    PrUiStateDestroyFn destroy);

// Returns an animated value attached to the node under `owner` and `key`. When `target` changes, the value moves from
// its current position to the new target over `duration` seconds, using `easing`, or linearly if `easing` is null. On
// the first call, or if `duration` is zero or less, the value is the target. The value persists under the same rules as
// prUiState.
float prUiTween(PrUi* ui, PrLayoutNode node, const void* owner, uint64_t key, float target, double duration,
    const PrLayoutEasing* PR_NULLABLE easing);

// ---------------------------------------------------------------------------------------------------------------------
// Edit gestures
//
// A control bound to the application's value, such as a field, a slider or a toggle, reports the boundaries of each
// edit gesture, such as one drag or one key press. A gesture begins before its first write, reports a change after each
// write, and commits once it ends, so that the application can record one undo entry for it, whichever library made the
// control. A gesture may commit with no change.

// The phases of an edit gesture.
enum {
    PrUiEditPhase_Begin,
    PrUiEditPhase_Change,
    PrUiEditPhase_Commit,
};
typedef uint8_t PrUiEditPhase;

// Called during the build with each phase of each gesture on `control`, the node that the control's creation returned.
typedef void (*PrUiEditFn)(void* PR_NULLABLE context, PrLayoutNode control, PrUiEditPhase phase);

// Sets the function that receives the gestures of the controls created under `node` in the current round. It replaces
// any function set on a node above `node`. A null function discards the gestures. A control whose parts edit its own
// copy of a value sets a null function over its parts, and reports its own gestures.
void prUiSetEditSink(PrUi* ui, PrLayoutNode node, PrUiEditFn PR_NULLABLE fn, void* PR_NULLABLE context);

// Reports a phase of a gesture on `control` to the function set on the nearest of `control` and the nodes above it that
// has one. Nothing is reported if that function is null, or if no node set one.
void prUiEdit(PrUi* ui, PrLayoutNode control, PrUiEditPhase phase);

// ---------------------------------------------------------------------------------------------------------------------
// Records
//
// A component library makes its nodes' records from group recipes of its own. A recipe is a small struct of plain data
// that writes one kind of group (pr/layout_style.h). The context keeps the groups that recipes made, and finds each by
// its recipe. It also keeps a memo of the records that the last frame's lookups returned, indexed by where in the round
// each lookup was made, so a widget unchanged since the last frame costs one comparison of bytes. Every library on the
// context shares the groups and the memo, and the context ages both by its rounds.
//
// A part is the recipes of one record. A recipe and a part are each identified by three things: their type, an address
// that the library owns for each type; their owner, the object whose values the recipes read, such as a theme, or null;
// and their bytes, which have no padding. Every part of one type has the same number of bytes.

// Writes the group of a recipe to `value`, which is the value of the recipe's kind (PrLayoutBoxStyle for a box group,
// and so on) and starts as layout's default. `owner` and `bytes` are the recipe's.
typedef void (*PrUiGroupMakeFn)(const void* PR_NULLABLE owner, const void* PR_NULLABLE bytes, void* value);

// A group recipe.
typedef struct PrUiGroupRecipe {
    const void* type;
    const void* PR_NULLABLE bytes;
    uint32_t size;
    PrLayoutGroup kind;

    // A group of `kind` that the recipe's group derives from, or 0. The context keeps it while it keeps the recipe, so
    // that the index in the bytes names one group for as long as the recipe can match.
    PrLayoutGroupIndex base;
    PrUiGroupMakeFn make;
    const void* PR_NULLABLE owner;
} PrUiGroupRecipe;

// Returns the group of `recipe`. On a miss, `make` writes a new group and the tree creates it. The context keeps a
// group until no lookup has used it for 64 rounds and no record names it. So the groups of a passing value, such as a
// fade's or a drag's, are released, and those of settled values stay.
PrLayoutGroupIndex prUiGroup(PrUi* ui, const PrUiGroupRecipe* recipe);

// A part: the bytes of the group recipes of one record, with its type and owner.
typedef struct PrUiPart {
    const void* type;
    const void* PR_NULLABLE owner;
    const void* PR_NULLABLE bytes;
    uint32_t size;
} PrUiPart;

// Looks up a part of `type` and `owner` in the memo, at the position of the round's next lookup. The position is the
// slot of the round's next creation (prLayoutTreeFrameCursor), with the number of lookups already made at that slot. A
// round that declares the last frame's tree therefore meets the last frame's positions in order, whichever library made
// each lookup.
//
// If the last frame's lookup at this position was of a part of the same type and owner, writes its record to `out` and
// returns that part's bytes, which stay valid until the next lookup. The caller compares them with its part's, which it
// knows the size of. Otherwise, returns null. If the bytes differ, or the result is null, the caller makes the record
// and passes it to prUiMemoKeep before its next lookup. Valid only during a build.
const void* PR_NULLABLE prUiMemoFind(PrUi* ui, const void* type, const void* PR_NULLABLE owner, PrLayoutStyleIndex* out);

// Stores `style` as the record of `part` at the position of the last prUiMemoFind, which did not find it, and returns
// `style`. `base` is a record whose index the part's bytes contain, as when the part restyles another record, or 0. The
// memo holds a reference to `style` and to `base`, so each index names one record for as long as the entry can match.
PrLayoutStyleIndex prUiMemoKeep(PrUi* ui, const PrUiPart* part, PrLayoutStyleIndex style, PrLayoutStyleIndex base);

// Releases the groups and the memo's records that `owner`'s recipes and parts made. A library calls it when the values
// that its recipes read change, and before it destroys the owner. Call between rounds.
void prUiForget(PrUi* ui, const void* owner);

// ---------------------------------------------------------------------------------------------------------------------
// Scrolling

// Scrolls each scroll container around the node, starting with the nearest, by the smallest amount that brings the
// node's box into view, or brings `rect`, in viewport coordinates as painted, into view. Uses the geometry of the last
// layout.
void prUiReveal(PrUi* ui, PrLayoutNode node);
void prUiRevealRect(PrUi* ui, PrLayoutNode node, PrLayoutRect rect);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

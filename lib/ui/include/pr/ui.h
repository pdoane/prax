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

// Sets the window that shows the context's main viewport, or none, or returns it. The window is the context's way to
// the platform: what its widgets ask of the system goes through the window and its application, and the context's
// output is the window's. Without a window, the context and its widgets work within the tree alone, and what needs the
// system does nothing. Setting another window drops what the context waits for from the previous one.
void prUiSetWindow(PrUi* ui, PrAppWindow* PR_NULLABLE window);
PrAppWindow* PR_NULLABLE prUiWindow(const PrUi* ui);

// A viewport that shows a root of the tree: the main viewport, which shows the round's first root, or the viewport of a
// portal that a host shows (prUiSetHost), which is the portal's key.
typedef uint64_t PrUiViewport;

// The main viewport.
enum { kPrUiViewportMain = 0 };

// Copies an event and its text into the queue for the next frame. The event's position, if it has one, is in px of
// `viewport`. Must not be called during a build. A Paste event goes to the focused control.
void prUiPushEvent(PrUi* ui, PrUiViewport viewport, const PrAppEvent* event);

// Reads the clipboard's text and delivers it to `control` as a Paste event in a later frame, whose request the read
// makes. If a read is pending, its text goes to `control` instead of starting another read. A read that fails pastes
// nothing. A clipboard without text, such as one holding an image, pastes the empty string: a control that inserts text
// ignores it, and a terminal still tells its program that the user pasted, so the program can read the clipboard
// itself. Destroying the context cancels the read.
void prUiPaste(PrUi* ui, PrLayoutNode control);

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
    PrVec2 extent;                  // main viewport size in px, used for each round's layout
    uint32_t maxRounds;             // upper limit on rounds in the frame; 0 means 1

    // The greatest extent of a portal's own viewport in px, such as the work area of the display, which portals are
    // laid out in. Zero means `extent`.
    PrVec2 portalExtent;

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

// Returns the round's overlay, a layer that covers the main viewport's root and paints above everything in flow. The
// pointer passes through the overlay to what is beneath it, except where a node in the overlay takes it. A library
// creates what stays in the main viewport under the overlay, such as the preview of a drop. What floats over the
// application is a portal instead.
//
// The first call in a round creates the overlay under the first root that the round created, which must already exist.
PrLayoutNode prUiOverlay(PrUi* ui);

// ---------------------------------------------------------------------------------------------------------------------
// Portals
//
// A portal floats over the context's window, such as a menu, a tooltip or a dialog. It floats in the overlay, unless
// the context has a host, which shows each portal in a viewport of its own: a window that the context's window owns, on
// its display and at its pixel ratio. A library creates a portal the same way in both cases, and the context lays it
// out and places it. The context's frame lays out every portal, and its output speaks for the windows of all of them
// (prUiOutput).

// How a portal is placed against its parent viewport.
enum {
    PrUiPortalKind_Popup,          // against an anchor, as a menu, a popover or a tooltip is
    PrUiPortalKind_Dialog,         // centered on the parent
};
typedef uint8_t PrUiPortalKind;

// A portal. A Popup opens against `placement`'s anchor, in px of `parent`. A Dialog in the overlay is centered on its
// parent, or `top` px below the parent's top if `top` is nonzero, and a host may place it otherwise, as a sheet. A
// modal Dialog takes the input of its parent while it shows. In the overlay, the context covers the parent beneath the
// dialog with an element of record `cover`, which takes the pointer, or a transparent one for 0.
typedef struct PrUiPortalDesc {
    PrUiPortalKind kind;
    PrUiViewport parent;
    PrAppPopupPlacement placement;
    float top;
    bool modal;
    PrLayoutStyleIndex cover;
} PrUiPortalDesc;

// Creates the round's portal of `key`, which must be unique in the tree, with input `flags`, and returns the node that
// holds its content. The node is as large as its content at the last layout, and the context places it. A portal whose
// flags are transparent lets the pointer through its viewport too. Call during a build. A portal lasts while every
// round creates it.
PrLayoutNode prUiPortal(PrUi* ui, PrUiKey key, const PrUiPortalDesc* desc, PrUiFlags flags);

// Returns the viewport that shows `node` in the committed tree, or the main viewport for a node that it does not show.
// Geometry queries on the node answer in px of that viewport.
PrUiViewport prUiNodeViewport(const PrUi* ui, PrLayoutNode node);

// Returns the window that shows the viewport of a portal, or null if none does yet.
typedef PrAppWindow* PR_NULLABLE (*PrUiViewportWindowFn)(void* PR_NULLABLE context, PrUiViewport viewport);

// What shows the portals of a context in viewports of their own: windows that the context's window (prUiSetWindow)
// owns, on that window's display. The context converts points between the windows (prAppWindowPointToDisplay), so that
// a pointer that one viewport captures can reach another, and names the window that holds the text being edited in its
// output.
typedef struct PrUiHost {
    PrUiViewportWindowFn window;
    void* PR_NULLABLE context;
} PrUiHost;

// Sets the host that shows the context's portals, or none to show them in the overlay. Call between frames.
void prUiSetHost(PrUi* ui, const PrUiHost* PR_NULLABLE host);

// A portal that a host shows, as of the last frame: its viewport, the viewport it floats from, and its place against
// that viewport (PrUiPortalDesc). `root` is the root that the viewport shows (prLayoutNodeDraw), and `extent` the
// portal's extent in px. `passthrough` is true if the pointer passes through it.
typedef struct PrUiViewportInfo {
    PrUiViewport viewport;
    PrUiViewport parent;
    PrUiPortalKind kind;
    bool modal;
    bool passthrough;
    PrAppPopupPlacement placement;
    float top;
    PrLayoutNode root;
    PrVec2 extent;
} PrUiViewportInfo;

// Writes up to `capacity` of the portals that the host shows to `out`, each after its parent and below the portals
// after it, and returns their number.
uint32_t prUiViewports(const PrUi* ui, PrUiViewportInfo* PR_NULLABLE out, uint32_t capacity);

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

// Writes the most recently routed pointer position to `out`, in px of the viewport that it is over, or of the viewport
// that reported it if it is over none, and that viewport to `viewport`. Returns false if no pointer event has arrived
// yet.
bool prUiPointer(const PrUi* ui, PrUiViewport* PR_NULLABLE viewport, PrVec2* out);

// Writes the extent of `node` at the last layout to `out`, in px, and returns true, or returns false if the node has no
// extent yet, as before its first layout. A widget that fits what it shows to its box reads the box's extent this way.
// If the round's layout gives the node another extent than the round read, the frame runs another round, as far as its
// limit allows, so that the frame shows the node at its new extent. Call during a build.
bool prUiExtent(PrUi* ui, PrLayoutNode node, PrVec2* out);

// Maps `point`, in px of the node's viewport, into the node's own coordinates as of the last layout: px from the top
// left of its first box, through every transform between them (prLayoutNodeLocalPoint). A widget measures a position
// within a box this way, such as a slider's value under the pointer, so that it holds under a rotation, a scale or a
// projection. Returns false if the node has no box, or if the point does not reach the box's plane.
bool prUiLocalPoint(const PrUi* ui, PrLayoutNode node, PrVec2 point, PrVec2* out);

// Returns true if the pointer is over the node or one of its descendants in the current round.
bool prUiIsHovered(const PrUi* ui, PrLayoutNode node);

// Returns how many seconds the pointer has stayed over the node or its descendants, or zero if it is not over them.
double prUiHoverSeconds(const PrUi* ui, PrLayoutNode node);

// Requests a cursor for while the pointer is over the node or its descendants. If several nodes request a cursor, the
// one closest to the node under the pointer wins. While a node holds the primary button's press, the cursor is the one
// closest to that node instead, wherever the pointer is.
void prUiSetCursor(PrUi* ui, PrLayoutNode node, PrAppCursor cursor);

// Sets where the caret of the text being edited stands in this frame, in px of the viewport that shows the focused
// node, and the kind of text, which chooses the on-screen keyboard and how the system treats the typed text
// (PrAppOutput::textInputKind).
void prUiSetTextInput(PrUi* ui, PrRect caret, PrAppTextInputKind kind);

// Marks `node` as a part of the window's custom title bar in the current round (PrAppWindowDecoration_Custom). Its
// bounds in the latest layout become a title bar region of the output. The controls within a Drag node, the nodes that
// are pressable or focusable, become Client regions after it, so that they take the user's presses. Call during a
// build.
void prUiTitleBarRegion(PrUi* ui, PrLayoutNode node, PrAppTitleBarRegionKind kind);

// Returns what the context reports to the window, which a frame handler returns or merges with its other views: the
// cursor resolved in the last round, or the arrow if no node requested one, whether it wants another frame because a
// tween is moving, a press is repeating, an activation is pending, input is still queued, or a frame was requested, the
// caret of the text being edited, the title bar regions at the end of the last frame, and the answer to a drag of the
// system. The output is the context's window's (prUiSetWindow). A caret in a portal's viewport names its window
// (PrAppOutput::textInputWindow). The window of a portal shows the same cursor and answers drags the same way. The
// regions are valid until the next frame. Call between frames.
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
// `origin` and `position` are in px of the viewport of the press, wherever the pointer goes. `delta` is the motion in
// the current round. `moved` becomes true once the pointer has gone past the drag threshold, and stays true for the
// rest of the drag. `released` and `cancelled` are set only in the round that ends the drag.
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
//
// A payload is what a drag and drop carries. A payload that the context's own nodes drag stays within the context,
// unless its drag lets it leave the window. A drag of the system brings a payload from another window of the
// application, or content from the system, such as files or text from another application.

// The kind of payload that carries content from the system (PrUiPayload::forms). Every other kind is the application's
// to choose.
PR_CONSTANT uint64_t kPrUiPayloadKindSystem = UINT64_MAX;

// The data a drag-and-drop operation carries. `kind` and `value` are chosen by the application. `source` is the node
// the drag started from, or none if the drag came from outside the context.
//
// A drag of the system carries content in `forms`, and its other members are zero for every other kind. A dropped
// payload of the system names its content as `drop`, which the program reads as a source (prAppDataReadText).
typedef struct PrUiPayload {
    uint64_t kind;
    uint64_t value;
    PrLayoutNode source;
    PrAppDataFormFlags forms;
    PrAppDataSource drop;
} PrUiPayload;

// How a payload's drag behaves.
enum {
    // The payload may leave the window for another window of the application. Once the pointer leaves the window, the
    // context hands the payload to the system through its window (prUiSetWindow, prAppWindowDragStart), and the window
    // under the pointer receives it as a drag of the system. Without a window, the payload stays in the window. The
    // window where the user drops the payload receives it, and the source's context sees no drop of its own.
    PrUiPayloadFlag_AnyWindow = 1 << 0,
};
typedef uint8_t PrUiPayloadFlags;

// Starts a drag-and-drop operation from a primary press held by `source`. The payload is picked up once the pointer
// moves `threshold` px, or immediately if `threshold` is 0. Escape cancels the drag, and losing pointer capture ends it
// without a drop. The release that ends the drag does not produce a click. While the system carries a payload that left
// the window (PrUiPayloadFlag_AnyWindow), the call does nothing.
void prUiDragStart(PrUi* ui, PrLayoutNode source, float threshold, uint64_t kind, uint64_t value, PrUiPayloadFlags flags);

// Starts a drag of content from a primary press held by `source`, which other applications can take, such as text or a
// file that the drag promises. The build calls it in each round of the press. Once the pointer moves `threshold` px,
// the context hands `desc` to the system through its window (prAppWindowDragStart), which copies it, and the system
// carries the content from then on. A press starts one drag. Without a window, the call does nothing. The drag's end
// arrives as the export of a payload of the system's kind does (prUiExportEnded).
void prUiDragStartContent(PrUi* ui, PrLayoutNode source, float threshold, const PrAppDragDesc* desc);

// Returns the payload being dragged, or null if there is none. The round that drops the payload, on a target or on
// none, still returns it, so that what a build shows while a payload is dragged, such as a drop target, is there to
// take the drop. The pointer is valid until the round ends.
const PrUiPayload* PR_NULLABLE prUiDragging(const PrUi* ui);

// Makes the node a drop target for payloads of `kind` in the current round. Returns true if it was the innermost drop
// target for the dragged payload's kind under the pointer in the previous round. A payload is never offered to its own
// source. While a drag of the system is over a target for its payload, the context answers the drag. It answers a
// payload with a move and the system's content with a copy. If the source does not allow that operation, the context
// answers with the first operation that the source allows.
bool prUiIsDropTarget(PrUi* ui, PrLayoutNode node, uint64_t kind);

// Returns the payload dropped on the node in the current round, or null. A drop goes to the innermost drop target for
// its kind under the pointer at release.
const PrUiPayload* PR_NULLABLE prUiDropped(const PrUi* ui, PrLayoutNode node);

// Returns the payload that left the window (PrUiPayloadFlag_AnyWindow), or the content that a drag handed to the system
// (prUiDragStartContent) as a payload of kPrUiPayloadKindSystem, if its drag ended in the current round, or null.
// Writes what became of it to `operation`: Move if a window of the application took it, this one included, and None if
// none did, as when the user dropped it outside every window or canceled the drag. A program that moves its content out
// of the window on a drop elsewhere, as a tab torn off into a window of its own, does so on None.
const PrUiPayload* PR_NULLABLE prUiExportEnded(const PrUi* ui, PrAppDragOperation* operation);

// ---------------------------------------------------------------------------------------------------------------------
// Keys, text and focus

// Claims `key` with exactly `modifiers` for the node, and returns true if such a key press was routed to the node in
// the current round.
//
// Claims made in one round decide routing in the next. A key press goes to the nearest node, starting at the focused
// node and walking up through its ancestors, that claimed the key. If nothing is focused, the walk starts at the key
// root (prUiSetKeyRoot). If no node claimed the key, it is used for focus traversal and activation. A key press that
// the input method takes for a composition (PrAppKeyboardEvent::composing) is neither claimed nor captured. It goes to
// the focused node.
bool prUiKeyPressed(PrUi* ui, PrLayoutNode node, PrAppKey key, PrAppModifierFlags modifiers);

// Captures `key` with exactly `modifiers` for the node, and returns true if such a key press was routed to the node in
// the current round. A capture routes ahead of every claim: a key press goes to the outermost node, among the focused
// node and its ancestors, that captured the key, before any node claims it. An application captures the few keys that
// it reserves for itself, which then work even inside a text field that would claim them.
bool prUiKeyCaptured(PrUi* ui, PrLayoutNode node, PrAppKey key, PrAppModifierFlags modifiers);

// The input events routed to a node in the current round, in order. Pointer events go to the node that captured the
// pointer, or otherwise to the node under the pointer, with their positions in px of the viewport of the press or of
// the node. Key and text events go to the focused node. The array is valid until the build returns.
typedef struct PrUiEvents {
    const PrAppEvent* PR_NULLABLE events;
    uint32_t count;
} PrUiEvents;
PrUiEvents prUiEvents(const PrUi* ui, PrLayoutNode node);

// Returns the focused node, or kPrLayoutNodeNone.
PrLayoutNode prUiFocus(const PrUi* ui);

// Sets the node of the current round that a key press starts at while nothing is focused, or the root for
// kPrLayoutNodeNone, which every round starts with. A host that wraps the application's root in nodes of its own, such
// as a title bar's, sets it to the application's root, so that the application's claims there take the keys. Call
// during a build.
void prUiSetKeyRoot(PrUi* ui, PrLayoutNode node);

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

# Immediate-mode UI

`lib/ui` provides the interaction that an immediate-mode UI needs beside layout: input routing, focus, capture,
scrolling and retained state, and what the component libraries over it share. It works over a layout tree that the
application declares whole every round (layout.md, Frames). Component libraries are written over `lib/ui` and
`lib/layout`, and Studio Kit (`lib/studio/kit`) is one of them.

## One tree

The layout tree is the scene. Every node that a frame shows or hit tests is a node of the tree, so painting,
stacking, clipping, transforms, scrolling and hit testing apply to all of it at once. Content that layout does not lay
out goes through one of two escapes. The application can place boxes itself, positioned absolutely and under a
transform for a canvas's pan and zoom. Or a painter can draw the content of a replaced element, and the parts of that
content take input as regions of the element's box.

## Hosts

`lib/ui` owns no window, renderer, fonts or clock. The application composes them: it creates the tree with its fonts
and shaper, creates a context over the tree, runs frames, and paints the tree's scene with the renderer it chose. This
composition is what lets a host replace any piece, so there is deliberately no single call that runs a UI in a window.
Each Kit example's `main()` composes its own. A root that draws no background lets a UI stand over what the host drew,
such as a 3D engine's frame, as the perspective example's root, a surface on `PrKitGround_None`, does.

Input arrives as `lib/app` events (`PrAppEvent`). A host that has its own window system translates its events into
them. Documents take the same events, so one host feeds native UI and documents alike.

## Rounds

- A frame runs rounds, up to its budget. A round routes queued input against the committed tree, then calls the build,
  which creates the tree again, states each node's interest in input, and asks what input reached each node. A layout
  follows each round, and the host paints the result of the last round.
- A round takes input up to an event that would make one of its queries ambiguous: a second press or release of a
  button, or a second key press. A round that routed a press, a release or a key press is followed by a round that
  routes nothing, so the tree that a frame ends with is declared from the state that its input left. A context's first
  round routes nothing.
- A node that the frame matches to a committed node keeps its handle, so the handle that the build holds is the same
  handle that input was routed to, and no identity is looked up.

## Interest and routing

- A node's interest is a set of flags that it states in each round that creates it: pressable, focusable, scrollable
  by the user, a wheel node, a focus scope, transparent to the pointer, its subtree transparent, or disabled. The data
  that `lib/ui` holds for a node is stored in columns indexed by the node's slot, and each entry is stamped with the
  handle and the round that wrote it.
- The pointer hits what paints on top, according to the tree's hit test. A press goes to the innermost pressable node
  among the ancestors of the topmost hit. Hover, capture and the cursor walk the tree's parents.
- Keys are routed by claim. A node claims a key and its modifiers by polling for it. A key press goes to the first node
  that claimed it in the last round, searching the focused node and its ancestors, or the root if nothing is focused.
  A node may instead capture a key, which takes it ahead of every claim. The outermost capture on that path wins, so
  an application's reserved chords work inside a text field. If no node claimed or captured it, the key press goes to
  traversal (Tab, within the innermost focus scope) and activation (Enter and Space click a focused pressable node).
- The wheel scrolls the innermost scrollable container under the pointer, each axis separately, and any scroll that
  the container cannot take passes outward in the same round. Scroll offsets belong to the layout tree, so a scroll
  records nothing and lays nothing out.

## Pointer geometry through transforms

- Routing reads the tree's hit test, which maps the pointer through each transform's inverse into a box's plane, so a
  node under a rotation, a scale or a projection takes input where it is drawn (layout.md, Trees).
- A position within a node is measured in the node's own coordinates, never against its bounds in the viewport:
  `prUiLocalPoint` maps a viewport point through the same inverses. A slider's value, a scrub's motion, a split's drag
  and a drop zone are measured this way, against the node's size as laid out, so a control on a turned panel follows
  the pointer in the panel's plane. Bounds in the viewport serve only what is placed in the viewport, such as a menu
  anchored to a widget.
- A component library's controls work under any transform, whoever applies it. Studio Kit's one 3D affordance is a
  component, the plane (`prKitPlane`): a box turned by a transform, which may also be the eye for the planes directly
  under it. Kit's boxes and placements carry no 3D fields.

## State

Retained state and tweens are kept on a node, identified by an owner and a key. The owner is an address that its
library owns, so the keys of different libraries never collide. They are kept as long as every round asks for them,
and the match carries them from round to round at no cost.

## What libraries share

Several component libraries can build one tree, each with its own widgets and appearance. What one library makes and
another must meet belongs to the context, so that no library owns it:

- **Keys.** A node needs a key only if it moves or comes and goes among its siblings. A library makes a `PrUiKey`
  under a base node (`prUiKey`, `prUiKeyIndex`), so an item keys under its container whichever library made each. A
  key that a frame gives twice is a caller's error. Layout reports both nodes (`prLayoutTreeDuplicateKeys`), and each
  round of the next frame outlines them in the overlay.
- **The overlay.** What floats over the application, such as a menu, a tooltip or a drag's ghost, is created under
  the round's overlay (`prUiOverlay`), a layer over the root above everything in flow. The context creates it under
  the root in the first round that asks for it.
- **Edit gestures.** A bound control reports the phases of each edit (`prUiEdit`) to the sink of the nearest node
  above it that set one (`prUiSetEditSink`), so the application records one undo entry for a gesture on any library's
  control.
- **Records.** The context keeps the groups that libraries' recipes made and a memo of the last frame's records by
  their place in the round (component_library.md, Records). It ages both by its rounds, so a library needs no call to
  begin a round.
- **Text editing.** A library's text controls share one edit (`pr/ui_text_edit.h`). In the simple form, the caller gives
  the text as a value in each round and takes the new text if it changed, and a text buffer holds the text only while
  the control is being edited. A caller whose text needs an edit stream, anchors or history across focus gives its own
  buffer instead (text.md, Text editing). The edit moves the caret and the selection by the pointer and the keys over
  what the last layout placed, and it applies typing, deletion and the clipboard of the application that the host binds
  (`prUiSetApp`). The edit creates the text node that shows the text, and the layout tree paints the selection and the
  caret for the frame (layout.md, Text positions, highlights and the caret). A library chooses only the look: the
  element that holds the text, and the colors of the selection and the caret.

## Component libraries

A widget is a function of its library that takes a parent, its content and its style, and returns a node that the
caller places, restyles or puts children under. [component_library.md](component_library.md) describes how a library
is written over `lib/ui` and `lib/layout`, with Studio Kit as its example.

A component library is opinionated, and its vocabulary is its own. `lib/layout` uses the web's names because it is the
substrate. A library built on it chooses the concepts it exposes, as each web component library does, and is not
expected to match another library or CSS. An application reaches past its library to `lib/layout` and `lib/ui`
directly for content the library does not provide, such as a 3D viewport.

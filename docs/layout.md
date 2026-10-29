# Layout

`lib/layout` lays out trees of styled boxes and paints them into `lib/draw` lists. It knows nothing of the DOM,
selectors or the cascade. A client builds a tree of nodes, each styled by a record, and asks for geometry and paint.
`lib/dom` is one client, and an immediate-mode UI is another.

## Style records (`pr/layout_style.h`)

- A record holds one group index of each kind (`PrLayoutGroup`). A group is a small plain struct of related values,
  modeled on CSS computed values: absolute where CSS computes to absolute values, with percentages kept.
- Groups and records are interned by the tree whose nodes they style, in tables of blocks whose entries never move. A
  tree has its own tables, so trees share nothing, and each tree lays out for the display that it is on. Index 0 of
  every table is its kind's default. Equal values intern to one index, so records compare as numbers.
- A producer that finds its groups itself, by keys of its own, creates them instead (`prLayoutBoxStyleCreate` and
  the others, `prLayoutStyleCreate`). A created entry hashes nothing, and interning never finds it. The producer
  guarantees one entry for each value it means, and two equal entries cost extra layout, never wrong output, because
  layout compares indices.
- A group's kind is either inherited or not, and is read either by layout or only by painting. A changed index of a
  paint-only kind needs no layout. If layout reads one fact about a value that painting reads whole, the layout group
  holds the fact and the paint group holds the value: whether a box is transformed, and the matrix; whether its
  z-index is auto, and the integer. A change from one transform or integer to another lays nothing out.
- A box's transform (`PrLayoutTransform`) is a 4x4 matrix that is affine in the size of the box's reference box
  (`transform-box`): the matrix at zero size, plus a vector per unit of width and one per unit of height added to its
  last column. Its origin stays separate, as lengths of the reference box, because folding a 3D origin into a matrix
  with perspective is not linear in the size. Painting resolves both once the box is laid out, and applies
  `T(origin) * matrix * T(-origin)`.
- A box's perspective applies to the transforms of its children, not to deeper descendants, about its perspective
  origin in its reference box, and moves with its scroll offset (css-transforms-2/#perspective-property). A box with a
  perspective is a containing block for all of its descendants, as a transformed box is, so each child is placed in
  it. The map from a child's plane to its parent's is `T(p) * perspective(d) * T(-p) * T(offset) * T(origin) * matrix
  * T(-origin)`, in doubles (`layoutPlaneMap`). A box whose `backfaceHidden` is set, and whose map shows the back of
  its plane (`prDMatrix4ShowsBack`), is neither painted nor hit; the walk and the scene apply the same test.
- **The compositing group** holds what composites a box with what lies beneath it, which painting and hit testing
  read: the clip path, the clip rectangle, the mask layers and mask border, the filter and backdrop filter lists, the
  blend mode and isolation (css-masking-1, filter-effects-1, filter-effects-2, compositing-1). Its basic shape, its
  mask and its filter lists are interned as one entry each, as background layers are. A clip path, mask layer or
  filter that a URL names is a source (`PrLayoutSource`). A source is a set of functions that the producer gives,
  which draw the resource for a reference box, append its filter nodes, or say whether it holds a point. The group
  holds a reference to the source's context. The source's version changes when what it draws changes, so the group
  changes with it. lib/layout knows nothing of what the source is. The DOM's presentation gives its SVG resources this
  way.
- **Corners** are the decoration group's: the radii and the shape of each corner of the border box
  (css-backgrounds-3/#border-radius, css-borders-4/#corner-shape). Everything that a box's corners shape reads them
  there: its background, border and outline, the clip of its content as a container, a replaced box's content, where
  it is hit, and a clip path or backdrop filter of the border box's edges. The other boxes' corners follow from the
  border box's, inset by the widths between them (`prCornerRectInset`), and an outline's grow as a shadow's spread
  does (`prCornerRectOutset`). A box whose corners are all square, as most are, draws, clips and hits rectangles.
- Interned and created entries are counted by the records and nodes that name them, and by references that a caller
  holds (`prLayoutGroupReferences` reports a group's count). An
  index returned by interning is valid until the next layout, which first frees the entries that nothing names.
  Interning grows with the number of distinct values, not with how often they recur.
- The cost of freeing depends on what changed, not on what the tables hold. Each table lists the entries made, or
  released to no references, since the last layout, and a layout frees from that list. A layout after no change frees
  nothing and reads no table.

A producer fills a group, interns it with its `Intern` function or creates it, and interns or creates a record of the
group indices. The DOM's projection interns its groups. A component library finds its groups by its own recipes,
creates them, and interns its records (component_library.md, Records).

### Inheritance

Inheritance is designed for the immediate-mode producer, not for the DOM, whose projection is retained, is under our
control, and can supply records in any form. A component library declares its records over `lib/ui`, which routes
input over the layout tree ([ui.md](ui.md)), so inheritance is a question for `lib/layout` and the producers that build
its trees.

- **The producer rebuilds the whole tree after any change or event**, so its cost is bounded by building and comparing
  records, which are plain data, hashed and compared as bits.
- **Styles are set late.** A reusable component builds a node, and its caller restyles the node afterward (for
  example, giving a kit's button a border), so a node's style, and its parent's, may change at any point before
  layout. Inheritance cannot be fixed when a node is created.
- **Inheritance is by group.** Any group index of a record may be a value that means "the parent's group", which an
  inherited kind takes by default. A late set replaces a whole group, never one property of it. Layout resolves the
  values once, in a pass before it lays out, into one record per node, which everything that reads a node's style
  reads. A node counts as changed if its resolved record changes, whether its own style or an ancestor's style caused
  the change.
- **Group boundaries follow what is set together.** A producer that sets a group states all of it, so properties that
  callers set independently belong in different groups: the faces of a font apart from how text sits on lines, the
  sizes of a box apart from its edges, and what a box paints apart from what lays it out. A record holds at most 16
  groups (64 bytes), and the aim is about 8.
- **Why not by property.** A mask of the fields that a node sets would let a node override one property and inherit
  the rest of its group, but it tracks properties per node and merges groups to resolve them. It is the next step only
  if real components are seen restating properties that they do not own, and boundaries that fit better come first.

Two earlier prototypes bracket the design. The first inherited at construction, in tree order, so a style set later
did not reach the nodes built before it. Tree-order construction was workable but often awkward. The second had two
groups, one read by layout and one by painting, and no inheritance. It was fast, but every component set every
property. Group-level inheritance aims at the second's speed without its restating. Once `lib/ui` runs real
workloads, its cost is measured, and the second prototype's model is the fallback.

## Frames (`pr/layout_frame.h`)

An immediate-mode producer declares its whole tree each frame. A frame creates the tree from scratch, and the next
layout commits it.

- **A creation matches as it is made, against the committed tree in preorder.** A creation that is the next committed
  node in preorder, with its key (none, for a child placed by position), kind and parent, continues that node: one
  comparison and one append, and the node keeps its handle, caches and scroll offset. Any other creation looks further:
  a keyed node in the tree's index of keys, under any parent, and an unkeyed node at its parent's next unkeyed committed
  child of its kind. The frame goes on in preorder from the node found. A creation that leaves its parent's children
  out of their committed order, a new node, or a node moved from another parent, marks the parent for the commit to
  relink. So creations may come in any order across parents, and a frame pays for a change where it is: a frame that
  follows the preorder throughout pays only for its comparisons.
- **Every node that a frame creates with no parent is a root of the tree it commits**, in the order created, and the
  committed preorder runs through each root in turn. An unkeyed root continues the first committed root of its kind
  that the frame has not created. A committed root that the frame does not create again is erased with its subtree.
  The frame's caller chooses which roots a layout lays out (`prLayoutTreeFrameRoots`).
- **The committed tree is untouched until the commit.** A record set in a frame is pending, and is compared with the
  committed record at the commit, so a caller that restyles a component's node after creating it pays only for a
  store. Links are also pending. Queries, hit tests and painting during a frame read the committed tree.
- **The commit** walks the frame's creations once. A frame that continued every committed node, each under its
  committed parent and in order, compares records and changes nothing else. Otherwise the commit relinks the marked
  parents through the tree's own edits and their marks, erases the committed nodes that no creation continued, sets
  the pending records, and takes the new preorder. A frame that creates the committed tree again marks nothing, and its
  layout costs the same as a layout after no change.
- **A frame is a mode of the tree.** Its creation calls take a key and match committed nodes, so they are separate
  from the retained-mode calls in `pr/layout_tree.h`, which create unkeyed nodes. C has no overloads, so the mode shows
  in the function names.
- A node's slot (`prLayoutNodeSlot`) is dense, and a node that a frame continues keeps its slot, so a client keeps its
  per-node data in arrays indexed by slot, beside the handle that each entry is for.

## Values (`pr/layout_value.h`, `pr/layout_unit.h`)

The immediate-mode UI declares every box's style every frame, and its speed comes from hashing and comparing groups.
So a group's size in bytes, including padding, is its cost, and declaring a style copies bits instead of converting
values.

- **Geometry is fixed point.** `PrLayoutUnit` is a saturating `int32_t` in units of 1/64 px (26.6), with a range of
  about ±33.5M px, which is the limit that web content already lives within, and with the rounding that WPT expects.
  Sums are exact, so offsets do not drift down a long document, and equality is exact, which caches and the
  from-scratch oracles rely on. Fragments, offsets, scroll offsets and geometry use it. Text keeps float advances within
  a line, and snaps to 1/64 px where a fragment is placed. Float coordinates stay small: a recording holds each block
  subtree in its own space, placed by a translation in doubles, and the list's transforms compose in doubles down to
  the target.
- **One value encoding, in typed wrappers.** A value that is a union of kinds is one 32-bit word, wrapped in a type for
  each category (`PrLayoutLength`, `PrLayoutNumber`, `PrLayoutLineHeight`, ...). Each type is a
  `struct { uint32_t bits; }` that documents the kinds that it accepts, and interning checks them in debug builds. One
  encoder, one resolver, one table of compound values and one hash serve every type. A group that needs more bits for
  a value escapes to 64 bits.
- **A word holds one scalar or one keyword.** A compound value (`calc()` and `min()`/`max()`/`clamp()` with
  percentages, `fit-content(<lp>)`, `minmax()`, `repeat()`, `anchor()`, `anchor-size()`, `span <n>`, or a ratio) is
  interned in the context, immutable and counted like a group, and the word holds its index. A calculation holds its
  operands in doubles, as CSS computes them, and only its result is converted to units. Values are canonical, so equal
  values have equal bits: a `calc()` that simplifies to pixels is stored as pixels.
- **The word's kind is a prefix code in its low bits**, read with `ctz(~word)`, and its payload is sign-extended by an
  arithmetic shift. Pixels, the common case, cost one bit test, and two pixel values add without decoding.

  | low bits | kind | payload |
  | --- | --- | --- |
  | `…0` | pixels | 31-bit signed 26.6: ±16.7M px, saturating; a specified length needs less than layout's range |
  | `…01` | percentage | 30-bit signed 14.16: ±8192% in steps of 1/65536 %, saturating |
  | `…011` | number | 29-bit float, the float's three lowest mantissa bits dropped: -0 stored as 0, no NaN |
  | `…0111` | compound | 28-bit index into the tree's table |
  | `…01111` | integer | 27-bit signed |
  | `…11111` | keyword | 27-bit id, from one keyword enum (`auto`, `none`, `normal`, `min-content`, `stretch`, ...) |

- **`z-index` is its own `int32_t`**, with `auto` as a bit beside it, because pages use values beyond any narrower
  range, and stacking order needs them exact.
- **Colors are RGBA half floats**, 8 bytes, encoded as extended sRGB with straight alpha, as draw lists carry them.
  That is scRGB with the sRGB transfer function, as the Khronos Data Format Specification defines it (Vulkan's
  `EXTENDED_SRGB_NONLINEAR`). Every 8-bit sRGB code survives the round trip exactly, so one type serves documents
  bound for either target format. -0 is stored as 0, and there is no NaN, so equal colors have equal bits. Linear
  blending belongs to `lib/draw`, at blend time.
- **A color's channels are `PrFloat16`**: `_Float16` under Clang, the main target, and `uint16_t` under MSVC, where a
  color is converted at run time through core's functions. A channel is set as `(_Float16)((x) + 0.0f)`, which stores
  -0 as 0.
- **Literals are built at compile time**, in C as in C++: pixels, percentages and numbers by integer arithmetic, and
  colors by `_Float16` casts, so a component that declares a style each frame copies bits. Values computed at run time
  convert in one instruction (`FCVT` on arm64, and F16C on x86-64, whose builds target x86-64-v3).

## Trees (`pr/layout_tree.h`, `pr/layout_query.h`)

- A tree is plain data: nodes indexed and linked by index, with an element styled by a record and a text node styled by
  its parent's record. A line break, as HTML renders a br, is a node of its own. Its record gives its clear and its
  display, and its line is set in its parent's font, as the text around it is. A client names its own objects to the
  tree by keys, each held by at most one node. The tree finds nodes by key, but never reads the keys. A node's name
  (`PrLayoutNode`) carries its slot's generation, so a name held past an erase, as a compositor may hold one, names
  nothing, even once the slot is reused.
- **Roots.** A root is a node with no parent. A layout is given its roots, each with the extent of its containing block,
  so the tree keeps no root of its own, and a detached node that waits to be inserted is not laid out. The first root is
  the viewport's: the initial containing block, whose root's background is the canvas's, whose overflow the viewport
  takes, and whose fixed boxes the viewport places. Every other root is the containing block of all the boxes under it,
  as an embedded document's root is. It is drawn by a replaced element's painter in that element's content
  (`prLayoutNodeDraw`), as an SVG tree draws a `foreignObject`'s content. A paint draws such a root flat, its layers
  in place with their own effects, before the boxes whose painters may draw it, and a painter that drew a root draws
  again when the root's drawing changes. Its viewport units are the viewport's, and its geometry is measured from the
  top left of its containing block.
- A change to a node marks the node, and marks its ancestors as holding a change. Paint-only record changes mark
  nothing. A change inside a box out of flow marks the ancestors between the box and its holder as holding only a change
  out of flow, which leaves their own results current: the holder lays the box out again and reuses those results. A
  change that may move a box out of flow to another holder (a position, a display, a transform appearing, or children
  inserted or removed) marks every ancestor.
- Painting, hit testing and geometry read the tree as of the last layout, which follows every change. Geometry is
  measured either as painted, through transforms and scrolling, or as laid out, before either applies
  (`PrLayoutCoordinates`). The placements publish the final tree (`LayoutPlacement`). Each placement gives a box's
  fragment and its offset in its parent's fragment (its holder's fragment if it is out of flow, and the nearest box
  around it if it is in flow), through the anonymous boxes, rows and lines between, and, for an inline box or a text,
  the line that it starts on in its block container. Placements are written once a layout's tree is complete, by a
  walk that descends only into the fragments that are new to it, so a fragment that a layout builds and then drops
  never reaches them. Readers of the final geometry read placements, never layout entries, which are a cache and may
  hold a different result. A query maps its node's boxes through their placements, transforms and scroll offsets, at a
  cost proportional to the node's depth.
- A query in viewport coordinates maps a box through every transform between it and the viewport. A box under a
  projection gives the bounding rectangle of the part of it that is drawn: the part nearer the eye than drawing's cut
  (`kPrPathPolylineNearPlane`) is cut away, so a box partly behind the eye has finite bounds, and a box wholly behind it
  gives an empty rectangle. Hit testing maps the point the other way, through each transform's inverse into the box's
  plane, so a box is hit where it is drawn. A point that a projection maps behind the eye reaches nothing in that
  plane, and a box that hides its back and shows it is not hit. Nor is a box that its color group makes invisible or
  gives no pointer events, though the boxes inside it can be hit. `prLayoutNodeLocalPoint` maps a viewport point into a
  box's own coordinates through the same inverses, one plane at a time, which is what a position within a box under a
  projection needs, such as the value under the pointer on a tilted slider, and `prLayoutNodeMapFromBox` maps the
  other way. A box whose pointer-events is none is not hit, though its descendants may be
  (css-ui-4/#pointer-events-control). A root other than the viewport's, which a replaced element's painter draws, is
  hit tested on its own (`prLayoutNodeHitTest`). The client that draws the root gives the point from the top left
  of the root's containing block.
- The viewport is a scroll container of the document. It takes the root's overflow, and the root itself does not clip
  by that overflow. Scroll offsets belong to the tree, by node, and layout reads none of them.
- Overflow is physical. A box, line or row records the rectangle that it and its descendants reach, measured from its
  own top left, which the rectangle may extend left of or above. The rectangle covers its own box, each child's box
  with what the child reaches in each axis that it does not clip, and a block container's in-flow content through the
  margins that the container holds, so that a margin past a box's end can be scrolled to in a scroll container that
  holds it. A rectangle needs no conversion if a child's writing mode differs from its parent's.
- A scroll container records its scrollable overflow instead: its padding box (not its border box), what its content
  reaches, and its in-flow children's border boxes, through anonymous boxes, rows and lines, with the margins that it
  holds and its own padding past them, cut at its origin's sides. Its origin is the corner of its padding box that is
  block-start and inline-start in its writing mode, and the viewport's origin is that of the principal writing mode. A
  box that does not scroll has a scrolling area all the same, which `scrollWidth` reads: its padding box and what its
  children reach, cut at its origin's sides, computed when it is asked for.
- A scroll offset is the scrollport's offset from its unscrolled position, which is the origin, so it is negative
  toward an origin at the right or the bottom. Painting, hit testing and a compositor translate content by it and read
  no writing mode. Its range is a rectangle of the least and greatest offsets.

### Text positions, highlights and the caret

A position in text is a caret before a byte of a text node's own text, the offset that DOM ranges and selections
hold. Layout answers where the last layout put a position, and which position is nearest a point
(`prLayoutNodeTextCaret`, `prLayoutNodeTextRangeBoxes`, `prLayoutTextPositionFromPoint`), and paints highlights of
ranges and the caret (`prLayoutNodeSetHighlights`, `prLayoutTreeSetCaret`).

- **Source offsets.** Collection keeps, for each byte of the collected text, the offset in its node's text of the
  character that it came from (`LayoutInlineShaping::sources`). White space collapsing and text-transform change the
  collected text, and the map carries each change back to the node's text: a kept space stands for its run, and the
  bytes that a transform made stand for the character that it replaced.
- **Caret stops.** A text fragment records a stop at the start of each cluster, in the order that it places its
  glyphs: the cluster's offset in its node's text and its line-left edge along the line, with justification in
  place. It records the end of its bytes, through the collapsible white space that ends its line, and whether it is
  reversed. A fragment holds one direction and one orientation, so a range of its bytes is one stretch along it, and a
  query of a position reads only the fragment and the writing mode of its record.
- **Breaks.** A preserved segment break makes a text fragment of its own at the end of its line, with no glyphs and no
  advance, so that a line that holds nothing else gives a caret a place. A break adds no box to the node's boxes, and
  is never hit.
- **Painting.** A text fragment with a highlight or the caret is recorded on its own: the highlights' backgrounds, then
  its glyphs, clipped to each stretch along the line that one color paints, then the caret. Every other fragment is
  recorded as before. Setting highlights or moving the caret marks the nodes whose recordings change, as a record
  change does.
- **Frames.** A frame sets highlights and the caret for itself (`prLayoutTreeFrameSetHighlights`,
  `prLayoutTreeFrameSetCaret`), and the commit removes those that the frame did not set again, as it erases the nodes
  that the frame did not create.

### Scroll anchoring

A scroll offset follows its content as layout moves it (css-scroll-anchoring-1), so that a reader keeps their place as
content above them changes, a stream rebuilds its tail, or a resize reflows the text. Anchoring belongs to the tree,
since everything it reads is geometry and a few facts of a box's record, so a document and a native tree get it alike.
`lib/dom` supplies `overflow-anchor` and fires the scroll events that an adjustment causes.

- **When.** A layout that has nothing to lay out does no anchoring. A layout that runs selects, before it lays out, an
  anchor for each scrolled container that lacks one, from the last layout's fragments, and after it lays out adjusts
  each container whose anchor moved. Every layout ends a suppression window, which is when a geometry read must see the
  adjusted offset.
- **What is anchored.** The viewport and each scroll container whose offset is away from its origin in its block axis.
  The tree lists the containers whose offsets are not zero, so a document that is not scrolled pays nothing. An anchor
  stays selected until its container's offset is set from outside the tree (a user, a compositor, script), an
  adjustment is suppressed, or the anchor stops being laid out.
- **Selection** follows the specification's candidate examination over the tree's nodes. The optimal viewing region is
  the scrollport. A node's rect is its scrollable overflow, cut at its own start edges. An inline box that is not
  atomic, and an element that generates no box, are descended into rather than selected. A scroll container is selected
  whole only if it is scrolled. A box that occupies no space is not selected, and the examination goes on past it. An
  anchor chosen among content that the last layout had not laid out is chosen again after the layout. An anchor is a
  whole node, so a paragraph that starts above the scrollport holds its start, and its lines can drift as it rewraps.
- **The adjustment** is the move of the anchor's block-start edge in its container's block axis, measured as laid out,
  before transforms and scrolling, added to the offset and clamped to the new range.
- **Suppression** follows the specification's triggers, which serve pages that move content as they scroll: a change of
  a box's sizes, margins, padding, insets, position or transform on the path from the anchor to its container, the
  anchor or its path becoming excluded, a box in the container becoming or ceasing to be absolutely positioned, and an
  offset of zero. A record that changes such a value marks its node, and the next layout reads the marks along each
  anchor's path. A layout in a new viewport extent suppresses nothing, since a page that adapts to its width changes
  those values around nearly every anchor as it resizes, and keeping the reader's place is what anchoring is for.
- **The compositor.** A compositor's own offset wins over the client's until the client takes it, so an adjustment is
  also a delta. The scene that the adjusting layout paints carries the deltas since the last scene, and a commit that
  replaces a scene adds them to the offsets that the compositor owns, so the new geometry and the offset arrive in the
  same frame.

## Layout

- Layout runs in logical terms (an inline axis along lines, and a block axis across them) and writes physical
  fragments. A box lays out its content in its own used writing mode, taken from its style's font group, in which
  `upright` text in a vertical mode becomes `ltr` (`layoutUsedMode`). The initial containing block takes the root's
  writing mode, which a document sets to the body's unless containment applies to the root or the body. A box's style
  is read through its logical view, and its children are placed at logical offsets that become physical when its
  fragment is made, once its size is known. Both mappings are in one place (`layout_logical.h`).
- The space that a box is laid out in is stated in the box's own axes: what an auto inline size fills, and the bases
  of its size percentages. What its parent places it by (the containing block's writing mode, and its two sizes, which
  margins, padding and insets resolve against) is stated separately. A box in its containing block's flow has bases
  equal to those sizes. An orthogonal flow root, a block container whose lines cross its containing block's lines, has
  them crossed. Its block-axis percentages resolve against the containing block's inline size, and its auto inline
  size fits its content in the containing block's block size. If that size is indefinite, the auto inline size fits in
  the lesser of the containing block's fixed max block size and a fallback, which is the inner size of the nearest
  scroll container, or of the initial containing block. `layoutBlock` makes that space, and places the box in its
  parent's inline axis once its block size is known. Measuring a containing block lays out an orthogonal flow root,
  and takes its block size as both of its contributions.
- The fallback travels in the space. A result that read the fallback says so, and its cache compares the fallback
  before reuse, as an intrinsic entry does. A scroll container's content reads its own sizes and the initial
  containing block's, which the container records as a read of the viewport's extent.
- Code that reads a display asks what the display means (inline-level, an inline box, a formatting context root, a
  scroll container, a layer) through the questions in `layout_box.h`, not by comparing values.
- A box's layout takes a space (`layout_space.h`) and gives a result. `layoutBlock` is the single entry point for a
  block-level box. It takes the cached result if the cache holds (`layout_cache.h`), and otherwise lays the box out,
  counts it, and caches the result. For a box in flow in a context, `layoutBlock` records the box's reads of the
  context, merges them into its parent's reads, and keeps them with its result.
- **The shell and its content.** Every block-level box is laid out in three steps (`layout_block.h`). Its shell finds
  its edges, its inline size, the block size that its style or its container decides, and its content box. The
  algorithm that the box's children name (`LayoutChildren`) lays out its content: flow (`layout_flow.h`), flex
  (`layout_flex.h`), grid (`layout_grid.h`) or table (`layout_table.h`), chosen in one switch. The shell then clamps a
  block size that the content decided, and makes the box's fragment, holds its boxes out of flow, and finds its
  overflow and the baselines that it exports. Intrinsic sizing and measurement dispatch on the same value.
- **Independent formatting contexts share one protocol** (`layout_content.h`): the content box and its origin, the
  block size that the style or the container decides or the limits of one that the content decides, and the goal, which
  is to build fragments or to solve the content alone for a measurement. The content gives back its block size, its
  baselines, and the bounds of its items' margin boxes. Flex and grid take exactly that. A table adapts it, since its
  wrapper box has no edges and its border box holds its captions. Flow takes its own input, because a block container
  in its parent's context finds its position through its content: margins collapse through its edges, and floats wait
  for them.
- A box establishes a block formatting context (it keeps its children's margins and its floats, and grows to contain
  its floats) if it is the root, it floats, it is a scroll container, its display is `flow-root` or `inline-block`, it
  is a flex or grid container, a table, a table cell or a caption, or its writing mode differs from its containing
  block's.
  Overflow `clip` clips without establishing one. An inline-level block container and a float shrink to fit: an auto
  inline size is `fit-content`, and auto margins are zero. The root is laid out by block rules whatever its display,
  and if it floats, it floats to its side of the initial containing block.
- An outside marker (`PrLayoutDisplay_Marker`), a list item's first child, is laid out apart from the item's flow as an
  inline-block is, and is a child of the item's fragment. The item gives the marker's margin box, about its baseline,
  to the first line of its flow through the space (`LayoutSpace::marker`), which passes it to each block-level child in
  flow until one has a first baseline, at any depth and into formatting context roots. The first line that is not
  phantom holds that extent as it holds its strut, and the marker sits on the item's first baseline. A result keys on
  the markers of its space, so a change to a marker that keeps its extent lays out the item and the marker alone. An
  item with no in-flow content lays out a line for its marker, and one whose content has no line puts the marker as if
  on a line at its content box's block-start. The marker's inline-end edge is at the item's border box, moved by the
  floats placed before the item as far as they move the start of the marker's line.
- A block-level box in flow is aligned in its containing block's inline axis by its `justify-self`, whose auto value
  is its parent's `justify-items` (css-align-3/#justify-block). At a value other than normal or stretch, an auto inline
  size is `fit-content`, and the box is placed in the space that its auto margins leave, with its over-constrained
  margin left as it is.
- A node's kind is a set of bits: whether it is a box that its record styles, and whether it is an element, whose
  children are laid out in it. A semi-replaced element, as HTML lays out a widget, is both, and differs from an
  element in one rule: in block flow, at a justify-self of normal, its auto inline size is `fit-content`. As an item
  and between insets, it is stretched as any element is.
- A fragment is immutable once made, and is shared by reference count. A box's result is its fragment, the margins
  that collapse through its block-start and block-end edges, and its first and last baselines, which are those of its
  first and last in-flow lines, through anonymous blocks and block children. A block-level scroll container passes up
  its block-end margin edge as its last baseline. A scroll container clamps its own baselines to its border box. A box
  whose writing mode is orthogonal to its space's writing mode has no baselines. Its fragment records where each
  baseline and its set of other baselines come from: the font of the line it was found on, a margin edge, or the
  margin box that it is synthesized from. If a clamp changed a baseline of the set, the fragment also records the range
  that each named baseline is clamped to, composed through nested scroll containers.
- A block-level box or inline-block keeps its last result with the space that it was laid out in. A layout reuses the
  result, with its whole subtree, if nothing in the subtree changed and the space is the same in everything that the
  result read. The result reads the containing block's inline size only if the box's style has a percentage that reads
  it. A block container in flow whose inline size did not fill the available space (a fixed width, a limit, or an auto
  width that its margins leave at zero) is reused at another available size if it resolves to the same inline size
  there, at the inline-start margin that it resolves to there, so a centered box of a capped width keeps its subtree
  through a resize. An inline formatting context is laid out whole with its container, and each inline-block and float
  in it is laid out through its own cache.
- A box's preferred aspect ratio is answered by one question (`layoutNodeAspect`): a replaced box's natural ratio if its
  `aspect-ratio` defers to it, and otherwise the property's ratio, relating the content boxes or the boxes that
  `box-sizing` names. An automatic size takes what the other axis's definite size gives through the ratio, with that
  size clamped by its limits first, and the box's own limits applied after. A block size that comes from a ratio is
  definite, so it is not auto for margin collapsing, and its automatic minimum lets the content grow the box unless the
  box scrolls. A box out of flow sizes its ratio-dependent axis through the ratio instead of stretching, and its
  content grows it as any box's content does. The box's intrinsic sizes are the transferred size if its block size is
  definite. A size that its flex container asks of its content has no ratio.

### Boxes out of flow

- A box whose position is absolute or fixed, other than the root, is out of flow (`layout_position.h`). It takes no
  part in its parent's layout except through a placeholder, a fragment with no extent that names the box at its static
  position. A placeholder in block flow takes no room and ends no margin chain. A placeholder in a run is an item with
  no text, no break opportunity around it, and no content of its line. Its static position is where the box would be
  as a static box, block-level or inline-level according to what its producer says its display was before
  blockification (`staticInline`), in the writing mode of its static context, whose start edges it gives.
- Three relationships are kept separate. The containing block box is the box whose edges the box is placed against: the
  nearest positioned, transformed or filtered ancestor for an absolute box, the nearest transformed or filtered ancestor
  for a fixed box (`layoutHoldsAbsolute`, `layoutHoldsFixed`), or else the initial containing block or the viewport. A
  box is filtered if its filter or backdrop filter is not none (filter-effects-1/#FilterProperty). The box group records
  whether a box is filtered (`filtered`), apart from the filter lists, as it records whether a box is transformed. A
  filtered root holds nothing, so its fixed boxes stay the viewport's. A transform does not apply to an inline box, but
  a filter does: a filtered inline box holds both kinds, as a positioned inline box holds absolute boxes. The holder is
  the box whose layout lays the box out and whose fragment holds the box's fragment. It is the containing block box,
  except that an inline box's block container is the holder, and the viewport is the holder for the initial containing
  block and the viewport. The placeholder is in the flow of the box's parent. A holder is always an ancestor of its
  boxes' placeholders.
- A fragment lists the boxes out of flow in its subtree that it does not hold, each with its static position measured
  from the fragment's top left, after its children in its allocation. A fragment with none stores nothing. Each
  fragment gathers its children's lists and its placeholders, and a holder takes the boxes that it holds, nearest
  holder first, before anything around it sees them. A holder lays out the boxes that it holds once its own size is
  known, from a work list that in turn takes the boxes listed by the boxes it lays out, and its fragment holds their
  fragments after its children in flow. A cached result brings its list with it, and no list names an inline box, so no
  cached result depends on an inline context outside it.
- A box out of flow is laid out by `layoutBlock` in a space that its containing block and insets make. Its
  inset-modified containing block's sizes fill an auto size if neither inset of the axis is auto, and otherwise its
  auto size is fit-content (`LayoutAutoSize`). Percentages resolve against the containing block's sizes, crossed if
  the box is an orthogonal flow root. Its cache keys on the sizes of the containing block, which together with its
  style decide that space, and on the size that it stretches to, which its static position decides if both insets of
  its block axis are auto. Its margin box is then placed at the anchor's edge of the inset-modified containing block.
  If that block's size is negative, it gives way at the inset that is not the anchor. Auto margins share the space
  that is left, and in the inline axis the start margin is zero if that space is negative
  (css-position-3/#abspos-layout, csswg-drafts#11478). Placing the box again costs a lookup.
- A positioned inline box's containing block runs from the start corner of its first part's padding box to the end
  edges of its last part's padding box, in its own writing mode, with an inline size of at least zero. The first and
  last parts are the visual first and last on a line, and its rows around blocks count only if it has no part on a
  line. At each of its layouts, its block container binds each box that is still escaping from its lines to the
  innermost positioned inline box around it, before holding the remaining boxes itself. A placeholder on a line
  resolves in bidi as a neutral with no text.
- A box out of flow adds to its holder's overflow, and through it to its ancestors' overflow, but to no scroll
  container's in-flow bounds, and a fixed box that the viewport holds adds nothing to what the viewport scrolls. It
  adds nothing to its parent's intrinsic sizes.

### Flex layout

- A flex container (`layout_flex.h`) is a block-level or inline-level box that block layout lays out as it lays out any
  box that establishes a formatting context, except for its content. Its items are solved, which finds their sizes,
  its lines' cross sizes and their offsets, and then each item is laid out at its final request and placed. Layout
  decides which children are items from the tree alone (`layoutEachItem`), so a producer that makes layout nodes
  directly has the same items as one with a cascade. The items are every child in flow that generates a box, whatever
  its display, and each run of text and line breaks that is not white space alone, which becomes an anonymous item. A
  child out of flow ends a run.
- What a container asks of an item is a request in each of the item's own axes (`LayoutRequests`): a fixed border-box
  size, with whether it is definite for the item's content; the size of its content, with its preferred size and
  limits unread; or its own sizing. The requests travel beside the space and are part of the key that a result is
  cached for. Every request that a container makes is fixed in the item's inline axis.
- An item is sized by its own axes, not by whether its container is a row. An item whose inline axis is the main axis
  takes its base and minimum from its intrinsic sizes and is measured for its cross size. An item whose block axis is
  the main axis (an item of a column, or an orthogonal item in a row) is measured for its base at its cross size, and
  its inline size follows the ordinary rules. An item with a preferred aspect ratio takes that inline size from its
  flexed size through the ratio. An item with a ratio and a definite cross size, preferred or stretched in a single
  line, takes its flex base size from the cross size, and its automatic minimum weighs the transferred size suggestion:
  the lesser of it and the content's for a replaced item, and the greater for any other item. An orthogonal item is
  measured in the space that it is laid out in, with its inline axis along the container's block axis.
- Items are collected into lines by their hypothetical outer sizes, and each line flexes its own items. A line's cross
  size is the container's if the container is single-line and its cross size is definite, and otherwise comes from its
  items and baseline groups. A multi-line container's lines then stretch to its cross size by `align-content`, which
  places them, with the gaps between them. A multi-line column's max-content cross size lays it out with each item's
  cross space set to its largest contribution.
- A measurement (`layoutMeasure`) asks an item for its block size, and for its baselines if the asker reads them. The
  answer is kept in the item's measurement records, a short list, newest first, that a box joins when it is first
  measured and that is invalidated with its intrinsic sizes. A record without baselines answers only for a size, and a
  record with baselines replaces it. A measurement never writes the item's layout entry, which holds its final layout.
  A block container that is measured is laid out, and the layout is handed to its final layout in the same root layout
  if the final layout is at the same request (`layoutFinal`). A flex container that is measured is solved alone, from
  its items' records, and builds no fragment. Nothing changes during a root layout, so a request asked again has the
  same answer. A layout that keeps no records, such as a reference layout from scratch, remembers its measurements
  while it runs (`LayoutMemo`).
- An item is measured only if its container needs what the measurement answers. A stretched item in a line whose
  cross size is the container's takes the line's cross size, and an item whose style fixes its cross size is asked
  only for its baselines.
- The alignment properties are resolved where a line is placed, in the container's logical axes. `start` and `end` are
  the container's. `self-start` and `self-end` are the item's own along the cross axis. `left` and `right` are the
  line-left and line-right if the main axis is the inline axis, physical left and right if the main axis is otherwise
  horizontal, and start in a horizontal column. Safe alignment, and the fallbacks of the distributed values, put what
  overflows at flex-start. Baseline alignment needs the item's block axis to be the cross axis, and otherwise falls
  back to safe self-start or self-end.
- A container's baselines are those of its startmost and endmost lines along the block axis. In a row, an item of the
  line that is aligned by baseline gives the line's baseline, and otherwise the line's startmost or endmost item along
  the main axis gives it, as in a column. An item with no baseline there has one synthesized from its border box's
  block-end edge. On a line, an inline-flex box aligns by its first baseline under `baseline-source: auto`, whereas an
  inline-block aligns by its last.
- A child out of flow has its static position where the container's sole item would be: in the content box, placed by
  `justify-content` and its own `align-self`, with stretch and baseline alignment treated as start. A static position
  can center the box, and carries whether its alignment is safe, which puts a box that overflows its inset-modified
  containing block at its start.
- A collapsed item takes no room in its line, which is at least as tall as the line that it was in with every item
  visible (its strut). A first round sizes and stretches the lines with the item visible, and the round that counts
  collects it at zero size and drops it. It is absent from the final tree, as a box of display none is
  (`layoutIsAbsent`): it has no fragment, rects or hits, and nothing in it paints. It takes no part in a row's
  intrinsic main sizes, and its strut keeps its part in a column's cross size.
- A flex container that scrolls holds its items' margin boxes in its scrollable overflow, so it scrolls to the end of
  an item's margin in either axis (css-overflow-3/#scrollable-overflow). A container that does not scroll gathers none.
- An item paints as an inline-block does, with its container's inline content, in the order that its container placed
  it. An item with a z-index other than auto paints whole at that level, as if positioned, and makes a stacking
  context.
- What a container reads of its items (their order, flexibility and alignment) is the container's input. A change to
  only those values marks the container, and the item is laid out again only if its request changes.
- A container's intrinsic sizes come from its items' contributions, by the Web-compatible algorithm. A row sums them
  with its gaps, each contribution being the item's ordinary contribution capped and floored by its hypothetical main
  size as its flexibility allows, with a content base sized under the constraint. A multi-line row's min-content size
  is the largest contribution.
- An anonymous item's layout entry, intrinsic sizes and measurements are kept under the first node of its run. A change
  to any node of the run drops them. A change to which children surround a run, or to whether a child generates a box
  or is out of flow, marks the content on either side of the change, so every run that the change splits, joins or
  extends is reached.
- `prLayoutTreeStats` counts a flex layout's work separately: measurements made, taken and replaced, measurements that
  final layouts used, containers solved alone and laid out, and the items that each round of resolving flexible
  lengths visits.

### Table layout

- A table (`layout_table.h`) is a block-level or inline-level box that block layout lays out as it lays out any box
  that establishes a formatting context, except for its content, as it does a flex container (The shell and its
  content, above). A node records how its children are laid out (`LayoutChildren`), and each walk over a container's
  children asks it once: `layoutEachChild` for flow, `layoutEachItem` for flex and grid, and `layoutEachTableBox` for a
  table, its row groups, rows, columns and column groups.
- A table is fit-content for an auto width, never narrower than its min-content width, and centred by auto margins
  (css-tables-3/#layout-principles). Its intrinsic sizes come from its columns' measures, its border spacing, its edges
  and its captions' min-content contributions. A percentage column asks for a table as wide as its max-content width
  over its percentage, and the other columns for their max-content widths over what the percentages leave.
- The grid is found from the tree alone at each layout of the table, with the anonymous parts that fix up its
  structure (css-tables-3/#fixup-algorithm): rows in no row group, cells in no row, and an anonymous cell around each
  run of other content, laid out as a block container over that run. White space between parts is dropped. A part with
  no box has no fragment. A run of table parts outside a table, in a block container's flow, is wrapped in an anonymous
  table, a block-level box that avoids floats. A run in an inline box is wrapped in an anonymous inline table, an
  atomic inline of the box's line with the initial box style, which inherits from the inline box. Children of a column,
  and of a column group other than its columns, generate no box. Columns that no column box defines, and that the same
  cells span as the column before, merge in auto mode (css-tables-3/#dimensioning-the-row-column-grid--step2).
- A table lays out each cell through the cell's own cache, as flex layout lays out its items: measured at its columns'
  width for its content's block size and first baseline (`layoutMeasure`), and laid out at its rows' height
  (`layoutFinal`). A cell whose final size is the size it was measured at, and that its alignment does not move, asks
  for its content's size again and takes the measurement's layout, so it is laid out once. A cell aligns its own content
  by its vertical-align, and a block container by its align-content (css-align-3/#align-block); the table gives a
  baseline-aligned cell only its row's baseline (`LayoutCellSpace`), which its result keys on. An anonymous
  cell or table keeps no cache, and is laid out again with its table or parent.
- Columns are sized by css-tables-3's measures, span by span, then by its four sizing guesses and the rules for excess
  width, or in fixed mode from the column boxes and the first row alone. Rows are as tall as their own heights, the
  cells they alone span, and their baseline-aligned cells above and below the row's baseline; spanning cells then grow
  their rows, and the table's height grows its rows, auto rows first.
- A table's fragment is its wrapper box: its captions, between its margins and its grid, and its grid's parts, its
  column groups, columns, row groups, rows and cells in tree order (`LayoutFragmentFlag_Table`). The fragment holds the
  rectangle of its grid, over which the table paints its decoration, and its grid's collapsed borders. The table, as its
  wrapper, is the containing block of the boxes out of flow that it holds.
- A collapsing table resolves each segment of its grid's lines among the cells, rows, row groups, columns, column groups
  and the table (css-tables-3/#border-conflict-resolution-algorithm): hidden wins, then the widest, then the style's
  precedence, then the cell over the row, row group, column, column group and table. A cell's border is half of its
  widest segment on each side, and the table's the outer half of its outermost segments, with no padding or spacing. A
  cell reads its borders through its space (`LayoutSpace::cellBorders`), and its result keys on them, so a change to a
  neighbour's border lays out the cells it moves. Border styles and colors of a table's parts are inputs of the table's
  layout.
- Tracks and track groups paint no decoration. Before a table's parts, painting draws the backgrounds of its column
  groups, columns, row groups and rows, each placed in its own box and clipped to the border boxes of the cells that
  start in it (css-tables-3/#drawing-cell-backgrounds); after its cells' backgrounds, its collapsed borders. A cell of
  `empty-cells: hide` with no content paints nothing in the separated model. A column is never hit.
- `prLayoutTreeStats` counts the tables laid out; their cells' measurements count with flex items'.

### Grid layout

- A grid container (`layout_grid.h`) is a block-level or inline-level box that block layout lays out as it lays out a
  flex container. Its items are made from its children as a flex container's are (`layoutEachItem`): each child in
  flow, blockified, and an anonymous item for each run of text. Items paint in order-modified document order, and an
  item's z-index applies whatever its position (css-grid-1/#z-order). Subgrid and masonry are not built.
- **Style.** The grid group (`PrLayoutGridStyle`) holds a container's templates, its implicit track sizes and its
  auto-flow, and an item's four placement lines. Gaps, the alignment properties and `order` come from the flex group.
  A track list is interned as one entry, as background layers are: its track sizes, the names of its lines, and at most
  one automatic repetition with names of its own. A repetition by a count is the producer's to expand. A template's
  areas are interned the same way. A name is a number that the tree interns for its life, together with the `-start`
  and `-end` forms that an area gives its edges and that a placement by a name alone reads
  (css-grid-1/#implicit-named-lines).
- **Placement** follows css-grid-1/#auto-placement-algo over the items in order-modified document order. Lines resolve
  by number and by name, with implicit lines counting as named where the explicit grid has too few. Conflicts resolve
  as css-grid-1/#grid-placement-errors says. The implicit grid holds lines -10000 through 10000, and an area past them
  is clamped into them. Auto-placement reads an occupancy bitmap with a row of bits for each track of the flow's axis.
  An automatic repetition repeats as often as the container's definite size, maximum or minimum allows, and auto-fit
  then collapses its empty tracks and their gaps (css-grid-1/#auto-repeat). The container's intrinsic sizes count only
  its limits, because its preferred size is what they stand in for. css-grid-1 counts the preferred size too, which
  would make a fixed-width grid's min-content size that width, and no implementation does.
- **Track sizing** follows css-grid-1/#algo-track-sizing, the columns first and then the rows at the columns' sizes.
  Columns of a container whose inline size is being measured are sized under a min-content or max-content constraint.
  Rows of a container whose block size is indefinite are sized under a max-content constraint, and again with the
  resulting size if a percentage of a row or of the row gap reads it (css-grid-1/#track-sizes) or a row is flexible,
  whose share of that size can differ from its share of the constraint. Orthogonal items contribute to the columns
  through the rows, so a grid that holds one sizes its columns and rows once more. Before the rows are sized, the rows
  that are not fixed are infinite (css-grid-1/#algo-grid-sizing), so an orthogonal item's inline size takes the
  fallback of css-writing-modes-3/#orthogonal-auto rather than the container's definite block size, which
  implementations disagree on using. Items aligned by their baseline along the rows are shimmed in their
  baseline-sharing groups, by the row where their span starts or ends (css-grid-1/#algo-baseline-shims).
- **Items** are measured and laid out through their own caches, as flex items are (`layoutMeasure`, `layoutFinal`). An
  item's size along its inline axis is fixed by the grid: it stretches over its area unless its alignment, an auto
  margin, a preferred size or an aspect ratio says otherwise, and its content-based minimum floors it
  (css-grid-1/#min-size-auto). Its block size is fixed if it stretches, and is its own otherwise, with percentages of
  its area. It is then aligned in its area by `justify-self`, `align-self` and auto margins, and the tracks in the
  content box by `justify-content` and `align-content`. A measurement of a grid container solves it alone, from its
  items' measurements, and builds no fragment.
- **Baselines.** A container's first and last baselines come from its first and last rows that hold items: from the
  items aligned by baseline there, or else from the first item in grid order (css-grid-1/#grid-baselines).
- **Boxes out of flow.** A box whose containing block is the grid container is placed in the grid area that its
  placement names, with auto lines and lines that the grid lacks at the container's padding edges
  (css-grid-1/#abspos-items). A child out of flow takes its static position as the sole item of its static-position
  rectangle: the grid area, if the container is its containing block, or else the content box
  (css-position-3/#staticpos-rect).
- **The resolved track list.** A grid container's fragment holds the used sizes of its tracks along each axis, which
  `prLayoutNodeGridTrackSizes` reads, with the names that its template gives the lines (`prLayoutNodeGridLineNames`).
  A document serializes them as the resolved value of grid-template-columns and grid-template-rows
  (css-grid-1/#resolved-track-list).
- **Changes.** An item's placement, order and self-alignment are its container's input, as an item's flex values are,
  so a change to them lays out the container, and the item only if its area or request changes.
- `prLayoutTreeStats` counts the grid containers laid out and those solved alone for a measurement.

### Floats and margins in a block formatting context

- A context keeps its floats' margin boxes in its own coordinates (`layout_exclusion.h`): a line axis measured from
  its content box's line-left edge, and a block axis measured from its block-start edge. It also keeps values that only
  grow as floats are added: the last float's block-start, and the lowest block-end on each side. The space between the
  floats over a block range, and the next float edge below an offset, are found by scanning the floats.
- A box in flow is laid out with its containing block among the context's floats and the margins before it, which form
  a chain from the last content edge (`layout_margin.h`). Its border box's block-start edge is known once the chain
  resolves: at its first content, its block-start border or padding, a box that avoids floats or has clearance, or its
  end if it does not collapse through. A float met before that point waits. A layout that finds where the margins
  resolve while floats are waiting stops there, and the box whose layout began with no floats waiting from outside
  lays out its children again, given the resolution.
- A block container's block-start edge is in one of four states. It is open while its margins collapse and where they
  resolve is not known. It is located if a layout that started over says where they resolve, although they still
  collapse. It is closed once they have resolved and its content is past them. It is stopped if they resolve while
  floats are waiting. Each kind of child (a float, a box that avoids floats, a box in flow, a run of inline content)
  moves the edge between these states, and nothing else does.
- A float is laid out through its own cache, independent of where it goes, and placed by CSS2's rules. A float with no
  outer block size excludes nothing. A box that establishes a context, or a block-level replaced box, is placed beside
  the floats over its block size, with its margins overlapping them, or is laid out as any block box if no float meets
  it. Clearance puts a box's border box at the bottom of the floats that it clears if its hypothetical position is not
  past them, and always if they adjoin its position.
- A box in a context keeps what its subtree read of the floats that existed when it began, measured from its border
  box's block-start edge: the block range of its queries, the floors that it read, and the floats in that range. It
  also keeps the floats that its subtree placed. The box is reused if it would read the same values at its current
  position, and its floats are placed again. If there were no floats in its range either time, it is reused wherever
  it is.
- A context's state is present whether or not it holds floats. A line records its whole block range, so that a float
  placed later finds the boxes that it moves, and a subtree may place a float, which the margin chain places. Anything
  that no float reads costs nothing extra. If a context holds no floats, no floor and no waiting floats, and a cached
  result read none, the result is reused without finding where it goes, and the range that it read is recorded where
  its parent places it, at the offset that placing gives. Placing is described once, for a box laid out and a box
  reused alike.
- A box's intrinsic sizes (min-content and max-content, the sizes that shrink-to-fit and the sizing keywords take) are
  measured from its content, with no inline base, and kept separately from its result. They are reused while nothing
  in the subtree has changed and, if they read it, the block-axis percentage base is the same. A change drops the
  intrinsic sizes of each box that it marks, because a layout that never measured a box still clears its marks. An
  inline formatting context keeps no intrinsic sizes of its own. It is measured again, from its kept text, when its
  container is measured, with each piece that starts an indented line widened by the indent, and with a percentage
  treated as zero.
- The things that read the viewport's extent, which no space carries, are listed by the tree, and a change of the
  extent marks each of them for layout.
- Font metrics, the half-leading and border widths snap to the tree's target pixel ratio. Every box laid out holds
  metrics, and no mark reaches them. A border width is snapped as css-values-4 snaps a border width, once for each box
  group as it is added and again for every group when the ratio changes, beside the group that interning compares. So
  a producer gives the width that it specifies, layout reads the snapped width at no cost, and a query
  (`prLayoutNodeBorderWidth`) reads back the width that layout used. A new ratio makes the tree forget its layout before
  it next lays out. The tree keeps its shapings, which hold no vertical metric.
- `prLayoutTreeStats` counts what a layout did, and `prLayoutTreeVerify` checks a layout's result against a layout
  from scratch. Tests assert both: what a change costs, and that what was cached is right.
- A worker (`pr/layout_worker.h`) is the layout stage of a pipeline: one thread that lays out and records the trees
  posted to it, in turn. A job holds its tree and the tree's context until the worker is waited for, so a client
  changes trees between jobs, and one worker serves every tree that its client's thread presents.

## Replaced elements

- A replaced element is a node with no children, whose content the client gives as data: its natural dimensions,
  which size its box if its style leaves a size auto (by CSS2's rules, the default object size, and the limits that
  keep a ratio), and what it draws. An `aspect-ratio` without `auto` replaces the natural ratio, and the natural inline
  size is kept. A producer gives an SVG image's content as the draw list of its document drawn over its natural size,
  whereas the host decodes other images to a bitmap. Setting the natural size lays the box out again. Setting the
  content records again and lays out nothing.
- Content is a draw list in the content's natural space, which painting fits to the content box by `object-fit` and
  `object-position`, in the decoration group, and clips to the content box. So content that only scales (an image, a
  canvas, a video frame) never depends on the box's size, and a recording of it stays independent of where the box is.
- Content that draws differently at another size, not just scaled, is a painter: a function that the tree calls with
  the content box's extent, on the thread that records the scene, and that draws the same for the same extent and
  client state. The tree keeps its drawing for that extent until the client invalidates it, and the from-scratch check
  calls it again and compares.
- Content that another scene fills (an iframe's document, and later a video's frames) is embedded. The recording names
  a slot with the content's id where its box paints, as it names a layer, and draws nothing there. Whoever plays the
  scene fills each slot (`PrLayoutScenePlayOptions::embed`), so the embedded content updates without the scene around
  it recording again.
- `stretch` is a sizing value of every box: the containing block's size minus the margins, or auto if that size is
  indefinite.

## Inline layout

The items of an inline formatting context are:

- the text of the context's text nodes, with white space collapsed and then transformed, end to end in one buffer;
- the starts and ends of inline boxes, at their places in that text;
- atomic inlines (replaced elements, inline-blocks, and inline boxes of another writing mode), each an object
  replacement character in the text that lines may break before and after, whatever UAX #14 says about the character
  beside it, except for GL, WJ and ZWJ characters other than U+00A0;
- line breaks, each a line feed;
- floats and boxes out of flow, at their places, with no text.

The text is itemized into shaping runs, each with one face, script, orientation, bidi level and shaping style, which
cross item boundaries wherever nothing that affects shaping changes, and each run is shaped once. Lines break greedily
at the break opportunities of the whole text, one line at a time. Each line is placed in the space beside the floats at
its block offset. A line is broken again if it turns out taller and the space over its height is narrower, and is
moved down if nothing fits. A float goes on the line on which it is met if the line has nothing before it or the float
fits in the space that is left, and after the line otherwise.

A line's children are its inline boxes' parts, its text runs, its atomic inlines, its placeholders and its floats'
anchors, flat and in tree order. Each is placed in visual order and by its vertical alignment. An inline box's parts on
the line come in visual order from its start edge, followed by its content on the line, with the boxes in it in turn.
The first part on the line where the box starts is its opening part, and the last part on the line where it ends is
its closing part. The last of a line's parts says how many of the children after it belong to the box. A float's
fragment is a child of the line's parent, and its anchor on its line names it.

Text is transformed (css-text-4/#text-transform-property) once a context is collected, before trimming and shaping. A
text item takes the transform of its own record, also on the first formatted line, so that the context's shapings for
both hold the same text. Each transform reads the rest of its segment's text as context: a word continues across
inline boxes for capitalize, and a final sigma looks past them. A transform can change the text's length, so the items
and segments move to the bytes they hold after it, and a byte that it changes is no longer white space.

What the text gives whatever the context's width is cached by the context's first node: the collapsed text, its break
opportunities, its glyphs, and, by byte, the advance before the byte and its first glyph, so that a line measures its
pieces without walking their glyphs. The cache is reused while the context holds the same nodes and none of them has
changed, so a new width breaks lines without shaping. A shaping is immutable once shaped. Its items carry their font
groups, which a change that keeps the shaping current never changes. The shaping holds a reference to each group,
because its runs index their faces, so a collection never frees a group that a cached shaping names and gives the
group's index to another group. A layout or measurement resolves each item's record, edges and atomic layout into its
frame. A line's content is collected once, and its alignment (`layout_line_align.h`) and justification
(`layout_justify.h`) read it, each with state of its own. An inline box's part records the physical sides at which it
breaks, across lines or at a bidi split, so painting reads no writing mode.

- A context whose block container establishes the formatting context and holds nothing else keeps the lines that its
  last layout made (`LayoutLineRecord`), under its first node. A layout of the container that is the same except for
  its block size, or for an available size that leaves the same inline size, takes those lines if it has the same
  collection of its shaping (`LayoutInlineShaping::collected`), the same content box and position, and each atomic
  inline laid out as the same fragment at the same relative offset. A context that placed a float, or whose inline box
  or atomic inline is moved by a percentage of the block size, keeps no lines. An atomic inline whose layout read the
  block size gives a different fragment at a different block size, so its lines are laid out again.
- Bidi levels are resolved when the context is itemized, by paragraph. A paragraph ends at a segment's bounds and
  after each forced break. Each inline box's `unicode-bidi` adds its controls around its content, closed at each
  paragraph's end and opened again at the start of the next. The container's override applies to each paragraph, and
  its `plaintext` takes each paragraph's level from its text. Upright text is strong left-to-right. Text in only
  left-to-right scripts in an `ltr` container resolves nothing: every level would be even, and its lines place their
  leaves in the text's order. A right-to-left run is shaped right to left, with its glyphs kept in the text's order and
  placed reversed.
- A font group holds a `lib/text` text font (`docs/text.md`), so layout sets a cluster in the same face that SVG text
  and Lottie would. A group with none takes the tree's default font. The group holds a reference to its text font
  while the tree holds the group. A tree that sets no text needs no default font and no shaper: a group with no text
  font has no faces, has size 0, and has its struts and baselines at its line's edge. Beside each group, layout keeps
  its text font's size and the rounded metrics of each face. A document's `@font-face` faces form a set of their own
  over the host's set, and its text fonts are in that set, so `font-family: X` in two documents gives two text fonts
  and two groups.
- Itemization belongs to `lib/text` (`pr/text_itemize.h`). The items become its pieces one for one: text, an inline
  box's start and end, an atomic inline as an object, a forced break or block as a paragraph break, and a float as a
  boundary that ends shaping. Its runs, levels, paragraphs and marks become the shaping's.
- A text font's answers are part of what a shaping and a box depend on. A layout brings the groups' text fonts up to
  date if the version of a set that they belong to has increased, or if a text font has joined the tree since the last
  layout. Otherwise it compares one version per set. If a text font's revision has increased, its groups' metrics are
  measured again, the shapings that name one of its groups are shaped again, and the nodes that it styles are laid out
  again. A change of the set that changes no answer costs nothing more. The layout's text requests the pending faces
  that it reaches, which the set's owner loads.
- A line places leaves: the pieces of its text in one shaping run and one level, each preserved tab, each atomic
  inline, and an empty leaf for a box that has nothing else on the line. The line's levels follow L1, and its leaves
  follow L2's visual order, from line-left. A preserved tab advances to its stop measured in the text's order, as
  breaking measured it, and moves with its leaf. An inline box's parts are runs of its leaves at consecutive visual
  positions, also split where reordering its paragraph on one unbroken line would split it (csswg-drafts#1509), and
  are found once, when the context is itemized. The box's start edge goes on its start-most part, in its own
  direction, on the line that holds its start, and its end edge goes on its end-most part on the line that holds its
  end.

- A font group carries what its text asks of shaping (`PrTextFeatureSet`, `pr/text.h`), which each run is shaped with,
  and of breaking (`PrTextWordBreak`, `PrTextOverflowWrap`, `pr/text_break.h`). Breaks are UAX #14's opportunities,
  tailored by lib/text (`prTextBreaksTailor`) only if some text in the context asks for it. word-break's break-all
  and keep-all change the opportunities themselves. overflow-wrap, and word-break's break-word, add emergency
  opportunities between grapheme clusters (`LayoutInlineShaping::emergency`). A line takes the last emergency
  opportunity that fits, and only if no ordinary opportunity fits. Only the opportunities of `anywhere` count for the
  min-content size. So a context whose text asks only for `break-word` finds its opportunities when a word fits no
  line (`layoutWordBreaks`). Text that does not wrap takes none.
- The first formatted line of a block container is laid out in the records that the producer gives for that line
  (`prLayoutNodeSetFirstLineStyle`). The context then has a second shaping, collected in those records
  (`LayoutInlineShaping::firstLine`). It has the same text and items, and only their font groups differ. The first line
  breaks, measures and sets its text in that shaping, in a frame of its own. The lines after it continue from where it
  ends, with the boxes it leaves open and the floats it placed. Its fragments are marked
  (`LayoutFragmentFlag_FirstLine`) and paint in the line's records. A context with no such record has no second shaping
  and pays nothing for it. Lines that a first line styles are not kept for reuse.
- A line is built in its own frame, measured from its line-left edge along the inline axis, and across from its
  line-over edge, which is the right edge for every vertical line except those of `sideways-lr`. So `vertical-lr`'s
  lines are laid out as `vertical-rl`'s are, and the line's fragment maps the frame to physical sides
  (`layoutLineFrame`). An atomic inline's block-axis margins and baselines are flipped into that frame when they are
  resolved.
- In a vertical typographic mode, each typographic character unit is upright or sideways, according to
  `text-orientation` and, under `mixed`, its first code point's `Vertical_Orientation` (UAX #50). The `sideways-*`
  modes set everything sideways. An upright run is shaped top to bottom, and its glyphs are placed down the line from
  their vertical origins on their face's central baseline. An upright run in a face with no vertical metrics advances
  by the face's rounded ascent and descent. A sideways run is shaped and placed as on a horizontal line, and its
  fragment says how painting turns it: a quarter turn clockwise, or counter-clockwise on `sideways-lr`'s lines.
  Advances are along the line either way, so breaking and measuring read nothing of orientation.
- A box on a line is aligned in the inline box around it, or in the root inline box, which is the container's strut.
  An inline box's extent and shift are measured about its dominant baseline, which also sets its text: `auto` is
  central in a vertical typographic mode except under `sideways`, and alphabetic otherwise. Each text fragment is its
  own face's content area, with that face's baseline of the dominant baseline's name on the item's baseline. An atomic
  inline's extent is its margin box about its alphabetic baseline. A replaced element's baseline is its margin box's
  line-under edge. An inline-block's baseline is the first or last baseline that its `baseline-source` picks, or that
  edge if it has none or if, under `auto`, it is a scroll container. A line exports its alphabetic baseline, with the
  baseline set of the container's font.
- `alignment-baseline` aligns a box's baseline of a given name with its parent's baseline of the same name, which by
  default is the parent's dominant baseline (css-inline-3/#alignment-baseline-property). `text-top`, `text-bottom` and
  `middle` use the box's edges and the parent's content area and x-height, and the rest use the font's or the box's
  baseline set, or the middle of the margin box if it has none. A face with no ideographic baselines has them at its
  rounded ascent and descent, with its central baseline midway between them. A face that has them, in its BASE table,
  is centered by those. `baseline-shift` then moves the box, or makes it the root of an aligned subtree at `top`,
  `center` or `bottom`. A box's local shift depends on styles and on the atomic inlines' layouts, so it is found once
  per layout of the context, and the shifts are summed down the tree with it. The exception is a box aligned by its
  leading box whose text uses fallback faces, for which the context sums the shifts per line.
- A line is as tall as the root's aligned subtree, including its strut. Then each `top`, `center` or `bottom` subtree,
  in the order in which the line meets it, grows the line if it is taller: below for `top`, and above for the others.
  Relative positioning then moves an atomic inline, and a relatively positioned inline box moves its parts and
  everything that they hold (each leaf by the boxes open around it), together with the rows around the blocks in it
  and those blocks. Nothing else on the line moves. Its percentages are of the block container.
- A font's ascent, descent and line gap are in whole target pixels, and a leading box puts its half-leading above the
  baseline rounded down to a whole target pixel, so that the baseline, and the text set on it, fall on whole pixels.
  On lines of normal height, a text run is as tall as the faces that it uses on that line. An explicit line height
  uses only its first face.
- A line is a phantom line, zero high and with no baseline, if it holds no text, atomic inline or forced break, and no
  inline box's start or end with a non-zero inline-axis margin, border or padding there, each judged on its own as
  specified. A segment with no text still makes a line if it holds an inline box's start or end or a box out of flow,
  so that every inline box has a part on the lines where it starts and ends (CSS2 §9.2.1.1). Floats alone make no
  line. A float beside only phantom lines adjoins the margins around them, as a float with no line does. The parts of
  the inline boxes on a phantom line are zero high at its top. The items after a forced break that ends the text are on
  a line of their own. A line break that clears puts what follows it below the floats on those sides, which is a floor
  that its line reads.
- A line's content starts at the container's text-indent if the line is indented, and is aligned in the space left by
  text-align, or by text-align-last for the last line and for a line that a forced break ends. It is aligned against
  the line's start, end, line-left or line-right edge, or centered, to a unit. The white space that a line ends in
  hangs: all of it at a soft wrap, and otherwise only what passes the line's end. A justified line shares its remaining
  space over its opportunities in units, with the first opportunities getting one more unit, so that it fills its space
  exactly. The opportunities are each word separator, according to its text's text-justify, and each gap between
  grapheme clusters that shaping leaves apart, according to their nearest common box's text-justify, after the line's
  last tab. The shares are added as the line is placed in visual order: a gap's share on the side that its text's
  order starts from, and a separator's share on its end side. A line with no opportunity aligns as its last line
  would, centered if that justifies. Alignment is in the paragraph's direction, with the white space that hangs at its
  end side.
- A block-level box in flow inside an inline box splits the context (CSS2 §9.2.1.1). The context is collected and
  shaped whole, with the box as a Block item at its place with no text, and the items between Block items are
  segments. Each segment is laid out and measured as an inline formatting context of its own, in an anonymous block,
  with its own break opportunities, shaping context and measuring, and the edges at a cut go with the segment whose
  items they are. Block layout lays out a run's segments, and between them each block as a block-level child, through
  the margin chain and among the floats. The block's containing block is the container's content box. The inline boxes
  open across a block break there, with their parts lacking the edges on the block's side. Each holds a row around the
  block: a fragment across the container's content box, as tall as the block's border box, which paints nothing, holds
  the block, and reaches as far as the block does. A row is new with each layout of its container, so it has no
  recording of its own: the walk passes through it to the block, which replays its own recording. Hit testing takes a
  row under its block, and geometry takes it in content order among the parts. A run is open while its blocks are
  laid out, so it holds its frame, and the blocks' own contexts take the next depths.
- An inline-block lays out its own content, which may hold contexts of its own, so inline layout and measurement nest.
  Each depth of nesting has its own scratch frame, made once and kept.
- Every atomic inline, float and block-level box inside an inline box is laid out through its own cache, as a
  block-level box is. Collecting a context does not descend into one or clear its marks, and a change to or inside one
  that keeps its kind leaves the context's text current: the box is laid out again, together with its line or its
  segments.

## Painting (`pr/layout_scene.h`)

Painting and hit testing share one walk in CSS painting order (`layout_walk.h`). A stacking context paints its
background and border, then the boxes of its list below level 0, then its flow's block backgrounds, then its floats,
each as if it made a stacking context, then their inline content, and then the boxes of its list at level 0 and above.
The phases of its content pass over every box that paints whole (a positioned box, or a box that makes a stacking
context), and over every box out of flow and placeholder. Tree order is logical: a line's children are in it, and the
floats phase visits the floats where their parents hold them. A fragment says whether it belongs to a float and
whether floats are among its descendants, so the floats phase walks only toward them. An inline-block that does not
paint whole paints at its place on its line as if it made a stacking context: its decoration, its block-level content,
then its inline content. The root's background paints the canvas.

A box's decoration is its background, its border, and its outline, painted with them, after the border. A later box
covers the outline, whereas CSS2's Appendix E paints outlines last in their stacking context. A visitor of the walk
declares only what it does, over defaults that decline, and says which subtrees it enters. Hit testing enters a
subtree only if the area in which its boxes can be hit, through the transforms of the boxes that paint whole, holds the
point. The tree keeps each fragment's area while it holds the fragment and the effects of the fragment's nodes are the
ones that the area was found for.

A background is a color under a list of layers (css-backgrounds-3). The color fills the box that the bottom layer's clip
names. Each layer is an image -- a decoded image, a draw list, or a gradient -- sized and placed in the box that its
origin names and repeated within the box that its clip names. The tree interns a box's layers as one entry, as it
interns a calculation (`layout_background.h`), so the decoration group that names them compares as bits, equal lists are
one entry, and a layout frees an entry that no group names. A layer paints as one brush over the area that its tiles
cover: a gradient that one tile covers paints as a gradient brush, and any other image as an image brush whose source
repeats it, which for a gradient is a list that draws one tile. A gradient's stops resolve against its line or ray for
each tile's size, and its brush spans the range of its stops, since a brush pads past its range by clamping. A repeating
conic gradient repeats its stops around the turn. A client may draw a gradient without a tree (`prLayoutGradientDraw`)
if its lengths are pixels or percentages, as an SVG mask layer does. The root's background paints the canvas: its color,
and its layers placed by the root's boxes over the whole canvas. Each fragment of an inline box places its layers in its
own boxes, as `box-decoration-break: clone` does. A scroll container's layers that are attached to its content (`local`)
are recorded with the content, so they scroll with it. They are placed and painted in the scrollable overflow area. The
background color goes with them if the bottom layer is attached to the content.

A layer blends with the layers below it and the background color by its blend mode, or composites onto them by
plus-lighter (compositing-1/#background-blend-mode). A background whose layers blend is an isolated group of its color
and layers, so that nothing beneath the box shows through the blending. A background whose layers all blend normally
records no group. Within a blending background, a layer fixed to the viewport is placed as a scrolling layer, as it is
under a transform, since the group is one recording. A scroll container's layers that scroll with its content form a
group of their own. The root's blending background is one group over the canvas, with its color in it, and the canvas's
color is then the tree's alone.

An image of a layer, a mask layer or a mask border may carry a filter list, as filter() does
(filter-effects-1/#FilterCSSImageValue). The image is drawn at its concrete object size, the tile's size, and the list
filters it with that size as its filter region, so the result keeps the image's size and origin. A blur extends the
image's edges, and a source names a filter element, whose object bounding box is the image. The tree copies a layer's
items, and their sources count as references.

The canvas's color is the root's background color over the tree's canvas color, which a client sets
(`prLayoutTreeSetCanvasColor`), as a document gives the `Canvas` system color. A scene reports it
(`prLayoutSceneCanvasColor`). The presentation pass belongs to the application, which can fill the canvas more cheaply
with its pass's clear than a draw can. An application that does tells the player
(`PrLayoutScenePlayOptions::canvasFilled`), or the compositor (`PrLayoutCompositorDesc::canvasFilled`), which then
records no fill for the canvas, and the root's background layers play over the clear. An application that draws beneath
the scene in the same pass lets the scene fill its canvas.

A layer fixed to the viewport (`fixed`) is sized and placed in the viewport and recorded in the viewport's space, in a
segment of its own. The segment keeps its box's clip. The scene plays the layer within that clip and offsets it by the
scroll offsets around it, so the layer stays in place on screen while its box scrolls. If a transform applies to the
box, the layer is placed as a scrolling layer is (css-transforms-1/#transform-rendering). A change of the viewport's
extent records again the boxes that have a fixed layer.

Painting order comes from the box tree, and positions come from layout. A stacking context keeps the list of the boxes
that paint whole in it (`layout_stacking.h`): the boxes in its subtree that no stacking context inside it contains,
sorted by level and then by tree order. The stacking contexts are the root, a positioned box whose z-index is not auto,
a fixed or sticky box, a box with opacity, a box with a transform or a perspective where transforms apply, a box with a
clip path, a mask, a filter, a backdrop filter, a blend mode or isolation, and a box that a compositor animates. A clip
rectangle makes none. It clips as an overflow clip does, as a container around the box's content, and it also clips
the box's own decoration (css-masking-1/#clip-property). A positioned box at z-index auto paints only its own content,
and the boxes that paint whole in it belong to its context's list, after it in tree order
(CSS2/#elaborate-stacking-contexts). A list is found from the box tree, following a mark on each node that leads to a
box that paints whole, when a paint or a hit test first needs it. It is found again if an insertion, a removal, or a
change of whether a box paints whole, of its level, or of whether it makes a stacking context, makes the list stale.
Layout changes no list.

A box of a list paints where the last layout placed it (`LayoutPlacement`, under Trees), from its stacking context, in
its own containers: the boxes that clip their content along its placements, which for a box out of flow are its holder
and the holder's ancestors. A box out of flow therefore escapes the clips and scroll containers between its holder and
its placeholder, and a fixed box that the viewport holds escapes the viewport's scrolling. A recording whose box places
one of its list's boxes through a box outside it is current only while that box stays where it was.

An inline box that paints whole (a positioned inline box, or one that is a stacking context because of its opacity)
paints from its list with its own phases: its list if it makes a context, the blocks in its rows, its floats and those
inside its rows' blocks, and its parts and content line by line. Its pieces (its group of parts and content on each
line, its rows, its anchored floats) are found from its opening part through its container's children, and its
container's own phases pass over them. A box with opacity is one group: a layer of its own, recorded from its
container's border box and keyed by its node, which is current while its container's fragment is the one it was
recorded from and nothing in the box has changed.

A box with its own layer is recorded separately, in its own space and without its effects. Such boxes are the root, a
box that its opacity or a moving transform makes a stacking context, a box with any of the compositing group's effects,
and a box that a compositor animates. An inline
box with a layer is recorded in its container's space. This also holds for atomic inlines (replaced elements on a
line): one that paints whole is taken out of its line's inline content and painted with the boxes that paint whole. A
positioned box without effects, and a box whose transform moves nothing, paint whole in the recording of the stacking
context around them. A scroll container's content is recorded unscrolled, in segments of the recording that name the
container that they are in, so no recording depends on a scroll offset. A recording names the layers in it where they
paint among its segments, and a scene plays each layer there, with its effects and in its containers.

Painting puts edges on target pixels, and geometry stays exact: fragments, placements, queries, hit testing and
recordings keep exact positions. A recording marks what snaps and how (`PrDrawEdges_Snapped`, [draw.md](draw.md)), and
lib/draw rounds it where the list is played, under the transform that places it. So a recording does not depend on
where it lands, and moving it records nothing, while every edge rounds from its position on the target.

- **Edges round to the nearest target pixel, half up, from where they land.** A box's border box, its border's inner
  edges, its background's color and layers, its outline, a replaced box's content box and the image fitted to it, and a
  container's clip each snap. So abutting boxes share one edge with no seam, and a box nested at fractions lands where
  its total offset puts it, not where each offset rounded alone would. A border's sides are as wide as the rounded edges
  leave them. Content that a list or a painter draws for a replaced box is fitted to its rounded rectangle.
- **Text** snaps in the block axis: the translation that places a run in its line rounds to a target pixel there, and
  the baseline with it, since metrics are whole target pixels. Along the line, glyphs stay where layout placed them.
- **Transforms are not snapped.** A transformed layer's transform is pushed apart from snapping, so its content rounds
  where it would lie without the transform, which then moves it. Snapping inside the layer does not depend on its
  transform, so content under an animated transform moves as one, without rounding again as the transform changes.
- **Scroll offsets** are rounded to whole target pixels where a scene plays them, so that a scroll moves what it shows
  by whole pixels. The offsets that the tree and queries hold are exact.

Recordings are cached at three granularities, each keyed so that a change records again only what it reaches.

- **Layers.** A layer's recording is reused while its fragment is the same and nothing that it read of its nodes has
  changed. A change marks the node and its ancestors up to the first one with a layer, so the check costs the same
  however much the layer holds. A change to a layer's effects records nothing, a change to what it paints records that
  layer alone, and a layout that only moves it, or a scroll, records nothing.
- **Subtrees.** Within a recording, every block subtree that draws, and every box that paints whole, has its own
  recording for each painting phase, keyed by fragment, which holds the boxes of its list if it is a stacking context.
  A subtree that draws nothing is also cached if walking it again would visit many fragments. Each records in its own
  space and is placed by a translation, so a cached recording is embedded wherever its subtree has moved.
- **Runs.** A box with more than `kPaintRun` children also records them in runs of that many, keyed by the box's node,
  the phase and the run's index. A run is reused while it holds the same child fragments at the same offsets from one
  another and nothing that they read has changed, so a layout that replaces the box's fragment keeps the runs that it
  did not touch. A change under a wide box records one run again and embeds one recording per run, instead of one per
  child. A stacking context's list of more than `kPaintRun` boxes also paints in runs, whose boundaries are decided by
  its boxes' nodes and which are keyed by their first box, so a box inserted, removed or moved between levels records
  only the runs around it again. A run of a list is reused while it holds the same boxes, with the same fragments, at
  the same offsets from one another and in the same containers.

Every recording (a layer's, a subtree's or a run's) has one structure: its segments, each in a container, the
containers that it opens, the layers and slots that it names, and the stacking contexts' lists that it painted from,
which it is current only while they are. A part that a cache keeps names the containers around it by node, and a
recording embeds it by appending it where it began, in the same chunk and container. The part's containers become the
recording's containers, or are containers that the recording has already opened, and the segments that it starts
become the recording's segments, with their lists taken as they are. So a part may span containers and name layers,
and costs its segments wherever it is appended. A part that places a box of a list through a box outside it, which can
move the box while the part stays as it is, is not cached. The chunk instead records where that box is, and is current
only while the box stays there. A recording names the container that a recorded item is in as one of its own, as a
container around it, or as the own space of a layer around it, by node, or as none, past the viewport.

A layout drops what is cached for the fragments that it replaces. A recording embeds its commands in batches of
bounded size, so that a player skips the batches outside its clip by their bounds (`prDrawListBounds`), and the cost of
playing a long document depends on what shows.

A paint is a scene (`prLayoutTreeScene`): the layers that it assembled, each with its effects and recording, and the
scroll containers with their clips and ranges. A scene is immutable and names nothing of the tree. A layer whose plane
map is affine plays under that map. A layer whose map is not affine plays as a projected group
(`PrDrawGroup::projection`): its contents are recorded into a list of their own when the scene plays, and the group's
bounds are that list's bounds, so a compositor's transform of the layer or of a layer inside it is bounded by what it
draws. A projected layer inside another flattens into the outer plane, and `preserve-3d`'s shared space is not built. A
scene holds no scroll offsets, so a scroll paints nothing and reuses the scene. Playing a scene records the list, with
any effects that a player gives in place of the scene's own, and with the offsets that it gives, which are the tree's
(`prLayoutSceneScrolls`) or a compositor's. A scene's containers form one tree across its layers, resolved by node
as the scene is assembled. A layer's opacity is one group around everything that it holds. Each segment, layer and slot
is placed in the layer by its own chain of containers, starting from the scene's space or from the nearest layer whose
transform places it, which holds every box out of flow in it. Consecutive pieces share the containers that their chains
share. A scroll finds what is under the point the same way.

### Effects (`layout_effects.h`)

A box's compositing group is resolved when its layer joins a scene, against the fragment that the layer was recorded
from, and the scene plays the layer inside what it resolves to.

- **Order.** The clip path clips the layer. The clip rectangle of a box with a layer also clips it, after the filter
  applies. Inside them, one group applies the filter, the mask and the opacity, in that order
  (css-masking-1/#placement), and composites by the blend mode. A backdrop filter is the group's backdrop, filtered and
  painted under it within the border box and its radii (filter-effects-2/#BackdropFilterProperty).
- **Reference boxes** are the fragment's border, padding, content and margin boxes. An inline box's border box is the
  union of its pieces' border boxes. A basic shape resolves in its reference box (css-shapes-1/#basic-shape-functions).
  A box keyword alone clips to that box with the corners of the border box's radii, reduced by the border and padding
  for an inner box, and grown by the margin for the margin box (css-shapes-1/#shapes-from-box-values). The radius
  percentages of inset() are of the reference box, and the radii are then fitted as border radii are. A polygon's
  round arcs each vertex, at most half the shorter of its sides.
- **The clip rectangle** applies only to an absolutely positioned box, with its auto edges at the border box's edges
  (css-masking-1/#clip-property). The box group records that it applies (`clipped`). The box is then a container
  whose clip is the rectangle, cut by its padding box in the axes that its overflow clips. A fixed box in it that the
  viewport holds escapes it, as it escapes an overflow clip.
- **Mask layers** are placed as background layers are, in the box of their origin and within the box of their clip, or
  are drawn by their source, and composite from the bottom layer up. A mask border is nine-sliced over the border image
  area, its middle opaque without `fill`, and masks the layers (css-masking-1/#the-mask-border).
- **Filters.** A filter function becomes nodes of lib/draw's filter graph that compute in sRGB, as their
  filter-effects-1 equivalents do (`prLayoutFilterAppend`). A source appends its own nodes. The filter's region covers
  the box's ink overflow, or the region that a source gives. A filter makes its box a containing block for absolutely
  positioned and fixed boxes, except on the root (filter-effects-1/#FilterProperty). A filtered layer plays its filter
  even if nothing was recorded in it, since a flood or an image of an element draws from nothing. A filter element's
  object bounding box on an HTML box is the border box with its ink overflow, and its user-space percentages are of the
  border box. A backdrop blur mirrors its edges within the border box.
- **Blending** composites with what lies beneath up to the nearest isolated group, and every stacking context is one
  (compositing-1/#csscompositingrules_CSS). A stacking context whose list holds a box that blends takes a layer for it
  (`layoutIsolatesBlending`) if its style gives it none, and a layer that holds a blending layer plays as an isolated
  group. A tree in which nothing blends pays nothing for this (`LayoutStyles::blendingGroups`).
- **Backdrop roots.** A backdrop filter reads what lies beneath it up to the nearest backdrop root
  (filter-effects-2/#BackdropRoot): the root, and a layer with a filter, opacity, a mask, a clip path, a backdrop filter
  or a blend mode, or whose will-change names one of them. The root's backdrop filter reads nothing.
- **The root's layer** plays without a frame of its own unless a clip or a mask needs its border box's space, so that
  its fixed boxes stay in place as the viewport scrolls. Its filter and blending apply around its containers.
- **Hit testing.** A clip path and a clip rectangle clip where the box and its content are hit. A mask and a filter do
  not (css-masking-1/#clipping-paths).
- **Curved containers.** A container's clip is curved at its padding box's corners where the clip is that padding box,
  in both axes (css-backgrounds-3/#corner-clipping). The recording's container carries the corners, so the scene clips
  to them and hit testing excludes what lies outside them, as the walk's hit test does.

## Animations and compositing (`pr/layout_animation.h`, `pr/layout_compositor.h`)

- Easing, the timing model and transform function lists belong to the engine, so a client's animations and the
  compositor agree on them.
- A track animates one box's opacity or transform, with a timing and keyframes of values. A box whose effects animate
  is a layer for the animation's duration (`PrLayoutEffectStyle::layer`), so its effects change how it composites and
  nothing else.
- A transform's keyframes are function lists, which interpolate as css-transforms-2 says: function by function while
  the kinds match, and the rest of each list as one matrix, decomposed. A compositor samples a track with the size of
  the layer's reference box, which resolves the lists' percentages, and plays the layer with the resulting matrix about
  the layer's origin, under its parent's perspective, as a projected group if the result is not affine
  (`prLayoutSceneSampleTracks`). A host that records a scene itself samples the same way, so its list shows what the
  compositor's frame at that time shows.
- A compositor holds the scene that a client last committed and copies of its tracks. Each frame samples the tracks at
  the frame's time and plays the scene with their values, on the compositor's thread if it has one, so composited
  animations run without the client's thread.
- A compositor holds a scene for each embedded content, committed by its id with its own tracks, and plays each scene
  in the slots that name it. A user's scroll over a slot scrolls the embedded scene first.
- A compositor scrolls the scene's scroll containers as a user scrolls them, from any thread, and chains any scroll
  that one container cannot take to the containers around it. It hands the offsets back to the client, which applies
  them to its tree. A commit says which offsets the client applied before recording its scene, and the compositor shows
  its own offsets until then.

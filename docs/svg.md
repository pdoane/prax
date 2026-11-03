# SVG

This document describes `lib/svg`. The module styles nothing itself: SVG content is styled by lib/css's cascade
([css.md](css.md)), built into a retained SVG tree, and drawn with lib/draw.

## Styling

- **Presentation attributes** are hints at the hint origin (css-cascade-5/#preshint), built in `css_hints.cpp` with
  HTML's. Each property that SVG 2 makes a presentation attribute is one, and so is a geometry property (cx, r, x,
  width and the rest) on the elements that it sizes. x and y are hints on every svg and symbol element, and an inner
  svg or an instanced symbol takes them from its style. An inner svg element's width and height are not hints, nor
  are a symbol's or a use element's, which are attributes alone (w3c/svgwg#1057). A `transform` takes SVG's syntax
  alone (css-transforms-1/#svg-syntax) and becomes the transform property. An attribute that holds more than one
  value declares nothing.
- **Lengths in an attribute** take every CSS unit (w3c/svgwg#1216). A number alone inside the functions of the filter
  attribute, such as `drop-shadow(4 5 6)`, is a length in px or an angle in degrees
  (SVG2/types.html#presentation-attribute-css-value). A negative length is replaced by the initial value in an
  attribute, is ignored in a declaration, and is clamped to zero when it comes from a calculation (w3c/svgwg#1202).
- **SVG's properties are computed values** in two style groups, and clip-path, the mask layers, filter, mix-blend-mode
  and isolation in the compositing group, which HTML's boxes share ([css.md](css.md)). `Svg` holds the inherited ones:
  fills, strokes, markers, paint order, rendering hints, text-anchor and pointer-events. `SvgGeometry` holds the rest:
  the geometry properties, d, path-length, vector-effect, and the properties of stops, filter primitives and masks. They
  inherit across HTML and SVG, and `getComputedStyle` lists them. A geometry property set by a style rule applies, as
  SVG 2 says.
- **SVG's user agent sheet** (svg2-draft/styling.html#UAStyleSheet, `svg_style.h`) follows HTML's.
- **A document that has no presentation**, such as an SVG image, an OpenType SVG glyph's document or a document that a
  harness draws, is styled by a cascade of its own (`svg_cascade.h`). Such a document does not change while it is
  drawn, so each style is computed when the tree first needs it, and kept. Drawing one element of a large document, as
  a glyph does, styles only that element's ancestors and subtree. This is SVG Integration's secure static mode: no
  script and no animation.

## The SVG tree (`svg_tree.h`)

A tree holds a node for each element that renders, with its geometry, its resolved paints, its transform and its
effects. A paint server, a marker, a clipPath, a mask or a filter is a resource that the nodes which reference it share.
A resource holds the data of its kind alone. A reference in a shadow tree names the element of its own tree first
(css-shadow-1/#css-tree-scoped-reference), and the document's otherwise. Building reads the document and the styles on
the document's thread. Drawing (`svg_draw.h`) reads only the tree, so the layout thread can draw it at a size known only
then. A length that a viewport decides, such as a percentage, stays unresolved until the tree is drawn.

A symbol that a use element instances is a viewport of the use's width and height, or of its own width and height
attributes, placed at its computed x and y. Its refX and refY, with the keywords of a marker's, name the point of its
content, in its viewBox's coordinates, that is placed there (SVG2/struct.html#SymbolAttributes). Without them, the
viewport's edge is placed there. The symbol's transform applies to the instance, since SVG 2 places the reference point
by "any transformations on the symbol" as well as its x and y. An instance of a symbol whose display computes to none
draws nothing, though SVG2/struct.html#SymbolNotes would display it inline: not drawing what an author hid is the safer
failure.

A use element hosts a closed shadow root that holds instances of what it references (SVG2/struct.html#UseShadowTree),
which the DOM keeps (`dom_use.h`), so that the presentation styles them as any element: they inherit from the use, match
the rules of the use's tree within their own, and run their own animations and transitions. A mutation of the referenced
subtree reaches the instances before the next restyle. An instance that still matches its original keeps its identity
and takes the original's attributes and data, so a change to the original is a change to the instance, which a
transition follows. A reference that makes a cycle instances nothing, and one use instances at most `kMaxInstances`
elements, counting the uses in its shadow tree.

A use element's x and y are a translation after its transform (SVG2/struct.html#UseLayout), so its clip path, mask and
filter apply in the space that the translation makes, and its opacity and blending outside it. The use builds two
nodes: its own, with its transform, opacity and blending, and under it the translation, with the clip path, mask and
filter. Its getBBox includes the translation and its getCTM leaves it out, which is what content on the web measures,
where SVG 2, which makes the translation part of the use's transform, would have them the other way round.

A tree is updated in place. Each element that renders keeps its node, in a slot that it holds until the node is freed,
and a node built again takes its old place among its siblings. Changes are noted as the presentation reports them
(`svgTreeNoteChange`). Noting reads the tree without changing it, so it may happen while another thread paints. The
next update (`svgTreeUpdate`) applies the work in order: removals, servers, then nodes from the root down. A g, an a or
an svg element reads its own values again and keeps its children. A change of children builds those that lack a node
and frees those that left. Any other node is built again whole.

Each href and URL that a build follows is recorded with the ID, the element that the ID named, and the node or server
that read it. A change at or under a referenced element builds again what read its subtree, and a change to what an ID
names builds again what named it. A gradient in one svg element can paint a shape in another, so every tree hears of
every change, and a filter of hashes skips the elements that no reference reads. A server is freed once twice as many
servers live as the last sweep kept and no paint references it.

`prCssPresentationVerify` compares each tree with a fresh build (`svgTreeSame`), node by node.

Text is laid out by `svg_text.h`, and SVG fonts come from SVG 1.1's font elements (`svg_font.h`). They belong to the
document, not to a build: the document's changes keep its faces in tree order, and a font is read when text first needs
it, until a change under it. A tree records the families that its text looked up, found or not, and a change to a family
builds again only the trees that read it. A font in another document that a `font-face-uri` names is kept by that
document, and a build that finds no font remembers that only for itself, since another build's host may load it.

## Geometry and vector effects

- **min-width, max-width, min-height and max-height** limit the used size of a rect, an image and a foreignObject
  (SVG2/geometry.html#Sizing), the minimum winning, with percentages of the viewport. An image's auto dimension is
  sized from the limited other one, and limited in turn.
- **vector-effect** lists any of its keywords together, since SVG2/coords.html#VectorEffects says that "these values
  can be enumerated", though the property's grammar line has one. non-scaling-size, non-rotation and fixed-position
  constrain the transform from an element's user space to the host, as SVG2/coords.html#VectorEffectsCalculation's
  formulas do. The host is the viewport coordinate system of the outermost svg element, the space that the tree is
  drawn in, which for an SVG image or a pattern's tile is the image's or the tile's. A fixed-position element's
  transform gives the origin of its user space in the host by its translation, and its linear part stays in the CTM.
  non-scaling-stroke changes only the stroke. Drawing, hit testing, getCTM and bounding boxes read the same
  constrained transform.

## References into other documents

A reference to another document (SVG2/linking.html#processingURL) names an element of a resource document that the
referencing document keeps (`dom_resource.h`): fetched once by its URL without a fragment, parsed in secure static mode,
and never changed. A document with a browsing context fetches through its host from its own origin only. SVG 2 asks
for a CORS request, which the host's fetch cannot make, so a reference to another origin is a network error, as a
failed CORS check gives. A document without a browsing context, as a harness draws, takes documents from the loader of
its draw options. Once one loads, the uses resolve their hrefs again and the presentation rebuilds what read the
references.

- **A use** instances the element that the fragment names, or the root without a fragment. The instance is a clone in
  the use's document, styled there as any instance is.
- **Paint servers, markers, clipPaths, masks and filters** in another document are styled by a cascade of that
  document (`svg_cascade.h`), which the tree keeps, from that document's own sheets.
- **A mask-image URL** of another document is an image, with a fragment or without one: until the document loads, a
  mask element cannot be told from a view of an SVG image. clip-path, filter and the paint properties follow the URL
  into the other document.
- An element inside a resource document references nothing outside it.

xml:base changes no URL: a reference resolves against the document's base URL, as the DOM (dom/#dom-node-baseuri) and
HTML (html/#document-base-url) define it. Neither has xml:base, though SVG2/linking.html#processingURL-absolute still
names XML Base.

## Text

- **White space** is CSS's (SVG2/text.html#TextWhiteSpace): each element's characters are processed by its computed
  `white-space-collapse`, and xml:space reaches text only through the user agent sheet. A preserved segment break is
  a forced line break. Soft hyphens and bidi controls are not addressable characters.
- **Lines.** Text with a forced line break or an `inline-size` is set as one line, broken into lines, and set again.
  Line boxes stack by CSS 2's rules, with the text element's font as a strut. Wrapped text ignores every positioning
  attribute but its first character's x and y, and each line is anchored on its own.
- **Breaking words.** Wrapped text breaks at lib/text's tailored opportunities (`prTextBreaksTailor`), as HTML text
  does. overflow-wrap anywhere or break-word, and word-break break-word, break a word that fits no line before the
  grapheme cluster that overflows. word-break break-all lets any two letters break, and keep-all keeps East Asian
  words whole. overflow-wrap applies only to auto-wrapped text, among which SVG2/text.html#TextProperties lists it.
- **Shaping** takes each element's font features, capital and position forms, kerning and synthesis from its computed
  font properties (`cssFontFeatureSet`), the same reading that any producer of text makes.
- **text-overflow** applies to text with a wrapping area and no textPath. clip draws the text inside the area's inline
  extent. ellipsis ends each overflowing line with an ellipsis in the text element's style, in place of the glyphs
  that leave it no room. SVG 2 says neither which style the ellipsis takes nor which edge it replaces glyphs at, so
  the engine takes css-overflow-3's: the block's style, at the end edge. w3c/svgwg#1115 asks whether to keep the
  property in SVG. The SVGTextContentElement methods and the bounds see the text without it.
- **`::first-line` and `::first-letter`** of a text element style its first line and its first letter
  (SVG2/text.html#TextProperties). A part of each element's characters takes the pseudo-element's style. On the first
  line, each element's part inherits from its parent's, under the line's own (css-pseudo-4/#first-line-inheritance).
  The first letter's part (`prTextFirstLetter`) inherits from the element that holds it. Text that is not broken into
  lines is all on its first line, since it is set as one line. Wrapped text finds its first line with the first line's
  styles, and its other lines with their own. A first letter split between elements is not styled, which
  css-pseudo-4/#first-letter-tree allows. Only a text element originates them, as SVG 2 requires them "on 'text'
  elements" (SVG2/styling.html#RequiredCSSFeatures): a tspan is not a block container. `::before` and `::after`
  generate nothing in SVG text.
- **Vertical text** is laid out as horizontal text in a frame turned a quarter turn, and what it records is turned
  back onto the page. text-orientation sets each character upright or on its side, by lib/text's itemization.
- **textPath** follows a path or the equivalent path of any basic shape. On a single closed subpath its text goes
  round once, from where text-anchor puts it about startOffset. An href to anything else is an error, and its text is
  not drawn.
- **SVGTextContentElement** measures the characters as laid out. Each addressable character keeps its position on its
  line, without the shift that aligns its run's baseline, its advance and its cell. A query of an element in a
  presentation brings the presentation up to date first.

## Paths

lib/path reads path data (`pr/path_data.h`) into commands as they are written, with implicit commands made explicit,
and builds a path from commands.

- **d takes path()**, and the attribute takes path data, whose hint is the path() of its string (w3c/svgwg#320).
- **A path() holds its commands as values** (`css_path.h`), read from its string once, when the value is matched: each
  command's letter, then its arguments as numbers, up to the first error. The value serializes in one form however it
  was written, a token stream writes it as its string, and SVG builds its path from the numbers. d computes the
  commands in absolute coordinates.
- **Interpolation** needs the same number and types of commands, in the same order (SVG2/paths.html#PathDataAnimation),
  and is discrete otherwise. A relative command and an absolute one, such as l and L, are of one type, which the
  computed value's absolute coordinates make the same value. Paths of the same commands are values of the same shape,
  so they interpolate and add number by number, as other values do. Path data has no range, so the property's does
  not clamp it. A flag interpolates as a fraction and is set if it is above zero.
- **Segments of zero length.** A subpath that only closes, and an arc that ends where it starts, are segments of zero
  length, which paint their caps (SVG2/paths.html#ZeroLengthSegments). SVG2/paths.html#ArcOutOfRangeParameters says
  such an arc is omitted, which would leave a lone moveto that paints nothing. The caps of a subpath of zero length
  face the positive x-axis of user space, which lib/draw maps into the space that it strokes in
  (`PrPathStroke::zeroLengthDirection`).
- **path-length** gives the author's length of a path or a basic shape. Its presentation attribute is pathLength. A
  stroke's dashes and dash offset scale by the length of the geometry over it (SVG2/paths.html#PathLengthAttribute), and
  a path-length of zero takes every dash and offset that is not zero past the end of the geometry.

## Painting

- **paint-order** orders a shape's fill, stroke and markers, and text's fill and stroke. Its keywords keep the order
  they are written in, which is what they mean.
- **Context paint** (SVG2/painting.html#context-paint) is the fill or stroke of the context element: the use element
  whose shadow tree the element is in, or the shape that a marker is drawn on. Drawing carries the context element's
  paints, with the user space that a paint server of theirs draws in and the bounding box that it maps onto. A use
  element's bounding box is that of its content, which is measured only for a paint server. Context paint without a
  context element paints nothing.
- **Markers** are resources, as gradients are. A marker's content is built once, from the styles of the marker's own
  tree, and is drawn at each vertex that references it. The vertices are those of each shape's equivalent path,
  including a rect's, a circle's and an ellipse's (SVG2/painting.html#VertexMarkerProperties). Their directions follow
  the path's directionality (SVG2/paths.html#PathDirectionality, `prPathDataVertices`). A closed subpath's first and
  last vertices take the direction half way between the closing segment and the first (w3c/svgwg#300). refX and refY
  take the keywords that w3c/svgwg#64 added. A marker does not draw inside its own content, and markers nest at most
  `kSvgMaxMarkerDepth` deep. A marker on a shape with non-scaling-stroke is sized by the stroke's width in the space
  that the tree is drawn in. A shape that is not visible draws no markers.
- **Paint servers** are resources: gradients and patterns, built once per element with what they take from their
  templates through href (SVG2/pservers.html#PaintServerTemplates). Each attribute comes from the first server of the
  chain that has it. The transform is the first that the transform property gives, of which gradientTransform and
  patternTransform are presentation attributes, so an attribute that does not parse leaves the transform to the
  templates. A template's children are the server's shadow tree: a gradient's stops and a pattern's content inherit
  from the server that the paint references, not from the template, nor from what the server paints.
- **Fallbacks.** A server in objectBoundingBox units on geometry whose bounding box has no width or height is ignored
  (SVG2/coords.html#ObjectBoundingBoxUnits). A server whose transform cannot be inverted displays nothing
  (css-transforms-1/#transform-function-lists). Both paint the paint's fallback, as a reference in error does.
- **A pattern** is drawn into a list of its own, its tile, which an image brush repeats. A backend renders the tile at
  the scale it is shown, so the content stays sharp under transforms. Shapes painted by one pattern in the same space
  share its tile. The content comes from the first pattern of the chain whose children are not all descriptive
  elements. The pattern element's own opacity applies to nothing, since the pattern is never rendered.
- **A pattern's tile** is a rectangle in user space, or in fractions of the bounding box, which the pattern's transform
  then maps in user space. SVG2/pservers.html#PatternElement applies the transform inside the bounding box's units,
  which would shear the tiles of a box that is not square while the content in user units inside them stays square.
  The content is clipped to the tile whatever its overflow: SVG2/pservers.html#PatternNotes leaves visible overflow
  undefined, and drawing it would need every neighboring tile's content in each tile. The content's percentages are
  of the viewport that the painted element is in, and it is nobody's context element. A pattern inside its own tile is
  a reference in error, and patterns nest at most `kSvgMaxPatternDepth` deep.
- **Gradient stops** mix with their color and alpha apart (SVG2/pservers.html#StopColorProperty), so a stop toward
  `transparent` mixes toward black.
- **A radial gradient whose start circle holds its end circle** paints nothing (SVG2/pservers.html#RadialGradientNotes),
  not even the fallback, where Canvas's gradient would paint the cone that both circles touch. SVG 2 says so plainly,
  and the cone covers what an author placed the circles to leave bare.
- **color-interpolation** linearRGB mixes a gradient's stops in linear light, by the value on the gradient that the
  paint references. On an element that paints, and on a group with opacity, it composites the element onto what lies
  beneath in linear light (SVG2/painting.html#ColorInterpolation), which draws the element in a group that reads its
  backdrop. Content that does not set linearRGB pays nothing for it.
- **Animation.** An element of SVG content has no box, so a compositor runs none of its animations. Its opacity and
  transform animate in its computed style, as its other properties do. An element of a use element's shadow tree is
  SVG content by its parent in the flat tree, the use, though its parent in its own tree is the shadow root.

## Clipping and masking

An element's effects apply to its drawing in the order of filter, clip path, mask and opacity
(css-masking-1/#placement): the clip path around the group whose filter, mask and opacity apply to the content. A node
keeps its computed compositing group, from which drawing reads the basic shape, the reference boxes and how mask layers
are placed and combined, with the clipPaths, masks and filters that its URLs name resolved. An outermost svg element's
clip path, mask, filter and blending are its box's, and the tree leaves them out, as it leaves out the box's opacity. A
node that establishes a viewport applies its effects in its content's user space, as its bounding box is.

- **Reference boxes.** fill-box is the object bounding box, and stroke-box the bounding box of the geometry and its
  stroke shape (SVG2/coords.html#TermStrokeBoundingBox), which SVG 2 defines in place of css-masking-1's appendix B. A
  container's boxes are the union of its children's. Without a CSS box, content-box and padding-box are fill-box, and
  border-box and margin-box are stroke-box; mask-origin's border-box is fill-box. view-box is the nearest viewport.
- **clip-path** clips to a basic shape in its reference box, stroke-box by default, with the shape's own fill rule, or
  to the box alone, or to a clipPath. A URL that names no clipPath clips nothing (css-masking-1/#the-clip-path).
- **A clipPath** is the union of its children's silhouettes (css-masking-1/#ClipPathElement): shapes, text, and use
  elements of one of them directly, each filled by its clip-rule, which inherits from the clipPath's ancestors. A child
  that is hidden or displays none adds nothing, and an empty clipPath clips everything away. A use child that
  references anything else adds nothing. css-masking-1 would ignore the whole clipPath instead, and an issue in the
  specification questions that rule. A child's own clip-path clips its silhouette, and the clipPath's clips the union.
  Fill, stroke, opacity and masks do not apply. The content is in user space or in the bounding box's units, which the
  clipPath's transform then maps into user space. A clipPath of one shape is a clip of the draw list. Any other draws
  its silhouettes into a list whose coverage masks a group. Its nodes keep their geometry, for a hit test to read.
- **A mask** draws its content, clipped to its region, into a list whose luminance or alpha masks the group
  (css-masking-1/#MaskElement). The region defaults to -10% and 120% of the bounding box in each axis whether or not
  any of x, y, width and height is given, as SVG 1.1 has it, though css-masking-1 establishes a region only if one is
  given. A width or height of zero hides the element. Its opacity and display do not apply, and luminance is in
  sRGB-linear if its color-interpolation is linearRGB. A mask is a container element, so its own clip-path and mask
  apply to its content.
- **Mask layers** are each a mask element, or an image or a gradient placed by mask-size, mask-position and
  mask-repeat in the box of mask-origin and painted within the box of mask-clip. mask-mode picks luminance or alpha,
  and the layers composite from the bottom up by mask-composite (css-masking-1/#layering). none is a transparent layer.
- **A mask layer in error masks nothing.** A layer that references no mask element, or whose image cannot be displayed
  or has not loaded, is ignored, for SVG content and HTML boxes alike. css-masking-1/#the-mask-image counts such a
  layer as transparent black, which would mask the element away. The same specification ignores a mask border image
  that cannot be displayed (css-masking-1/#the-mask-border-source), and the engine applies that rule to layers too.
- **References in error.** A clipPath or a mask that applies inside its own content ignores the reference, and they
  nest at most `kSvgMaxEffectDepth` deep. In objectBoundingBox units on a bounding box of no area, a clipPath and a
  mask hide the element.

## Filters and blending

- **A filter list** applies its items in order, each reading the result of the one before it
  (filter-effects-1/#FilterProperty). A filter function becomes nodes of lib/draw's filter graph that compute in sRGB
  (`prLayoutFilterAppend`, which HTML boxes share). A filter element becomes the nodes of its primitives
  (`svg_filter.h`), where it reads the item before it as SourceGraphic. A URL that names no filter element ignores the
  whole list.
- **A filter element** is a resource, which takes its attributes and primitives from the filters that it references
  through href, as patterns do. filter-effects-1 dropped the attribute that SVG 1.1 had
  (SVG11/filters.html#FilterElementHrefAttribute), and the engine keeps it, since it costs only the chain that patterns
  follow. Its properties inherit from its own ancestors, and it applies inside an element that displays none
  (filter-effects-1/#FilterElement). Its region defaults to -10% and 120% of the bounding box, and clips each
  primitive's inputs and results. A primitive's subregion defaults to the union of its inputs' subregions, or to the
  region for a source or feTile, and one of no area is transparent black. An `in` that names no preceding result is
  the implicit input.
- **A filter that hides.** A filter element without primitives (filter-effects-1/#FilterPrimitiveTree), one whose
  region has no area or is in the units of a bounding box of no area, and one that applies inside its own feImage
  content hide the element.
- **Sources.** SourceGraphic and SourceAlpha are the filter's input within its region. BackgroundImage and
  BackgroundAlpha are what lies beneath in the nearest isolated group, as filter-effects-1/#AccessBackgroundImage
  defines them without enable-background. FillPaint and StrokePaint are the element's fill and stroke over the whole
  region.
- **Primitives** compute in the space of their color-interpolation-filters. auto is sRGB, which
  filter-effects-1/#ColorInterpolationFiltersProperty lets the user agent choose. feImage draws an image as an image
  element fits it in its subregion, or an element as a use element draws it, moved by the subregion's origin: the
  subregion bounds every primitive, and a use element moves what it references by its x and y
  (filter-effects-1/#feImageElement). feDisplacementMap reads its map in its space and moves its input unconverted. A
  filtered element draws no further than the region of a filter element that ends its list.
- **Primitives in error.** A negative standard deviation is zero on its axis, and a negative morphology radius passes
  the input through. feColorMatrix with values of the wrong length passes its input through. feConvolveMatrix is
  transparent black with an order that is not positive, a target outside its kernel, or a kernel of the wrong length,
  where filter-effects-1/#feConvolveMatrixElement says to pass the input through and has an open issue on that
  (w3c/fxtf-drafts#237). A divisor of zero takes the default. Lighting without a light, feSpecularLighting with an
  exponent outside [1, 128] (SVG11/filters.html#feSpecularLightingSpecularExponentAttribute), and feTurbulence with a
  negative base frequency are transparent black. A spot light's negative specularExponent is its initial value, 1, and
  a limitingConeAngle of zero lights nothing.
- **Tainting** (filter-effects-1/#tainted-filter-primitives). Every source is tainted, as are a flood or lighting color
  that reads currentcolor, an feImage of an element, and a primitive that reads a tainted input. feDisplacementMap
  with a tainted map passes its input through (filter-effects-1/#fedisplacemnentmap-restrictions).
- **mix-blend-mode and isolation** make a group of the element, which blends with what lies beneath up to the nearest
  isolated group (compositing-1/#csscompositingrules_SVG). Opacity, a filter, a clip path, a mask, blending, an inner
  svg element that clips, a 3D transform and isolation: isolate isolate a group. A 3D transform is a transform property
  that lists a 3D transform function (css-transforms-2/#3d-transformed-element), even one whose matrix is 2D, such as
  translateZ(0): compositing-1 isolates "3D transforms" without exception. A use element forms a non-isolated group.
  The outermost svg element is an isolated group (SVG2/render.html#ParentCompositing), which the tree draws as one only
  if something in it blends or reads its backdrop.

## Presenting SVG

- **An outermost svg element** is a replaced element (SVG2/render.html#TermReplacedElement), whose content box is its
  viewport. The presentation keeps a record for each such element with a box (`css_presentation_svg.h`), and lib/svg
  fills it: a tree, the element's natural size, and the layout roots of its foreignObjects. The browsing context runs
  lib/svg's step between the presentation's boxes and its layout (`svgPresentationUpdate`), for each frame, each job
  and each synchronous read (`DomDocumentHost::updatePresentation`). The box's painter draws the tree at the extent
  that layout gives. The presentation never calls lib/svg to update anything. It notes which SVG elements' styles
  changed and which foreignObjects gained or lost a box, and lib/svg follows the document's tree itself once it draws
  anything.
- **Resources of HTML boxes.** A box's clip-path, mask layers and filter name clipPaths, masks and filters by URL
  (css-masking-1, filter-effects-1). lib/svg resolves them for the presentation (`CssPresentationReferences`) from its
  references: a tree that holds the resources the boxes name and no node, built from the presentation's styles and
  brought up to date by the changes that every tree hears. The projection takes each named resource as a layout source,
  whose functions draw it, append its filter nodes or hit test it for the box. The box's border box is the resource's
  user space, the viewport of its percentages and its object bounding box, and a filter's bounding box also takes in the
  box's overflow. A source is counted by the groups that hold it, and the next update after none does frees it with its
  resource. Each rebuild of the resources gives the sources a new version, so the boxes that named one are projected
  again and their groups change. A URL that names no element of the kind clips or masks nothing, and a filter list with
  one filters nothing, though the box still makes a stacking context and a containing block.
- **Natural size** comes from the width and height attributes and the viewBox (`prSvgElementNaturalSize`), even if CSS
  sets those properties to auto, so that content which resets them keeps the size that its markup gives
  (`svg/layout/svg-intrinsic-size-min-size.html`). SVG2/coords.html#SizingSVGInCSS takes the properties instead, which
  would make such an svg fill its container. The ratio is that of the width and height if both are absolute, and of
  the viewBox otherwise.
- **An SVG image** (`PrSvgImage`) is built once with its own cascade, detached from its document so that it can be
  drawn and freed on any thread. Its painter draws it at the size it is shown: its viewBox and preserveAspectRatio
  place the content in that size, and without a viewBox, its width and height scale to it. Before it is shown, its
  text and viewport units resolve against its natural size, or else against the default object size of 300 by 150.
  An SVG image loads no external resource, only the images of data: URLs.
- **Views.** The fragment of the URL that names an SVG image may name a view (SVG2/linking.html#SVGFragmentIdentifiers):
  svgView(), a media fragment's xywh, clamped to the image, or a view element. The element that the fragment names is
  the document's target while the image is built. svgView()'s transform() moves the view about its center after its
  viewBox, as a transform property of the root would with the user agent sheet's transform-origin, since SVG 2 does not
  support transform-origin in a view.

## Embedded content

- **An image element** shows a raster image or an SVG image (SVG2/embedded.html#ImageElement). It is embedded content
  that its document's browsing context loads (`domEmbeds`), as an HTML img is, and the element's node is built again
  once it has loaded. An feImage that names an image loads the same way. A document that has no presentation takes it
  from the host's loader (`PrSvgDrawOptions::loadImage`). object-fit and object-position size
  and place the image in its positioning rectangle. preserveAspectRatio then fits the image's ratio into that, unless
  object-fit kept the ratio already or the image has none. An SVG image takes the result as its viewport, ignores its
  root's preserveAspectRatio, and takes a width and a height without a viewBox as its viewBox (w3c/svgwg#249). The
  image's overflow clips it to the positioning rectangle, and an SVG root's overflow clips its content to the viewport.
- **A foreignObject's content** is laid out by the presentation's layout tree, as a root of its own in a containing
  block of the positioning rectangle's size in user units, which the build resolves in the nearest viewport. The
  foreignObject's box fills that block, and its margins, position, transform and effects apply to nothing, since SVG
  places and draws it. The SVG tree draws the root's drawing at its place, under the transforms, clips and opacity
  around it. The presentation brings an svg element's tree up to date once all the boxes of an update are made, so that
  the tree finds its foreignObjects' roots. Layers in the content are drawn flat, without the compositor. Hit testing
  reaches the content through the foreignObject's place, and the client rects of its boxes map from the root's
  containing block onto the page through it. An svg inside a foreignObject is outermost. A foreignObject's overflow is
  hidden unless set, as SVG 2's table of overflow gives it (SVG2/render.html#OverflowAndClipProperties), though its user
  agent sheet leaves it visible.
- **requiredExtensions** passes for the namespaces of HTML and MathML (w3c/svgwg#138), and an empty list fails.
- **An image element fires load or error** once what its href names has loaded or failed to
  (SVG2/interact.html#SVGEvents), once for each URL that it names in turn. The load starts when the element is connected
  or its href changes, whether or not the element renders. SVG 2 dispatches the event once the resource has loaded, so
  an element that displays none still fires it. An feImage fires neither event.

## Interaction (`svg_query.h`)

- **Hit testing** reads the SVG tree, as drawing does (SVG2/interact.html#PointerEventsProp). The presentation hit tests
  its layout tree, and for the box of an outermost svg element, asks the content for what lies under the point in its
  content box (`CssPresentationSvgContentFunctions::hitTest`). What it finds stands before the svg element, and the svg
  element is hit where none of its content is. A foreignObject's content is a layout root of its own, which the
  presentation hit tests in turn (`prLayoutNodeHitTest`), before the foreignObject.
- **pointer-events** decides what a node is hit over: its fill, by its fill-rule, or its stroke, with the stroke's
  width, joins, caps, dashes and vector-effect, whether the fill or stroke paints, and whether the node is visible.
  The stroke shape includes the dashes (SVG2/painting.html#StrokeShape), so drawing and hit testing share one shape.
  A paint that names no server and has no fallback paints nothing, and context paint paints what its context element's
  paint does. bounding-box hits a node over its object bounding box, and a container over its own where none of its
  content is hit. Text is hit by its characters' cells, by the pointer-events, visibility and paints of the innermost
  element that holds the character. An image and a foreignObject are hit by their positioning rectangles. A clip path
  clips where content is hit; a mask, a filter, opacity and markers change nothing (css-masking-1/#clipping-paths). An
  svg element whose box takes no pointer is not hit itself, but its content may be, as the descendants of a CSS box may.
- **Targets.** An element of a use element's shadow tree is hit as itself, and its events retarget to the use as the
  DOM retargets them. An instance also listens with its original's listeners, at the same phase
  (SVG2/struct.html#UseEventHandling). Its event handlers are its own, from the attributes that it copies. An SVG
  element's event handler names its event evt as well as event (SVG2/interact.html#EventAttributes).
- **Links.** An a element with an href or an XLink href is a hyperlink (SVG2/linking.html#AElement): it matches :link,
  is focusable, and a click follows it. An a element inside another hyperlink, of any namespace, is an inactive
  placeholder, as SVG 2 says. HTML's parser never nests a elements, and the inner link could not be followed apart from
  the outer. A change to an href restyles the links inside its element.
- **autofocus** on an SVG element focuses it at the first rendering update after it is inserted, if it is the first
  focusable element with the attribute (html/#flush-autofocus-candidates), as on an HTML element.
- **Scripts.** A script element in SVG content runs as HTML prepares a script, with its href in place of src. The HTML
  parser runs one as it closes it, at its end tag or as it closes itself (html/#parsing-main-inforeign).

## Geometry queries

The queries of SVGGraphicsElement and SVGGeometryElement measure the tree that draws the element: the content of its
outermost svg element, brought up to date for its box's extent, or for a document that has no presentation, a tree built
for the query at the svg element's natural size. A tspan or a textPath is measured in its text's user space.

- **getBBox** gives the object bounding box, with the stroke bounding box if asked, cut by the bounds of the element's
  clip path if asked, and with the markers' if asked, their content's stroke bounding boxes placed at each vertex
  (SVG2/coords.html#BoundingBoxes). An element without a clip path is not cut. A container's box holds its children's,
  including those without area, such as a horizontal line's, and those that are hidden.
- **getCTM** maps the element's user space to the coordinates that its nearest viewport is placed in, through that
  viewport's own placement. An outermost svg element's maps to its viewBox's. The engine reads
  SVG2/types.html#__svg__SVGGraphicsElement__getCTM's "SVG viewport's coordinate system" as the system that the viewport
  is placed in. **getScreenCTM** maps on through the svg element's border and padding, its box and every transform
  around it to the viewport, as the affine transform that maps the user space's origin and unit vectors where they are
  drawn.
- **Client rects.** An element of SVG content has one client rect: its object bounding box mapped through every
  transform as it is drawn, as cssom-view-1/#dom-element-getclientrects names "the bounding box of the element as
  defined by the SVG specification".
- **Interfaces.** Each shape has its own interface, SVGRectElement through SVGPolygonElement, under
  SVGGeometryElement, with the lengths that it reflects.
- **The SVG DOM's lengths** (`pr/svg_length.h`) reflect the length attributes of each element's interface. A missing or
  invalid attribute reflects its initial value. A length resolves percentages against its element's viewport along the
  attribute's direction and font-relative units against its element's font. A length in a unit that SVGLength has no
  constant for, such as Q, is of the unknown type, as SVG 2 says. With no animation running, an animated value is the
  base value, read only.

Suites: `svg-wpt`, the SVG render corpora ([testing.md](testing.md)), and `suites/prax`'s SVG 2 chapters. The
invalidation check over real pages checks every tree against a fresh build after each change.

# CSS

CSS lives in `lib/css`.

## Pipeline

1. Tokens (`css_syntax.h`): each has a kind and a source span. Escapes are resolved when a value is read.
2. Rules and declarations (`css_syntax_rule.h`): the css-syntax-3 consume algorithms, with no grammar.
3. Matching (`css_parse.h`, `css_match.h`, `css_parse_terminal.h`): one matcher runs every grammar.
4. Parsed data (`css_rule.h`): reference-counted rules, preludes, selectors and declaration blocks.
5. Objects (`css_object.h`, `css.cpp`): CSSOM objects, created on first access, with `pr/css.h` over them.
6. Serialization (`css_serialize.h`, `css_rule.cpp`, `css_selector.cpp`, `css_shorthand.cpp`): every CSSOM getter.

`css_root.h` holds the CSS state of a document or shadow root, and of an element. `css_owner.h` gives a tree its sheets
from their owners, `css_inline_style.h` holds an element's inline style, and the presentation fills computed styles
(`dom_computed_style.h`).

## Grammars

`tools/gen/gen_css.py` generates every grammar from webref into one flat node array (`css_grammar.h`). Anything that
webref cannot express is a table in the generator, with its reason. To change what CSS accepts, change the tables or
the webref pin and run `./dev.sh gen`. Rules in prose that no table can express go in a terminal parser
(`css_parse_terminal.cpp`) or a match check (`css_match.cpp`).

## Values (`css_value.h`)

A value is a preorder tree of 16-byte `CssValueNode`s. `next` is a relative distance, so subtrees move without fixups.
A value is built in a `CssValuePool` and stored in a `CssValueList`, which is one allocation of nodes plus text.

- Property values are flattened: they hold matched keywords, numbers, functions and literals, and no production nodes.
- Selectors, preludes and queries are structural: every literal is kept, and a `PrCssValueKind_Production` node is kept
  if the production carries meaning (`kCssValueDefKeepsNode`).
- Values are canonical if cssom-1 defines a canonical form: implied components are omitted (`CssGrammarOmission`),
  calc is simplified (`css_math.h`), and colors are stored as channels (`css_color_value.h`).
- Custom properties and values with substitution functions are `PrCssValueKind_Tokens`.

`PrCssValue` is the public form. The Typed OM is in the binding (`js_css_typed_om.cpp`), and converts to and from
`PrCssValue`.

## Data and objects

- Data (`CssSheetData`, `CssRuleData`) takes one allocation for each rule's contents. A block's declarations, values
  and text are one allocation. Style rules hold compiled selectors, and preludes are serialized from the compiled
  form.
- Data names only what it holds (no rule names its parent) and is shared by reference count. A document caches the
  data of recent style elements by their text (`CssRoot::cache`).
- Changes are staged ("Edits" in `css_rule.h`) and committed copy-on-write along the path to the change
  (`cssRuleWritable`). A changed declaration block becomes mutable, with one allocation per declaration.
- Objects (`CssStyleSheet`, `CssRule`) are created on first access, reference their data, and live as long as their
  tree. Sub-objects (the style declaration, media list and rule list) are members, and their handles are member
  addresses.
- A parse runs on a `CssParser` (`css_parser.h`) that the sheet owns. The parser is created on first use, and released
  after a source larger than `kVmWorthwhile`. A parse with no sheet uses a temporary parser.

## Shorthands (`css_shorthand.h`)

Blocks hold longhands only. A shorthand is expanded when it is declared, through the components of its grammar that
set longhands, and is read back through the same grammar. A shorthand whose longhands share one value (a CSS-wide
keyword, or a pending substitution) is stored once, as a shared declaration.

## Selectors (`css_selector.h`)

A selector list compiles to records in one allocation. Simple lists compile directly from tokens, and others compile
through the matcher. Debug builds check that the two paths agree. Matching (`css_selector_match.h`) runs right to left,
and `CssSelectorMatcher` caches across one query. Pseudo-classes read element state, never attributes. Selectors are
serialized from their records everywhere they appear.

A sheet's default namespace restricts every compound, not only its type selectors: a compound without a type or
universal selector compiles with an implied universal selector in that namespace, which serialization leaves out
(selectors-4/#type-nmsp). The subject compound of a selector in the argument of `:is()`, `:where()` or `:not()` is not
restricted (selectors-4/#matches). So the user agent's HTML sheet, whose default namespace is HTML's, matches no SVG or
MathML element with `:link` or `[hidden]`. A namespace that the specifications name matches by its ID, without reading
a URL.

A selector matches in a tree's context (css-shadow-1/#selectors-data-model): a combinator walks no further than its
tree's root. In a shadow tree's context, the host is the parent of the tree's top elements, and only `:host`, `:host()`
and `:host-context()` match it (`CssSelectorMatcher::host`). `::slotted()` matches the elements assigned to a slot,
through the slots that are themselves assigned. A query or `matches()` on a node of a shadow tree matches in that
tree's context.

## The cascade (`css_cascade.h`)

A rule set files each complex selector of a document's style rules under the most selective feature of its subject
compound (an ID, a class, an attribute or a type), or with the universal entries. An element matches the entries filed
under its own features and the universal entries. An entry also keeps up to four features that the subject's ancestors
must carry: IDs, classes and attribute names first, then types. They come from the compounds that the selector reaches
through descendant and child combinators. A Bloom filter of the features of an element's ancestors rejects, without
walking the ancestors, the entries whose ancestor requirements cannot match. The cascade sorts the matches by origin,
importance, layer, specificity and order, and the last declaration of each longhand wins. A `revert` rolls a longhand
back to the declarations of the origin before its own. A `revert-layer` rolls it back to the declarations of the layer
before its own.

An `@import` files the imported sheet's rules in the layer that its `layer` or `layer()` names
(css-cascade-5/#at-import). An import whose `supports()` condition fails applies nothing, though its sheet is still
fetched.

A selector of a pseudo-element whose boxes the presentation generates (`::before`, `::after`, `::marker`, and the marker
of a `::before` or `::after`), or of `::first-line` or `::first-letter`, is filed among the entries of that
pseudo-element, under the features of its originating element. A pseudo-element's style is matched against the
originating element, with the selector's pseudo-elements matching the pseudo-element. The style inherits from the
element or pseudo-element that originates it. The features of a pseudo-element's selectors restyle the originating
element, as the features of the element's own selectors do. Every other pseudo-element's selector is dropped.

The document's rule set files the sheets of every tree, the document's and each shadow root's, after the user agent's
rules. Each entry keeps the root of the tree whose sheet holds it (`CssRuleEntry::scope`), and an element matches the
user agent's entries, its own tree's entries, the `:host` entries of the shadow tree that it hosts, and the
`::slotted()` entries of the trees whose slots it is assigned to. One set keeps one index of invalidations and one set
of matchers and caches for every tree. A tree's depth, its number of shadow-including ancestor shadow roots, orders the
encapsulation contexts before layers and specificity (css-cascade-5/#cascade-context): a normal declaration of an outer
tree wins over an inner one, and an important declaration of an inner tree wins. A set tests scopes only if its document
had a shadow root when the set was built (`CssRuleSet::scoped`), and a document's first shadow root builds the set
again.

A rule set also keeps an invalidation for each feature that its selectors read (a class, an ID, an attribute or a
state). The invalidation says what a change to that feature of an element restyles: the element, its siblings, its
whole subtree, or the descendants that carry one of the subjects' features. A feature read inside `:has()` also names
the anchors that a change to it can make match. These are the ancestors, or, for an argument that reaches siblings,
the earlier siblings, that carry the feature of the compound holding `:has()`.

The rule set records what no invalidation covers, the structural pseudo-classes and sibling combinators, as
`CssRuleReads`. Matching records these reads on the parent whose children they read, and only if the structure decides
the match. A compound tests what reads structure only after the compounds to its left match, and a sibling combinator
counts only if the compounds beyond the siblings match. Each kind of read names what adding or removing a child
restyles: the parent, the element immediately after or before the change, or every element after or before it. A read
made for one of the child's descendants or later siblings also restyles their subtrees.

## Computed values (`css_style.h`)

A style holds one group of each kind. Each group holds the computed values of its properties as value runs in one
allocation, and groups are interned so that elements styled alike share them. Styles and groups are immutable and
counted atomically. Each thread computes in its own builder, and the builders intern into one locked table. A child
shares an inherited group with its parent if it declares none of the group's properties. A child that declares none of
the properties of a group that does not inherit takes the table's initial group of that kind, without building it. A
table's initial groups take their values from initial values that are computed once per process. Lengths compute to px,
and named colors to sRGB. `color-hdr()` stays as specified, since it resolves for the display only where the style is
projected (html.md, Media queries).

Which group holds each property is the engine's choice, which gen_css.py keeps beside the property data
(`STYLE_GROUPS`, emitted as `css_style_group.h`). Every longhand that webref lists is in a group, is flow-relative and
computes as its physical counterpart, or is on the list of the longhands that the engine parses but does not compute
yet (`NOT_COMPUTED`), whose declarations a style drops. The generator fails on a longhand that is none of these, and on
a group that holds a property whose inheritance is not the group's.

Viewport-percentage lengths are relative to the extent that the presentation is updated in, the initial containing
block (css-values-4/#viewport-relative-lengths): the viewport on the screen, and the first page's area in print
(layout.md, Paged media).

Font-relative lengths are relative to the element's font, and their root forms to the root's font
(css-values-4/#font-relative-lengths). In the font group, they are relative to the parent's font. em and lh read the
computed font size and line height. ex and cap read the x-height and cap height of the first available face. ch and ic
read the advance of "0" and of U+6C34 in the face that sets each glyph. The advance is measured down the line if the
glyph is set upright.

The producer of styles measures these metrics (`CssFontMeasure`). The presentation measures them in text fonts of its
own, which a lock guards so that any restyle thread can use them. A metric is measured once per style that reads it. A
metric that cannot be measured takes a fallback: 0.5em for ex and ch, 1em for ic and for upright ch, as css-values-4
gives them, and 0.7em for cap. A face with no cap height measures its ascent for cap (css-values-4/#cap). A style
computed without a producer that measures, such as a style computed alone, takes the fallback for every metric. A style
that reads a metric is marked (`readsFontMetrics`, `readsRootMetrics`) and is not shared through either cache. It is
computed again when the document's fonts change, such as when a web font loads. A style that reads no metric costs
nothing more. Once a style of the document has read a metric, a change to the root's font restyles every element.

A flow-relative property and its physical counterpart share one computed value, the physical property's, and inherit
as the physical property does (csswg-drafts#3029). Once the font group gives an element's writing mode and direction,
each flow-relative longhand's winning declaration replaces its physical counterpart's if it comes later in cascade
order. A style therefore holds only the physical properties, and a flow-relative property reads as its counterpart.

An element that matched the same entries as an element computed earlier, under a parent with the same inherited groups
and custom properties, takes that element's style without computing it. An inherited group that the cached style took
from its parent with no declaration may differ: the element then takes the cached style with its own parent's group in
place of that one. The style's other groups do not depend on such a group, unless the style resolved a length relative
to its font or mapped a flow-relative property, both of which read the font group. A style that read a parent's group
that does not inherit, as `inherit` does, is reused only under the same groups of every kind. A style that read the
viewport is reused only at the same viewport size. Each thread keeps such styles for the rule set that it was built
from. An element with its own declarations, such as an inline style or presentational hints, computes its style, as a
check of the presentation against a full recomputation does.

Those styles are a cache of fixed size, so whether an element finds its own last style there depends on how many other
styles came between. Each element therefore also keeps what its style was last computed from (`CssCascadeMemo`): the
entries it matched, its parent's style, the root's font and the viewport if the style read it. A restyle that finds the
same takes that style whole. An element whose style is costly to compute, such as a root that declares a design
system's custom properties, costs a match and no cascade each time it is restyled to the same style.

An element whose style is computed takes each group that has declarations from a cache of computed groups when it can.
A group is cached by what its values can read: the declarations that won its properties, the parent's group of the same
kind if the group inherits, the element's font bases, and the rest of the element's surroundings that a value reads. The
items of different lists that declare one inherited property, as `li { text-align: match-parent }` does, then share the
group that holds it. An inherited group whose declarations only restate initial values, under a parent whose group is
the initial one, is the initial group, and is taken without building or hashing it. This matters for the `font`
shorthand, which resets every longhand that it does not set. Without it, every element that declares `font` would
build a typography group.

The font group holds the properties that select a font. The typography group holds what text asks of shaping and line
breaking beyond its font: font features, capital and position forms, kerning, the synthesis of small capitals and
positions, word-break and overflow-wrap. The `font` shorthand resets most of the typography group, so the typography
group is computed right after the font group. A shorthand whose value substitutes a variable is then parsed once for
both groups. Text that declares none of the typography properties shares the initial group. The producers read the
initial group as no font features (`cssFontFeatureSet`) and no tailoring of line breaks (`cssTextBreakStyle`).

A document's rule set is built on a rule set of the user agent's sheets alone, which the presentation files once for the
document's conditions and media. A change to the document's sheets copies that set and files only the document's own
sheets over it.

`var()` and `attr()` are substituted where a value computes (css-values-5/#arbitrary-substitution). `attr()` reads the
attribute of the element styled, or of a pseudo-element's originating element. A style that reads attributes this way
is not shared with elements that matched the same entries, and a change to an attribute that a rule's `attr()` names
restyles the element that carries it. A URL made from an attribute's value makes its declaration invalid
(css-values-5/#attr-security).

Custom properties are a set per element, with references substituted once each. An element that declares none shares
its parent's set. An element that declares some gets a new set, which holds its declarations and its parent's own
entries, layered over a shared base. Lookups read the set, then the base. The base is the set at the top of the chain,
usually the root's, which holds a design system's variables, so declaring one property copies only what was declared
between the base and the element, not every inherited variable. A set is flattened into a new base once its own
entries outgrow the base. The CSS-wide keywords apply to custom properties as to any other property.

The user agent's rules come from sheets that the presentation's creator passes (`PrCssUserAgentSheet`). Each sheet
applies while the document meets its conditions, such as quirks mode. A change to those conditions restyles every
element. The selectors of these sheets may name the engine's private pseudo-classes, such as `:-pr-list-box`. Author
sheets cannot name them. A sheet may apply only to the widgets rendered with one appearance, native or base
(css-ui-4/#appearance-switching), and holds that appearance's look. The cascade decides an element's used appearance
from its kind of widget and from the declarations of every other sheet. Those declarations give the computed
`appearance`, and whether an author's declaration devolves the widget (css-ui-4/#appearance-disabling-properties). A
second pass then applies the entries of the sheets for the used appearance in their place. Only an element that matches
such an entry takes the second pass. A pseudo-element takes its originating element's used appearance. HTML's sheets
(`html_style.h`) hold every rule of the HTML Standard's rendering section that applies at the user agent's level. They
are written for the rule index: a selector names a type, a class or an attribute of its subject wherever it can.

Presentational hints have an origin of their own between the user agent and the author (css-cascade-5/#preshint).
Their declarations have zero specificity. Every hint is built in code (`css_hints.h`), as a block of declarations for
the element. An element builds its block on first use and keeps it until an attribute that it reads changes. That
change restyles only the element. Some hints come from another element: a table's attributes style its row groups,
rows, cells and column groups, and a body's link colors style the document's links. The table or body builds one block
for each kind of element that takes its hints. The elements that share a block can share a computed style.

An element's transform is its `translate`, `rotate`, `scale` and `transform`, composed in that order
(css-transforms-2/#ctm), with their 3D functions and percentages, relative to the box that `transform-box` names. A
translation by a percentage to the left of a function with perspective, which applies after it, counts as zero.
`perspective` (at least 1px), `perspective-origin` and `backface-visibility` project onto the effects group, and a
perspective makes the box a containing block, as a transform does. A transform that is not affine on the element's
plane paints as a projected group, which flattens into its parent's plane: `transform-style: preserve-3d` is not
rendered. The resolved values of `transform-origin` and `perspective-origin` are pixels from the reference box's top
left, if the element has a box that is not an inline box.

## Presentation (`css_presentation.h`, `pr/css_presentation.h`)

A presentation is the rendering of one document: each element's computed style, projected onto `lib/layout` records, and
the layout tree of the boxes that they generate. The document marks its nodes as they change (`DomNode::pending`), as
the rule set's invalidations say. An update visits only the marked nodes. It restyles an element that changed and
whatever the change reaches, restyles children if their parent's inherited values changed, and changes the layout tree
as little as that requires. If every style of a large subtree is computed again and someone waits for it (a synchronous
read, or a change after all of the document's content has been taken in), the update first computes the styles on its
threads, dividing the subtree's subtrees among them, then walks the subtree and takes each style. A load's restyles stay
on the caller's thread. `prCssPresentationVerify` checks an update against one from scratch, and
`prCssPresentationStats` counts what the update did. A browsing context renders its document if its host gives a default
text font and a shaper, and `getComputedStyle` reads the presentation.

Some of what a box is comes from the element's markup, not its style: whether it is replaced, a line break, a button
or a text field, the columns that a table cell spans, whether `display: contents` generates nothing, and the page's
supported color schemes. Each markup language gives the document's CSS a provider for its namespace (`css_boxes.h`),
as it gives one for its hints, and the presentation asks it as it projects an element. A provider names the tags that
it answers for, so the elements of every other tag cost a projection no call. The content of outermost svg elements is
lib/svg's, which fills the presentation's records of them (`css_presentation_svg.h`, [svg.md](svg.md)).

The presentation renders the flat tree (css-shadow-1/#flattening, `dom_flat.h`): a shadow host's children are its shadow
root's, a slot's are its assigned nodes or else its own children, and a light child that no slot takes is not
rendered. Every walk reads the flat children, and a change marks its way up through the flat parent, so a change in a
shadow tree, or to a slotted node, reaches the walk through the host or the slot. Styles inherit along the flat tree,
so a slotted element inherits from its slot. A slot is `display: contents` through HTML's sheet. A slot's change of
assignment, and a host's new shadow root, tell the presentation which nodes left or joined the flat tree. In a document
that has never had a shadow root, the flat tree is the light tree, and its walks are the light tree's loops.

Restyling reads nothing from the layout tree. It decides which nodes have boxes and queues the resulting changes, and
the tree takes the queue while the page's thread holds the tree. So a host's job (`prCssPresentationStart`) restyles
the next job's content while the agent's layout thread lays out the previous job, and the two run as a pipeline instead
of taking turns. With a job budget (`prCssPresentationSetJobBudget`), content that the document added goes into jobs in
tree order, as much as the presentation's measured rates of laying out and styling say fits the budget. The rest waits,
as content that the parser has not reached does. Changes to content already taken in are never held back, so a change
that a user makes waits for one short job at most.

A box's background projects onto the decoration group: its color and its layers, one for each item of
`background-image`, with the same item of each other background list property, repeated as needed
(css-backgrounds-3/#layering). Colors, including those of a gradient's stops, resolve where the style projects, for the
element's color and the display, and a `light-dark()` image picks its image there, by the element's used color scheme.
An image that a style names by URL is fetched once for the presentation and decoded by the browsing context
(`DomDocumentHost::decodeImage`), and it resolves against the document's base URL. An image that is still loading keeps
the nodes whose records named it, and projects only those again when it settles. A color that the display's headroom
decides reads state outside its style, so its box projects again when the headroom changes. The background longhands,
`background-blend-mode` among them, are a style group of their own, apart from the decoration group, since most elements
declare no background. If the root's background has neither a color nor layers, the root takes the body's background. A
gradient converts to layout's form in one place (`css_gradient.h`), for any client. The presentation interns a
calculation of a length and a percentage in its layout tree. A client without a layout tree, such as an SVG tree's mask
layer, keeps only the calculation's length.

`clip-path`, `clip`, the mask layer and mask border longhands, `filter`, `backdrop-filter`, `mix-blend-mode` and
`isolation` are a style group of their own, the compositing group, since few elements clip, mask, filter or blend.
The group projects onto layout's compositing group. An element whose group is the initial one takes layout's initial
group without reading its own. On a box, a mask layer's fill-box is the content box for mask-clip and the border box,
mask-origin's initial value, for mask-origin, and stroke-box and view-box are the border box for both
(css-masking-1/#the-mask-clip, css-masking-1/#the-mask-origin).

A filter function computes with the defaults of its omitted arguments. The color of drop-shadow() resolves only in the
resolved value of a filter property. `filter()` is an `<image>` wherever one is written
(filter-effects-1/#FilterCSSImageValue). Its filter list computes as the filter property's does, and it projects as its
input, a decoded image or a gradient, with the list's items in the layout image (`PrLayoutBackgroundLayer::filters`). A
filter() inside filter() appends its list after the inner one's. A URL in the list that names no filter element leaves
the list filtering nothing, as in the filter property, and an input that cannot be displayed makes the image fail. In
generated content and as a marker image it is a replaced box that paints the image as its background, sized by the
input's natural dimensions. `css_filter.h` turns a computed filter list into layout's filter functions, for any client.
A negative argument is invalid, except to hue-rotate() and to the offsets of drop-shadow().

A basic shape computes with its lengths absolute, its position as two length-percentages, and `xywh()` and `rect()` as
`inset()` (css-shapes-1/#basic-shape-computed-values). The shape resolves where it is drawn, in a reference box that
only its user knows (`css_shape.h`): the object bounding box of an SVG element, or a box that layout gives. A
`circle()` takes a single size of css-images-4's `<radial-size>`, which may be a percentage. The `rect()` of `clip`
separates its edges with commas or with spaces, but not both (css-masking-1/#funcdef-clip-rect). A URL in clip-path, a
mask layer or filter names an SVG resource, which the presentation gives layout as a source (svg.md, Presenting SVG).

Colors resolve in their element's context (`CssColorContext`): its color for currentcolor, its used color scheme for
`light-dark()` and the system colors, and its accent color for AccentColor. A color function computes to the color
that it resolves to, in its own space, unless it reads one of these. A color that reads the display's headroom or the
user's preferred scheme marks its record as reading state outside its style, so the record projects again when either
changes.

A `::before` or `::after` whose element's box holds its children generates a box as the first or last of them, if its
content is neither `none` nor `normal` (css-pseudo-4/#generated-content). The presentation keeps such a pseudo-element
as a node of its own (`RenderPseudo`), which the document does not hold, with an entry and a box like an element's. Its
style is computed with its element's, under the element's style. The rule set keeps apart the entries that set content,
and an element whose pseudo-elements match none of them, as most elements' do, computes no style for them, whatever
other rules, such as a `*::before` reset, they match. The boxes of the content are a text box for each run of strings
and a replaced box for each image that has loaded. A point on any of them hits the originating element.

An element that a `::first-line` or `::first-letter` rule matches originates that pseudo-element
(`cssCascadeOriginating`). If a rule set has no such rules, its elements originate none, and nothing is matched to
find that out. The presentation keeps the styles of these pseudo-elements for each element that originates one, and for
no other element (`RenderFirsts`). The first line's style is computed under the element's style, and the first letter's
under the first line's (css-pseudo-4/#first-letter-tree). The cascade drops their declarations of properties that do not
apply to them (css-pseudo-4/#first-line-styling, css-pseudo-4/#first-letter-styling), such as a transform or a position,
and writing-mode, direction and text-orientation on `::first-line`.

Once the boxes of an update are in place, the presentation applies these styles, by one rule for HTML boxes and SVG text
(`applyFirsts`). In HTML, each box on the first formatted line of the element's block container, through its first
in-flow block descendants, gets a layout record for that line (`prLayoutNodeSetFirstLineStyle`). That record inherits
from the parent's record for the line, and takes custom properties from the parent's own style
(css-pseudo-4/#first-line-inheritance). The first-letter text (`prTextFirstLetter`) is moved into an anonymous inline
box of its own, or a float, before the text box that it came from. The `::first-letter` styles that box, and inherits
from the box that holds the text, as that box is styled on the first line. The pseudo-elements of an element and of
its ancestors nest, so the elements are applied outermost first. SVG text applies both pseudo-elements to the parts of
its characters ([svg.md](svg.md#text)).

A list item, an element whose display has `list-item`, has a `::marker`, which comes before its `::before`
(css-lists-3/#marker-pseudo). Its style is computed for every list item, and it generates a box if its content is not
`none` and a content of `normal` has a marker image or a marker string to show (css-lists-3/#content-property). Of the
properties that do not inherit, only those that css-lists-3/#marker-properties lists apply to the marker's box, which
takes the initial values of the others. The inherited values reach the marker's text, except text-align and
text-indent, which place no marker's text. An inside marker's box is an inline box, and an outside marker's is a marker
box, which layout places beside the item's first line. A `::before` or `::after` that is a list item has a marker of
its own, the first box of its content.

Counters follow tree order over the boxes of elements and pseudo-elements (css-lists-3/#creating-counters), and so
does the quote depth (css-content-3/#quote-values), so the presentation computes them in a pass of its own
(`dom_counter.h`), after a restyle decides which nodes have boxes. A participant is a node with a box that resets,
sets or increments a counter, as every list item does, or a pseudo-element whose content reads a counter or steps the
quote depth, as a marker and a quote do. A quote's marks come from the quotes of its pseudo-element, whose auto takes
CLDR's marks for the language of the originating element's parent, so a restyle of the originating element runs the
pass for its quotes. Each element whose subtree holds a participant carries a
flag, so the pass walks only those subtrees. An update runs the pass only if a participant came or went, or was
restyled in a way that reaches counters, and the pass sets a pseudo-element's text again only if its counters give
another one. Inserting an item at the start of a list changes every number after it, and costs what it changes.

A pass keeps where it ended for the next: the last element it walked and that element's ancestors, with the counters in
scope where each of them closes, past its content that the pass walked. The presentation notes the element at each
change to the participants. If every noted element follows one of those points, the next pass starts at the deepest such
point and walks only what follows it, so a parser that appends content in jobs pays for what each job appends. A
removal, a change before the points, or a reversed counter in scope at the point, whose initial value counts what
follows, starts the pass at the root.

Page-margin boxes (`css_presentation_page.h`) are boxes of the presentation's tree, children of the root's box, which
layout places in the margins of each page (layout.md, Paged media). The view resolves the @page rules that apply to
each kind of page. The presentation cascades their declarations into styles that no element's rules give
(`cssCascadeBlocks`). The page context's style inherits from the root's. Each margin box's style inherits from the
page context's, and takes its margin rules' declarations after the user agent's text-align and vertical-align
(css-page-3/#margin-text-alignment). A margin box whose content computes to none generates no box.

Once the content is laid out into pages, the presentation makes each page's margin boxes from what their content gives
on that page:

- strings and images;
- the page and pages counters, and the counters of the page context;
- the document's counters as they are at the start of the page (css-page-3/#page-based-counters);
- named strings (`string-set`, css-gcpm-3/#named-strings);
- copies of running elements (css-gcpm-3/#running-elements).

The last three take one walk of the document in tree order, and only if a margin box reads them. A named string's or a
running element's assignments are placed on the page of the element's box. An element with `position: running()` has
no box, only a placeholder (`PrLayoutPosition_Running`), which places it on a page. A copy is a copy of the running
element's boxes (`prLayoutTreeCopy`), so it inherits as the element does where it is. A page's margin box whose record
and content are the same as before is kept, so a second pass over unchanged pages changes nothing.

Counter styles (`css_counter.h`) turn a counter's value into text. The predefined styles that css-counter-styles-3
writes as rules are generated from its source into a table (`css_counter_style.h`), with the complex styles' characters
taken from the draft's tables and their algorithms written as systems of their own. A document's @counter-style rules
are resolved with the rule set into styles that take the place of predefined styles of the same names.

CSSOM View's geometry and scrolling (`pr/css_view.h`) also read the presentation. A read brings the presentation up to
date on the caller's thread, including all the content that is waiting, waits for the layout thread if it holds the
tree, and answers from the layout tree as painted or as laid out. An element's scroll offset is its box's offset in the
layout tree, and the root's overflow is the viewport's, which HTML's body gives if the root's overflow is visible. A
scroll, whether by script or handed back from a compositor, queues its scroll and scrollend events for the next
rendering update.

## Animations (`css_animation.h`, `dom_animation.h`)

A presentation runs the transitions that changes to an element's style start, and the CSS animations that its style
names, on a timeline whose time the host sets each frame. An element keeps its base style, which is the cascade's, and
the style that it presents, which applies its animations' values over the base as the animation and transition origins.
A frame samples each animating element's animations over its kept base. Nothing is matched again, and what the change
reaches follows from the groups whose values changed, as for any restyle. Values interpolate as computed values. Colors
interpolate premultiplied, filter lists function by function, and basic shapes argument by argument. Track lists of one
length interpolate track by track, and repeat()s of one count over lists that do. A track that does not interpolate with
its counterpart switches on its own, and line names switch (css-grid-1/#repeat-interpolation). A pair that does not
interpolate switches at the halfway point, as every pair of a property whose animation type is discrete does, with one
exception. The properties of images are discrete, but two filter() images of one input interpolate by their filter lists
(filter-effects-1/#interpolating-filter-image), in lists of images of one length whose other items are equal. Any other
pair of images would cross-fade (css-images-4), which the engine does not draw, so it switches.

Animations of opacity and transform alone are composited. The element's box is a layer that keeps its base effects,
the presentation hands the animations' tracks to a compositor (`prCssPresentationTracks`), and the presentation's frames
visit the element only when such an animation ends. A read of the element's computed style samples them. Each animation
keeps the phase that its events last saw. A rendering update finds the changes at its time and fires their
TransitionEvents and AnimationEvents in the order in which they happened, including those of animations canceled since.

A pseudo-element that the presentation generates animates as an element does, with a base style and a presented style of
its own. Its base style decides whether it exists, so an animation that takes its box away keeps it and its animations,
and a base style that generates nothing cancels them. Its events go to the originating element, and name the
pseudo-element.

Suites: `css-parsing` (the tokenizer and consume algorithms, tested directly) and `css-wpt`.

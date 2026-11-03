# Text

A producer turns styled text into runs with `lib/text` (text fonts and itemization), shapes each run into glyphs of a
`lib/font` font, places the glyphs as a glyph run, and records a `Glyphs` command. A producer outside CSS's inline
layout gets all of that from a text layout. Layout, SVG text, Lottie and the viewer are producers, and they share the
same face selection and itemization, so the same text in the same style is set the same way in each. Draw backends read
the font, never the shaper.

## Fonts (`pr/font.h`)

- `lib/font` reads its fonts itself, with no dependency: TrueType outlines and their variations (`glyf.cpp`), CFF and
  CFF2 charstrings (`cff.cpp`), advances (`glyph_metrics.cpp`), color glyphs and palettes (`colr.cpp`), and the other
  tables (`sfnt.cpp`). The readers load what FreeType loads: the tests hold every glyph's points and advance to
  FreeType's at each corner of a variable font's design space.
- Hinting is a backend interface (`pr/font_hinter.h`), and a derivation names the hinter that fits a font's outlines.
  The FreeType backend hints. A font without a hinter draws its design outlines at every size, whatever its hinting.
- Every length is in ems, with y pointing down from the glyph origin on the baseline.
- A font copies or maps its file's bytes, and everything reads those bytes, never the system's font resolution. A WOFF
  or WOFF2 file is decoded into the sfnt or collection that it holds, with a codec that the caller gives
  (`prFontCreateWoff`), so `lib/font` carries no compression of its own. WOFF2's transformed glyf, loca and hmtx tables
  are rebuilt in `lib/font/src/woff.cpp` before the readers or the shapers see the font, so every reader of the bytes
  reads an sfnt.
- A derivation (`@font-face` descriptors, rendering options) is a separate font that shares its base's bytes.
- Synthesis and hinting are properties of the font, so the shaper and the backend agree. Results that depend on size
  (hinted outlines, advances) take the size as an argument.
- Every metric and baseline has a value, which is synthesized if the font lacks it.
- Color glyphs: COLR is walked into pushes, pops and fills that `lib/draw` lowers. Their paint types are COLR's own,
  not draw's, because `lib/draw` depends on `lib/font`, so `lib/font` names nothing of `lib/draw`. Bitmap glyphs
  are drawn by `lib/draw` through `lib/image`, and SVG glyphs by `lib/svg`, each a glyph decoder. `lib/font` decodes
  no pictures.
- A font is immutable and thread-safe. The readers only read the font's bytes, so they take no lock. A mutex guards the
  cache of outlines, and painters are called without it held.

## Font sets (`pr/font_set.h`)

A font set implements CSS font matching over faces. Faces are described as `@font-face` rules describe them, and
installed fonts are further faces. "Face" means only that, a member of a set, while a font is one instanced font of a
file. Files open on their first match. A match asks a family for a style (`PrFontStyle`: weight, width, slope, size,
and what may be synthesized), sets variation axes, and synthesizes bold or oblique if the face lacks them. The matched
faces go into a `PrFontMatch` that the caller reuses, which holds references to their fonts. A host sets how a set
hints the faces that it matches (`prFontSetSetHinting`), and the sets over it hint theirs the same unless they set
their own, so a document's fonts are hinted as its host's are. Layout shapes text at the target pixels of its size, so
that a fully hinted font advances as hinted, and shapes it again when the target pixel ratio changes.

- **Installed fonts come from the host.** `prFontSetAddFile` adds a file's faces, reading their tables but creating no
  font. `prFontSetAddFileFamily` adds a family by a file that holds it without reading the file, which a match reads
  when it first asks for the family. The backends of `lib/font` add the system's fonts that way, by the families that
  CoreText (`prFontCoreTextInstall`) and DirectWrite (`prFontDirectWriteInstall`) know them by, so the files of the
  families that no text asks for are never read. `lib/font_bundle` adds the fonts that a build fetches. `lib/font`
  itself calls no platform API.
- **Families are names or generics.** An entry of a family list (`PrFontFamily`) is a name or a `PrFontGeneric`. A
  generic is never matched as a name. The set binds each generic to a family (`prFontSetSetGeneric`), and a generic
  bound to nothing matches nothing, so the list continues. The default family may be either.
- **Sets are layered.** A set may have a parent. A family that the set defines is matched in that set alone, whether
  its faces are loaded or not, and any other family is matched in the parent. A document's `@font-face` faces are a
  set over the host's set, so they shadow the installed families with the same names, and a family whose faces have
  all failed is missing, instead of falling back to the host's family of that name. Generics and fallback families
  belong to the root set.
- **Faces can be pending.** A face added without its font (`prFontSetAddPending`) is matched like any other face but
  serves nothing. If a match passes a pending face whose ranges hold the text that it was asked for, it records a
  request. The set's owner takes the request (`prFontSetTakeLoads`), loads the face, and answers with the font or a
  failure. Meanwhile, the text is set in the next family.
- **A failed face keeps its style.** Matching narrows over every face of the family, including failed ones, and then
  leaves out the failed ones. A failure therefore leaves its style without a face, and the text moves to the next
  family, instead of to another weight of the failing family. Faces with one style and differing ranges form one
  composite face, and a failed member leaves the others in place.
- **Faces rank by the caller's order.** A family's faces are checked from the highest rank down. The rank is the order
  in which they were added, until the owner orders them (`prFontSetOrder`), as a document orders its rules.
- **One state is one font.** A face's instance is derived from its font (`prFontCreateDerived`), and equal derivations
  of the same bytes are the same font while it is open, so faces with one source and the same descriptors share one
  font.
- **A set has a version and stamps.** The version increases whenever a face is added, loads, fails, is ordered or is
  removed, or a binding changes, in the set or in a parent. Each family has a stamp of the last change that may change
  its match (`prFontSetFamilyStamp`), so a text font matches again only if a family that it names changed. Removed
  faces leave the set, and a match keeps copies of its faces' ranges.
- **One thread at a time per set.** Each set has its own lock, so a host set can be shared on any thread by the sets
  layered over it.

The families that text falls back to belong to the host, in the order that it tries them (`PrFontFallbackFamily`).
Each may name the locale that it is made for, as a pattern whose unset parts match anything (`und-Hant` for
Traditional Chinese whatever language it writes, `und-Hant-HK` for Hong Kong's forms), and the generic family whose
style it is drawn in. Faces that a document adds never join them: fallback families are matched among the faces of the
outermost set (`prFontSetMatchFallback`).

## Font bundles (`pr/font_bundle.h`)

A font bundle is a set of bindings that an application installs into its font set: the generic families, other names
that the bundle gives its fonts, and the families to fall back to, with the locales that each is made for. Every bundle
falls back to the same families: Noto for the scripts in modern use, symbols, math and color emoji, and Last Resort
last.

- `Noto` names serif, sans-serif and monospace in Noto, and math in New Computer Modern Math.
- `Standard14` substitutes for PDF's standard 14 fonts: Tinos, Arimo and Cousine for Times, Helvetica and Courier,
  with their metrics, also bound as the generic families; and LeviSymbol and Levibats for Symbol and ZapfDingbats.
- The library knows its files by name. `lib/font_bundle/font_bundle.cmake` pins each file by URL and SHA-256, and
  `pr_font_bundle_fetch` fetches them with FetchContent into a directory. The application passes that directory, or
  ships its own copy of the files. The engine's build fetches every bundle into `font_bundle` in its build directory.
- Variable fonts are kept as they are published, with the axes in their names. Weights and widths come from their
  axes.

## Text fonts (`pr/text_font.h`)

A text font is what text is set in: a family list and a style in one font set, at a size and for a locale. Its faces
are those of the families that its list names, then of the set's default family, then of the set's fallback families.
It answers which face sets each grapheme cluster (`prTextFontMatch`) and caches the answer. A text font is reference
counted, so a producer makes it once and names it wherever its text is set. Layout's font group holds one.

- A cluster takes the first requested face that covers all of it, in the order for its presentation: color faces come
  first for a cluster with emoji presentation, and last for any other cluster. Then come the fallback families, those
  that declare the cluster's script first, each ranked by how well its locale suits the text's and whether its generic
  matches the style's. A cluster with a private-use code point or a control character does not fall back.
- An answer is an index into a table of the fonts that the text font has answered with. Runs name fonts by these
  indices. A new answer appends to the table.
- The first available face is the first requested face whose ranges hold U+0020. Its metrics give the style's strut
  and font-relative lengths.
- The advance of a character (`prTextFontAdvance`) is the advance of its glyph, along or down the line, in the face
  that the text font sets it in. The ch and ic units read it.
- If a family that it depends on has changed, the text font's update (`prTextFontUpdate`) matches its families again
  and asks every question that it has recorded again. If every answer is the same font, the table and its indices
  stay. Otherwise the table is emptied, the revision (`prTextFontRevision`) increases, and each owner that kept answers
  drops them. A text font records a bounded number of questions, and a text font that went past the bound gives up its
  table at the next change.

## Itemization (`pr/text_itemize.h`)

Itemization takes text given as pieces in logical order: text in a style, an inline's start and end with its
`unicode-bidi`, an object, a paragraph break, or a boundary that only ends shaping. It produces UAX #9 levels and
paragraphs, UAX #24 scripts with paired brackets, the orientation of each character, and runs. A run is a maximal
range in one piece's style, face, script, level and orientation, with the text around it that shaping reads as
context. Each producer keeps what is its own: layout its lines, tabs and line reordering, SVG its chunks, positions and
SVG fonts, and Lottie its animators.

## Shapings (`pr/text_shaping.h`)

A shaping holds what a text's lines need that does not depend on their width: its runs, glyphs, advances and break
opportunities. Any host can use it, and it needs no layout tree. A flexbox's measure function can set text through a
shaping alone.

- The host describes the text as pieces (`pr/text_itemize.h`), each with its text font, features and break style. An
  object is a piece, and the host gives its advance (`prTextShapingPositions`).
- The host measures and breaks one line at a time (`prTextShapingLineWidth`, `prTextShapingBreakLine`), so each line
  can have its own width. It reads the text's intrinsic widths from `prTextShapingIntrinsic`.
- A shaping holds what it derives from the text, not the text. The host passes the text to each call that reads it.
- What differs between hosts is an option, not a hook. A piece says whether lines wrap in it and whether they may break
  after any space, each byte of white space says what a line's end does to it, and the host chooses where lines may
  break beside objects and gives the tab stops. Layout's inline formatting is a host, and it describes its items this way.

## Text layouts (`pr/text_layout.h`)

A text layout is text set in lines in one width and one font, for a producer that needs nothing more: a UI's label, a
text field, or a drawing's caption. It is a host of a shaping. It breaks lines greedily, with the white space at the end
of a line hanging past its width, and aligns each line. It gives glyph runs (`PrFontGlyphRun`), the lines that they are
on, caret stops at grapheme clusters, a caret's position, and the stop nearest to a point.

A glyph run belongs to `lib/font`, not to text or draw: text places glyph runs and draw records them, and neither
depends on the other. A producer that shapes runs itself places a glyph buffer as a glyph run
(`prTextGlyphBufferPlace`).

## Shaping (`pr/text.h`)

`lib/text` is a backend interface. Its backends wrap kb_text_shape (`kb`), HarfBuzz (`harfbuzz`, the reference) and
CoreText (`coretext`), and a host may implement its own. The simple backend (`simple`) needs no library: it gives each
character its glyph and kerns pairs by GPOS or the kern table, which is all that Latin, Greek and Cyrillic text without
ligatures needs. It places such text as HarfBuzz does.

- A run carries its script and its locale (`pr/locale.h`), and each backend converts them at its boundary. HarfBuzz
  and CoreText take the locale's BCP 47 tag, and kb takes its OpenType tags.
- Runs come from itemization, and the caller breaks lines (`pr/unicode_line_break.h`), with the opportunities tailored by
  `pr/text_break.h`.
- What a style asks of shaping is a feature set (`PrTextFeatureSet`): its features, capital and position forms,
  kerning and synthesis. A feature set is immutable and reference counted. A producer makes one for each distinct
  style and applies it to each run. Null asks for the defaults. HTML layout and SVG text read it from the same computed
  style, so the same text shapes the same in both.
- Text before and after a run is context: shaping joins across it, but produces no glyphs for it. A run split by style
  shapes the same as the whole run.
- Shapers agree on the pen position plus offset. The split between advance and offset, and the order of overlapping
  glyphs, may differ.
- A run from top to bottom is upright text, placed by horizontal origins about the line's center. HarfBuzz and
  CoreText shape it vertically, with vertical forms and vertical positioning. kb sets it one grapheme cluster at a
  time, horizontally, with vertical forms and no context, and leaves out vertical positioning.
- `prTextShaperShape` enforces what callers rely on for every backend: monotonic clusters, caret stops, and the
  lowering of kerning, caps, and subscripts and superscripts to features. If any character lacks the feature,
  synthesis applies to the whole run.
- Fallback happens before shaping, by cluster, in a text font, and a run is in one face. Each run is shaped in its font
  with its surrounding context. A default ignorable code point that a face draws as nothing produces no glyph.

Tests compare every backend with HarfBuzz. Known differences are separate tests marked to fail.

## Breaks within words (`pr/text_break.h`)

Line breaking is Unicode's (`pr/unicode_line_break.h`), and lib/text tailors it once for every producer of text: HTML layout
and SVG text break words alike.

- `prTextBreaksTailor` takes the opportunity after each code point, and the word break and overflow wrap of the text
  that each code point belongs to. A word break that adds or removes opportunities between letters changes them in
  place. An overflow wrap, or a word break that breaks words that fit no line, gives an emergency opportunity after
  each grapheme cluster inside a word. The caller takes an emergency opportunity only if no ordinary opportunity fits.
  Each says whether it counts when the narrowest width of the text is measured. A boundary between text of different
  styles takes no tailoring.
- `prTextFirstLetter` finds the initial letter of a block's text: its first letter, number or symbol cluster with the
  punctuation around it. Every producer that sets the letter apart, as a box or as a styled part of its text, finds
  the same extent.

## Text editing (`pr/text_edit.h`)

`lib/text_edit` holds what editing means, apart from how the text is shown, so form controls, component libraries and
editors share it. It has no dependency on UI or layout, and it is tested without a window.

- **The buffer** is UTF-8 in a gap buffer, with an index of its lines. Offsets are 32 bits, and a buffer holds at most
  2 GB, well past the largest document that editing targets. A reader takes chunks, and does not assume that a document
  is contiguous. Only text that is shown whole asks for it as one view.
- **Transactions.** Every change is a transaction that replaces byte ranges, in one mutation primitive. A transaction
  has an origin, which decides whether it can be rejected and whether it enters the history.
- **Proposals.** A local or composition transaction is proposed before it applies. The host may reject it or change its
  text, which serves a cancelable input event, a filter, or a limit on length.
- **The edit stream** gives each consumer the replacements of each transaction, with their lines and columns, at its own
  pace. Highlighting, language servers and replicas of the text on other threads read it.
- **Anchors** are positions that follow their text, with a bias that decides where they go at an insertion. A view's
  selections follow the same rules, except that a selection of the view that made an edit lands after the new text.
- **The history** holds invertible steps and the selections around them. It belongs to the owner of the model. A
  composition stays out of it until it commits. Runs of typing or deletion join one step if they continue from where the
  last step left the selections.

A text control in a document holds its value in a buffer for its life, and its selection in a view of that buffer, as
bytes. Script reads and sets the selection in UTF-16 code units, and the binding converts. `beforeinput` runs ahead of an
edit, outside the transaction, because a listener may change the control. Its `::field-content` shows the value.

### Commands and units (`pr/text_edit_command.h`)

Every editor shares what a key press asks for and what it moves by, so that keys and motions mean one thing everywhere. A
key press names a command by the platform's bindings (`prTextEditCommandFromKey`), and the editor decides how to carry it
out: a text field in lib/ui by its own edit, and a form control by way of `beforeinput`.

Words are the word segments of UAX #29 that hold a letter or a number, and a run of Han or of Hiragana is one word,
since the report ends a word at each of those characters. Backspace removes one code point of a grapheme cluster, so
that a writer can correct a mark or a vowel, and takes whole a cluster that means one thing only as a whole: an emoji
sequence, a flag, a keycap, or a character with a variation selector. Forward Delete and motion take the cluster.

### Styles

A buffer holds style runs beside its text, as opaque values that mean something only to whatever shows the text, so a
text that no one styles pays nothing for them. They follow every transaction. Text takes the style of what it replaces
or follows, and a style set at a caret waits for the next insertion. The history restores the styles of what it brings
back, and a style change is a step of its own that makes no version of the text.

`prUiTextEdit` shows each run of a caller's buffer as an inline box with a text node in it. A run's value indexes a table
of records that the caller interns in each round, because a record that no node names is freed, and a buffer's values
outlive it. Offsets stay in the flat text, and `prTextEditGeometryFromLayoutRuns` maps them to the nodes, so that the
place where two runs meet is one place for a caret. Kit's text area gives its styles as roles and tones, and the studio
gallery's controls section has one to try.

### Motion (`pr/text_edit_motion.h`)

A key names an arrow as it lies on the screen, and the flow of the caret's line says whether that arrow runs along the
line or between lines. Motion works in positions, a node and an offset, over an interface to geometry
(`PrTextEditGeometry`), so that flat text and a tree of nodes share one set of rules, and tables of cases run without a
window.

- **The arrows along a line** have three behaviors in bidi text (`PrTextEditArrowMotion`), which differ only at a change
  of direction. Logical, the default, runs through the text in order, and the arrow that points the way the paragraph
  runs goes forward, so the caret jumps at the change. LogicalStops adds a stop there, which reaches both of the places
  that the offset has. Visual walks the line's stops in order of their place along it. No motion stops twice at one
  place, and a press past a soft wrap moves past the offset.
- **Words and the ends of a line** go by the paragraph's direction and the order of the text, so the end of a line is
  its last offset, wherever that shows. Words span the nodes of a paragraph.
- **A collapsing motion** goes to the end of the selection on its side, by the paragraph's direction. Between lines, it
  moves on from there.
- **Between lines**, the goal is a position along the line in layout coordinates (`prLayoutLineStops`), so it holds under
  transforms and in every writing mode.

`pr/text_edit_layout.h` answers the interface from a layout tree. Text that no layout shows as it stands, such as masked
text, uses its bytes' lines (`prTextEditGeometryFromText`).

### The windowed view (`pr/text_view.h`)

A document of any size shows through a window of rows near the viewport, each a block for one logical line, so that the
cost of a frame follows the window. The height index is a sum tree of two levels over the lines. Rows in the window are
exact. Every other line starts as an estimate from its length, and `prTextViewRefine` improves it in slices of time from
its text, through an estimator of advances (`pr/text_estimate.h`) that never shapes. A row's measurement outlives
refinement until the line's text or the width changes. The scroll position is an anchor in the text with a distance into
its line, so a correction of an estimate does not move what the reader sees.

Rows stack along the block axis of their writing mode, so a vertical view is the same index and window, with a scroll
offset along x that is negative toward an origin at the right. Style runs show as inline boxes in a row, so shaping
continues across them, and a line that is all of style zero holds its text directly. Offsets stay in the line, and the
place where two runs meet is one place for a caret. Colored spans, such as the colors of syntax categories, are layout
highlights of the rows' text nodes under the selection's, so they cost no node and no layout, and a frame that sets the
same spans again records nothing.

To motion, each logical line is a node (`PrTextEditGeometry`), so a motion reads one line's text and the window's rows,
and never the whole document. A line outside the window answers from its own bytes until a row shows it. Commands act at
every selection, as one transaction for an edit, and selections that meet join. `prUiTextView` connects a view to
immediate-mode frames, and `examples/scribe` is an editor over it, whose operations are commands (`lib/studio/command`).

### Syntax (`pr/syntax.h`)

A language is an interface that makes the syntax tree of a buffer: a tree that its grammar parses, which each edit
parses again incrementally from the buffer's edit stream. A tree gives the spans of the categories that its grammar names,
such as `keyword` or `string.special`, for the range that a host shows. Spans do not overlap: where categories nest the
inner names the bytes, and of the categories of one node the last pattern of the query wins, as tree-sitter's own
highlighter chooses. A registry holds a host's languages, finds one by name or by file, and gives each category name one
number, so a host maps names to looks once. Coloring is the host's step on top of the categories.

An update parses for a budget of time that its caller gives. A parse that runs out of time resumes at the next update,
and until it finishes, the spans are those of the last parse that finished, moved by the edits since. A text that loads
streams into its buffer in pieces, as the HTML and Markdown loads do (`PrTextEditOrigin_Load`), and each piece parses
before the next arrives, so the text and its syntax show together, and the cost of a piece follows the piece.

The tree-sitter backend (`PR_SYNTAX_TREESITTER`, off by default) evaluates the queries' text predicates itself, with
`lib/regex`. An injection hands ranges to another language of the registry: the language that its query names, or that
the text of a capture names, such as the info string of a Markdown fence, which a language's name pattern matches. Its
content leaves out the ranges of the content node's named children, as a block quote's markers, unless the query
includes them, and the matches of a combined injection are one document. An injected language injects in turn, four
deep at most, as a script in an HTML block in Markdown's inline text. Each injection in the range asked for is parsed
when spans of it are asked for, and its tree is kept: the edits move it, a query of the same ranges reuses it, and one
of ranges that an edit touched parses it again incrementally. A tree that a query does not reach is dropped. Each
built-in grammar has a header and a function of its own (`pr/syntax_treesitter_<language>.h`), so a program links only
the grammars that it names, and a host can add a grammar of its own (`prSyntaxTreeSitterLanguageCreate`).

## Languages and scripts (`pr/locale.h`)

This section describes how the engine names languages, scripts and regions, which tags are legal, where the data comes
from, and how an element's language is resolved and reaches the code that reads it.

### Standards, and what each names

Several standards name the same things differently, and each boundary converts in one place.

| What | Standard | Example | Engine |
| --- | --- | --- | --- |
| A language tag | BCP 47 (RFC 5646), validity and canonical forms by the IANA Language Subtag Registry | `zh-Hant-HK` | parsed and resolved; the raw tag is kept for `:lang()` |
| A language | ISO 639-1 (two letters) if it has one, else ISO 639-2/3, as the IANA registry lists it | `zh`, `yue` | `PrLanguage` |
| A script | ISO 15924 four-letter code, whose registration authority is Unicode | `Hant` | `PrScript` |
| A script, as Unicode's Script property | the same codes as ISO 15924's PVA column, with long names (`Han`) | `Hani` | `unicodeScript` gives `PrScript` |
| A region | ISO 3166-1 alpha-2 and UN M.49 numeric, as the IANA registry lists them | `HK`, `419` | `PrRegion` |
| A shaping script | OpenType script tag, mostly lowercase ISO 15924 with exceptions (`dev2`, `hani` for Hans and Hant, `kana` for Hira and Kana, `yi  `) | `hani` | a table by `PrScript`, at the shaper boundary |
| A shaping language | OpenType language system tag | `ZHH ` | a table by locale, at the shaper boundary |
| Locale data | Unicode CLDR: likely subtags, quotation marks, parent locales | `zh_Hant_HK` | generated tables |

HarfBuzz maps BCP 47 to OpenType tags itself. It is not a source. The generator's check compares our mapping with its
table and reports where they differ, and each difference is resolved from the sources.

### Identifiers

- **`PrScript`, one byte**: an enum of every ISO 15924 code except the private-use range, 224 in all, with
  `PrScript_Zzzz` (Unknown) first so that zeroed storage is unknown. Its code and its OpenType tags are tables indexed
  by value. `unicodeScript` returns it.
- **`PrLanguage`, two bytes**: an enum of every language subtag that the IANA registry lists, about 8,300, in
  canonical form (a deprecated subtag maps to its preferred value, and an extlang to its language), with
  `PrLanguage_None` first. Every legal language has a value, so the only languages that the engine cannot name are
  those of ill-formed or unregistered tags. Its data (the OpenType systems, quotes and casing) is in sparse tables
  indexed by value.
- **`PrRegion`, two bytes**: an enum of every region subtag that the registry lists, about 300, with `PrRegion_None`
  first.
- **`PrLocale`, four bytes**: the language in 14 bits (16,384 values for about 8,300 languages), the region in 9 bits
  (512 for about 300) and the script in 8 bits, packed in a `uint32_t`, with accessors for each. Zero is no locale. It
  is what an element's language resolves to, and it decides everything that the engine does by language.
- **Three headers.** `pr/locale.h` declares the types and functions, and names only the zero of each set
  (`PrLanguage_None`, `PrScript_Zzzz`, `PrRegion_None`). The enumerators are split by how widely they are used, which
  C's unscoped enums allow without loss. `pr/locale_common.h` names every region; the languages that CLDR counts at
  least 50 million people as using, and the most used language of each script that UAX #31 recommends for modern use
  (Table 5), with the scripts written together (Han, Hiragana, Katakana, Hangul) counted as their combinations
  (`Jpan`, `Kore`, `Hans`, `Hant`); those scripts and combinations; and ISO 15924's special codes (Common, Inherited,
  mathematical notation, symbols, emoji, unwritten). `pr/locale_full.h` names the rest. Code that branches on a value
  includes the header that names it. The rest of the engine reaches languages by tag and never parses the full list.

### Resolution

1. **A tag** is parsed by BCP 47's grammar, as well-formed or not, case-insensitively, with grandfathered and redundant
   tags mapped by the registry. An ill-formed tag, an unregistered subtag, or the empty string resolves to no locale.
2. **The tag is canonicalized** by the registry's Preferred-Value and Deprecated fields, with extlang forms (`zh-yue`)
   mapped to their language (`yue`), and then by CLDR's language aliases (UTS #35's canonical form: `cmn` is `zh`, and
   `sh` is `sr-Latn`), because CLDR's data names locales that way. A private-use script or region is dropped, and the
   script Zzzz means no script.
3. **The tag is maximized** by CLDR's likely subtags (UTS #35, "Add Likely Subtags"), and by the registry's
   Suppress-Script for a language that CLDR has no likely subtags for: `zh` becomes `zh-Hans-CN`, `zh-TW` becomes
   `zh-Hant-TW`, `sr` becomes `sr-Cyrl-RS`, and `ja` becomes `ja-Jpan-JP`. Variants and extensions are dropped. None
   that the engine distinguishes is in scope yet (polytonic Greek's OpenType system, `el-polyton`, is the first to
   add).

The locale's data is then looked up from the most to the least specific locale, as CLDR's parent locales inherit:
`pt-Latn-PT`'s quotes are `pt_PT`'s, and `zh-Hant-HK`'s OpenType system is `ZHH `.

### An element's language

An element's language is HTML's language of a node (html/#the-lang-and-xml:lang-attributes): the nearest inclusive
ancestor's `xml:lang` (in the XML namespace, on any element) or else its `lang` (in no namespace, on HTML and SVG
elements), where a shadow root's ancestors are its host's; else the document's default language; else unknown. An
empty value means unknown, and stops the walk.

- **The default language** is the tag of the last `<meta http-equiv="content-language">` pragma, set when the meta is
  inserted. It is the first token of the meta's content, or none if the content lists languages with commas. Otherwise
  it is the first tag of the HTTP `Content-Language` header, which a host gives with the response
  (`PrHtmlResponseHeaders`). Browsers take the first of several tags, where HTML would take none.
- `:lang()` matches the raw tag by extended filtering (RFC 4647 §3.3.2). A tag that cannot be resolved is still a
  language for `:lang()`, distinct from every other.
- The resolved locale is part of the element's computed style, inherited from its parent's style if the element has no
  attribute of its own, so a restyle resolves a tag once per element that sets one. It inherits along the flat tree, as
  properties do, so a slotted element without a tag takes its slot's locale. A style kept for reuse is reused only for
  an element with the same locale.
- A change to `lang` or `xml:lang` restyles the element, and a change of its locale restyles its descendants, whether
  or not a selector reads the attribute. A pragma that changes the default language restyles the document.
- `PrLocale` reaches layout in the inherited group that holds text's properties, and text runs carry it to shaping.

### What reads it

- **Font fallback** prefers the families made for the locale, closest first, so Han text in `ja` takes a Japanese face
  if the font set has one (css-fonts-4/#cluster-matching). A locale is matched with its likely script and region
  added, so the CJK families are made for writing systems, not languages: `yue` is `yue-Hant-HK` and takes Hong Kong's
  forms. Which family a locale prefers is for the host to say, because a face's OpenType language systems name the
  languages that it has localized forms for, not the languages that it is made for.
- **Shaping**: HarfBuzz takes the language as a BCP 47 tag and the script as ISO 15924, and kb takes OpenType tags.
  Each converts at its boundary. A locale's OpenType language systems are those that the registry lists for its
  language, with the system that lists the fewest languages first and deprecated tags last, followed by those of its
  macrolanguage. A macrolanguage that no system lists takes the systems of its member languages, the most used first.
  A system that the registry names for a script or a region (Chinese Traditional, Hong Kong) applies only to a locale
  with that script or region, and comes before the others. A system for a variant (polytonic Greek) is left out.
- **Case mapping**: SpecialCasing's language conditions (`tr`, `az`, `lt`), and Greek uppercasing.
- **Quotes**: CLDR's quotation delimiters, for `quotes: auto` when generated content lands.
- Later: line-break strictness (`line-break` with `ja` and `zh`), and hyphenation dictionaries.

### The generator

`tools/gen/gen_locale.py` works from the sources alone, fetched and pinned by `./dev.sh fetch` into
`third_party/language`: the IANA registry and ISO 15924's code table, each pinned by digest because neither has a
versioned URL; ISO 639-3's table, for the OpenType registry's three-letter codes; the OpenType language and script tag
registries, pinned by digest; and CLDR at a release tag (the supplemental data and the delimiters of each locale). It
writes the enums into `lib/locale`'s public headers, with the tables beside them, and `gen_unicode.py` writes Scripts.txt
by `PrScript`.

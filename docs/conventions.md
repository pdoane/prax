# Conventions

`./dev.sh lint` checks the conventions that a tool can check (`tools/lint/lint.py`). When a tool can fix a problem,
lint names the command: `./dev.sh rewrap` for comments, and `./dev.sh prax fix` for the suite's numbering. Write new
code in the style of the reference at the end of this file.

## Layout

```
lib/<module>/include/pr/          public C headers, included as "pr/<name>.h"
lib/<module>/src                  implementation, included as "<module>/src/<name>.h", namespace pr_impl
lib/<module>/<layer>/             the module's layer for a language: a C layer such as quickjs, or a C++ layer such
                                  as cpp or quickjs_cpp. A layer is laid out like a module, with public headers in
                                  include/pr/, src and tests
lib/<module>/tests                one doctest binary per module, plus one C translation unit of the public headers
lib/<module>/backends/<backend>/  a backend of the module's interface, with public headers only. It is built into the
                                  module's library when the build turns it on
lib/<module>/backends/shared/     code that the module's backends share, laid out like a backend, with internal
                                  headers in src. Only the module's backends and comparisons include it
lib/<module>/comparisons/<name>/  a backend that wraps another implementation for comparison with the module. It is
                                  not a product backend, but it follows a backend's rules
lib/<module>/CMakeLists.txt       the module's library and backends, its layers' libraries, and its tests
cmake/                            the build's functions (build.cmake), its fetched dependencies (dependencies.cmake),
                                  and the installed package
tools/<concept>/                  one part of the development workflow, named for its dev.sh command. It holds the
                                  scripts, harnesses and libraries for that command, and the command definitions
                                  (commands.py) that tools/__main__.py dispatches to. All scripts form one Python
                                  package and run as `python3 -m tools.<concept>.<module>` from the root
tools/harness/                    code shared by the harnesses, the tools and the libraries' tests
```

- The C API is the foundation. Each language sits on top of it in a layer with its own library,
  `lib/<module>/<layer>/`, and its own public headers. A script layer is named for the engine it targets:
  `lib/dom/quickjs` is the DOM's QuickJS binding. A binding's API is C, implemented in C++. Its names carry the layer's
  tag, `Js`, followed by the module's tag (`prJsDomBind`).
- A C++ layer is named for its language with `cpp`, and it is the only kind of layer with C++ public headers.
  `lib/core/cpp` is the toolkit that every implementation is written with. `lib/core/quickjs_cpp` and
  `lib/webidl/quickjs_cpp` are the toolkits for writing QuickJS layers. Every other public header is C. C++
  conveniences for a C type that need constructors or friends on the type stay in its C header under `__cplusplus`.
- The C++ API is in namespace `pr`, and the implementation is in `pr_impl`. `pr/base.h` makes `pr` visible in
  `pr_impl`, so implementation code names the C++ API without a qualifier. So does code with a using-directive for
  `pr_impl`. Applications, under `examples`, `apps` and `tools`, use `pr` and never name `pr_impl` (`lint.py`,
  `application-namespace`). Code shared among tools has its own namespace, such as `harness`.
- A binding reaches every module, including its own, through public headers. The only implementation headers it
  includes are its own layer's (`lint.py`, `binding-include`). The QuickJS layers build on two toolkits: core's
  (`lib/core/quickjs_cpp`), which wraps native handles as JS objects, and Web IDL's (`lib/webidl/quickjs_cpp`), which
  binds interfaces as Web IDL specifies.
- A library reaches another library through its public headers. Only a library's own tests, bindings and backends
  include its implementation headers (`lint.py`, `private-include`). For now, CSS, DOM, HTML, SVG, MathML and XHR share
  their implementations. Core's C implementation uses only the header-only parts of `lib/core/cpp`, such as its atomics
  and memory operations.
- Core holds the platform ([platform.md](platform.md)), the toolkit, and the foundation types: views of memory (bytes,
  strings and pixels) and the scalar and vector types they are built from. Every library brings core with it, so
  nothing in core is a subject of its own. The toolkit holds general-purpose data structures and algorithms. A
  facility with its own domain, such as JSON or task scheduling, is a module.
- A type that has a subject lives in that subject's module, however small, together with its operations. Color values
  are in `lib/color`, rectangles and transforms in `lib/geom`, and locales in `lib/locale`. The build assumes COMDAT
  folding, so functions that a consumer does not call add nothing to its binary.
- An interface module defines a record of functions and the dispatch over it. It also holds the types that those
  operations take and return, as `lib/app` holds displays and user preferences. Its library includes each backend that
  the build turns on with the option `PR_<MODULE>_<BACKEND>` (README.md). With no backends, it builds for a host that
  brings its own.
- A target links each module whose headers its files include, and links publicly if a public header includes them. It
  links nothing that it does not include (`lint.py`, `module-dependency`). The link lines in a target's
  `CMakeLists.txt` therefore list its dependencies, and a new dependency shows up there. An engine library is declared
  with `pr_library(<name>)`, which creates the target `pr_<name>`. Every target links it as `prax::<name>`, the same
  name that a consumer of the installed package or the source tree uses. Every target may use core and its C++ layer
  without linking them explicitly.
- Each file's basename is unique across `lib`. A source file has the same basename as the headers it implements, and
  defines their functions in the order that the header declares them.
- Every public name begins with a tag that its module owns, as a whole word, and each tag has one owner (`lint.py`,
  `tag`). A module owns the tag that matches its directory name, and a backend uses its interface's tag. The vocabulary
  modules (core, geom and locale) define types that appear more in other modules' signatures than in their own, so
  they own one tag per vocabulary instead. Core owns `String`, for example, and geom owns `Rect`. `lint.py` lists every
  module's tags.
- A public C header is named for its tag: `pr/<tag>.h` or `pr/<tag>_<topic>.h`. A backend's tag is
  `<interface>_<backend>`.
- Generated files carry a "do not edit" banner. To change one, change the generator or its pinned input and run
  `./dev.sh gen`.
- A comment describes only its own declaration. Do not copy a set defined elsewhere, such as enum members or a list of
  commands, into a comment. Name the category instead. Code never links to `docs/`.

## Naming

| what | spelling |
| --- | --- |
| public type / function | `Pr<Tag><Name>` / `pr<Tag><Subject><Verb>`: `PrDrawList`, `prDrawListPushGroup`. The subject is what the function creates, changes or reports, which is not necessarily its first parameter. A function with no subject is `pr<Tag><Verb>`: `prColorConvert` |
| enum value / flag | `Pr<Type>_<Name>` / `Pr<Type>Flag_<Name>`. Combinations are members too |
| constant | `k` prefix. A limit is `k<Subject>Max<What>`. In a public header, an integer that fits in an `int` is `enum { kPrX = n }`. Anything else, such as a float, is `PR_CONSTANT`, which is `constexpr` in C++. A constant is never a macro unless an `#if` reads it |
| callback type | `Fn` suffix, with `void* context` as its first parameter. A function that takes a callback takes its context as the next parameter |
| implementation | `pr_impl::`. Every module's implementation shares this namespace, so a function or type declared in an internal header begins with a word that belongs to its module (`layoutBlock`, `CssValue`, not `css::Value`). That word is the module's tag, a backend's name (`cpuShade`), or a format that the module reads (`sfntOpen`, `pngDecode`). `lint.py` checks it (`module-word`). Otherwise it is named like a public function, without `pr` |
| C++ API | `pr::`, in the public headers of a C++ layer and the sources that implement them. A source of the C++ API is one `pr` block, with its anonymous namespace inside it |
| file-local | no prefix, in one anonymous namespace per file |
| binding | `js` prefix inside a binding and in the QuickJS toolkits. A binding's C API is `prJs<Tag>`, as in `PrJsHtmlEngine` |
| predicates | `Is` + adjective, `Has` + noun, or a third-person verb (`prIRectContains`). In an immediate-mode UI, a query about an event in the current frame is a past participle (`prUiClicked`) |
| structures | `Desc` holds what a call cannot proceed without, such as a handle, an extent or a name, and is never null. Every member of `Options` has a default: it has a `Default` function, and null means the defaults. `Info` is reported. A `Desc` whose neutral value is not all zeros has a `Default` function too |
| width and height | `extent`. A `size` is one number |
| frame | the work for one displayed frame, at every layer: `PrAppFrame`, `prUiFrame`, `prLayoutTreeBeginFrame`, `PrDrawFrame` |
| pixels | a `px` is a CSS or logical pixel. A target pixel is a pixel of the surface being drawn into: a window's drawable, a texture, a screenshot. The engine draws to targets, not devices, so it has no device pixels |
| pixel ratio | target pixels per px: `pixelRatio`, `targetPixelRatio`, `--pixel-ratio`. It is not a scale, which is part of a transform, nor a resolution, which is a media feature measured in dpi or dppx |
| 2-vectors | `PrVec2` / `PrIVec2` / `PrUVec2`. The field or parameter name says what the vector means |
| padding | `_pad`, or `_pad<N>` if there are several. A padding field is `uint8_t[]`, and a padding bit-field is `uint8_t` |
| parameter | named for what it is (`PrDomNode* node`), never `self`. This is not an object-oriented API, and a handle is a parameter like any other. The same holds for a local pointer, such as a callback's context after the cast. `lint.py` checks both (`self-parameter`). In a binding, a JS object is named for what it unwraps to (`imageData`, or `nodeObject` beside a `PrDomNode* node`), or `object` if it can be any class. `lint.py` rejects `thisValue` too |

Beyond the tag, use the shortest name that the domain recognizes. That is a judgment for review, not a rule. A module
whose API is a single object names the handle with its tag alone (`PrUi`, `PrCanvas`, `PrMarkdown`). When a
specification has a long name and a shorter term for the same thing, use the shorter term (`PrHtmlPage`). A part of a
larger module keeps the words that say what it is (`PrPathPolyline`, `PrLayoutCompositor`). Some parts of a name are
rules, and `lint.py` checks them (`function-name`, `creator`):

- The subject is what a function creates, changes or reports. Properties of a part, read or set, are named for the
  part (`prLayoutNodeParent`, `prKitDockLeafActiveTab`). An edit that rearranges the parts of a container is named for
  the container (`prLayoutTreeInsert`, `prKitDockAddPanel`, `prDrawListPushClip`).
- A getter is named for what it returns, without `Get`. A lookup from another form is `From<Source>`
  (`prLocaleFromTag`, `prGpuFormatFromPixelFormat`), and a conversion to another form is `To<Target>`. No name ends in
  `Of` or `Named`.
- A function that returns a shared, never-destroyed value for a description is named `Intern`
  (`prLayoutStyleIntern`, `prGpuSamplerIntern`).

Web modules name handles and members after their Web IDL: `CSSStyleDeclaration.getPropertyValue` is
`prCssStyleDeclarationGetPropertyValue`. A member that creates a handle follows the C lifetime conventions instead.
Either it is the handle's Create (`createElement` is `prDomElementCreate`, `createLinearGradient` is
`prCanvasGradientCreateLinear`), or it fills a handle that the caller created (`querySelectorAll` fills a
`PrDomNodeList`).

## Comments

Comments and docs are technical writing for engineers who know the field. Write clear, plain sentences that a
technical editor would accept. [technical_writing.md](technical_writing.md) says how they should read. This section
says what they must contain.

- A comment says what the name and type do not. If it adds nothing, remove it. `bool contentBased;` is not helped by
  `// its base is its content's`.
- Document responsibilities, invariants and intent: statements that stay true as the code changes. Do not narrate the
  steps of an implementation. If a step is not obvious, say why it is there.
- Lead with what the declaration does or holds. A function that does work starts with a verb (`Creates`,
  `Recomputes`). A getter may name what it returns (`The font's metrics.`). A predicate starts with `Returns true if`.
  A type or a field is a noun phrase.
- **A comment does not lean on its name.** A fragment such as `float width; // a stroke's` makes sense only beside the
  name and says nothing new. Say what the declaration holds, in words that stand on their own. If several fields
  describe one thing, such as a stroke's width, joins and caps, put one comment above the group that names the thing.
  `lint.py` flags a short trailing phrase that ends in a possessive (`leaning-comment`).
- Use the field's standard terms: cache, invalidate, dirty, incremental, recompute, dependency, pipeline, block,
  thread-safe. Do not invent a paraphrase, such as "what is kept" for a cache.
- Put threading, ownership and cost each in a sentence of its own, so that a reader can find them.
- **A comment applies to everything after it up to the next blank line.** A comment on its own line describes every
  declaration that follows it until a blank line, such as a group of fields or functions that share one contract. A
  comment about a single field is a trailing comment on that field. If it needs lines of its own, follow the field with
  a blank line. When you add a field below a commented one, put a blank line before it, or the comment will apply to
  it too. The reference shows both cases: the points of `PrSketchStroke` share one comment, and `Sketch::bounds` has
  its own.
- Cite specifications, not other implementations. The engine's behavior is the engine's own decision. A comment or a
  test states what the engine does and why. What a browser or player does belongs in the plan that weighed it. A
  backend that wraps an implementation names that implementation.
- Name a test case for what it tests. Names from a plan, including the IDs of its probes, stay in the plan, which is
  deleted once its step lands.

## Rules the example cannot show

- **Formatting.** `./dev.sh layout` fixes these rules, and `lint.py` checks them (`indentation`, `line-length`,
  `operator`, `wrap`, `initializer`).
  - Indent four spaces per level, never with tabs. Lines end by column 120.
  - A switch's case labels are indented one level inside it, and their statements two.
  - A continuation line of a statement, argument list or initializer is indented one level past the line it
    continues. It is never aligned to a column on that line, so that renaming a call does not reindent its arguments.
    A continuation of an item that began its own line is one level past that item.
  - A closing bracket on its own line returns to the indentation of the line that opened it. A lambda's body is one
    level past the line that opens the lambda.
  - The head of a function, block or control statement continues two levels in, so that it does not line up with the
    body below it.
  - A line runs toward the column limit before it breaks. Anything that fits on the previous line goes there. A short
    tail of at most 10 columns runs past the limit rather than sit alone on a line. A string literal that continues
    another joins it as one literal. A line that breaks at a binary operator ends with the operator.
  - The body of a control statement starts a new line. So does an initializer-list item that begins a line, and the
    text after a string literal that ends in a line break.
  - An initializer list that fits on one line stays on one line. One that does not breaks after its opening brace,
    ends its last item with a comma, and closes on its own line. If its items are designated, each gets its own line.
  - A conditional expression fits on one line. If it would not, write it as statements instead: an `if`, or a variable
    assigned in each branch (`ternary`). No tool rewrites these.
  - Every text file is UTF-8 with LF line endings, no trailing whitespace, and a final newline. `.gitattributes` keeps
    the endings LF in every checkout, `.editorconfig` tells editors, and `lint.py` checks each (`whitespace`).
- **Strings.** Inputs are `PrStringView`. A result is a `PrStringView` if its bytes outlive the call, as with a
  generated table or storage that the object owns until it changes. A result is an owned `PrRawString`, released with
  `prRawStringReset`, if the bytes are copied. Nothing is NUL-terminated. In C++, a NUL-terminated string or a char
  container such as `std::string` converts to a view implicitly, and views compare with `==`. A view of a whole literal
  is written as the literal, with no length beside it, and is computed at compile time (`lint.py`, `literal-view`). C
  spells the view with `PR_SV(text)`, which evaluates `text` twice. `{nullptr, 0}` is the null view, which is distinct
  from the empty view, and zeroed storage is a null string. All text is `char`, in UTF-8 unless a comment says
  otherwise. Binary data is `uint8_t`.
- **Closed sets are enums**, and open sets are strings. A parameter that selects a behavior is an enum, never a
  `bool`. Flags that combine with a type's values, such as `PrLayoutAlignmentFlag_Safe` with an alignment, are members
  of the same enum. A combination is then a constant of one type and needs no cast.
- **Booleans in structures.** Related booleans, members of one set, are a flags enumeration. Unrelated booleans are
  `bool` fields, or bit-fields once space matters. Pack unrelated booleans into flags by hand only if bit-fields cost
  measurable time, and only if the gain outweighs the cost of packing and unpacking.
- **Results.** Return values, including small structures. A call that can fail returns `bool` if there is one way to
  fail, or `PrStatus` otherwise, and writes its result through an out parameter. An out parameter for a handle is set
  to null on failure. A call that fills a container provided by the caller appends to it.
- **Lists** take one of three shapes, depending on the collection.
  - A collection that is stable while it is read, or whose elements are computed one at a time, is read by count and
    index (`prFontAxisCount`, `prFontAxis`).
  - Contiguous storage that the object owns is read by count and pointer, valid until the object changes
    (`prDrawListCommands`).
  - A computed snapshot, or one read across a lock, is written into `out`, up to `capacity` elements, and the call
    returns the total (`prLayoutNodeBoxes`). Such a call has no side effects. A call that removes what it returns, such
    as draining a queue, writes up to `capacity` elements and returns how many it wrote. The caller repeats it until
    it writes none (`prFontSetTakeLoads`).
- **Failure.** Data from outside the program (files, network, script, the user) is checked rigorously, and a failed
  check returns a status. Anything the calling code controls is a contract, checked with `PR_ASSERT`. If a caller may
  not know whether a contract holds, expose the check (`prXIsValid`) instead of making the call fallible. Validate once
  at the boundary and pass validated forms inward. If the domain has a natural answer, such as clamping, ignoring, or
  an empty or partial result, degrade instead of failing. Draw and geometry tolerate non-finite values, because
  transforms and embedding can produce them late. `PR_ASSERT` compiles out. A check that stays in release builds is an
  `if` followed by `PR_FATAL`. Running out of memory aborts, so nothing checks allocations for null. No C++ exceptions.
- **Ownership.** `Create` pairs with `Destroy` for an object with one owner, or with `Release` for a reference-counted
  object. An immutable counted object is `const` everywhere, including `Retain` and `Release`. `Destroy` and `Release`
  accept null. `Reset` frees an object's contents. `Clear` empties it and keeps its storage. Counted references never
  form cycles. Back edges are uncounted (`pr/weak.h`), or a whole tree is counted as one unit. Storage must not grow
  when the same mutation repeats, except in interning tables.
- **Initializers.** A structure built from more than a couple of values, or from literals whose meaning depends only
  on their position (`true`, `0`, `nullptr`), uses designated initializers and leaves out zeroed members.
- **Types are written out.** A declaration names its type, so that the code says what a value is, instead of deducing
  it with `auto`. Use `auto` only where there is no type to name: a lambda, a structured binding, a generic lambda's
  parameter, or a template parameter that is a value of any type. A table of anonymous structures that a loop reads
  gets a named structure.
- **Defaults.** Zero-initialization is convenient but not a design goal. A structure whose neutral value is not all
  zeros has a `Default` function. An absent value may be a sentinel (the field's comment says what it means), a
  `has<Member>` flag, or a flags set.
- **Sentinels.** An enum's absent member is `<Type>_None`. A standard's own code for unknown, such as ISO 15924's
  `Zzzz`, keeps the value that the standard gives it. The absent value of an integer type is `k<Subject>None`
  (`kPrLayoutNodeNone`, `kLayoutFaceNone`). A lookup that finds nothing returns `k<Subject>Missing` (`kArrayMissing`,
  `kHashMissing`).
- **Memory.** Use `alloc<T>`, `allocZeroed<T>`, and `prMemoryAlloc`/`prMemoryFree`. No `new`, `delete` or `<new>`, and
  no constructors or destructors in the implementation (`lint.py`, `destructor`). Growable storage is a `RawArray`. Its
  elements are copied as bytes, and its owner resets it. Text is a `PrRawString`. Storage with a known bound may
  reserve address space instead (`pr/vmem.h`) if that is worth the overhead. Use indices and relative offsets instead
  of pointers, which keeps data relocatable and smaller. Examples, tools and tests may use core's managed layer instead
  (`Array`, `String`, `Map`, `Set`, `Function`, `Lock`, `Defer`), whose values construct, copy and destroy their
  contents. An implementation includes none of its headers (`managed-include`).
- **Index domains.** An index passed between functions has a typedef that names what it counts (`LayoutNodeIndex`,
  `PrLayoutStyleIndex`), used in signatures and fields. An array whose index is not simply a position says what
  indexes it: a ` // by <domain>` comment, a type that names the index, or a word that every reader knows (byte, code
  point, glyph, slot). `lint.py` checks the comment (`index-domain`).
- **The standard library and the system** are wrapped by core. Only the core source that wraps a facility includes its
  header, and `lint.py` records which source that is. `stdbool.h`, `stddef.h` and `stdint.h` are used directly. The
  tests, and the harness headers that they include, are held to the same rule: there are many of them, and a standard
  header such as `<string>` costs more to compile than most tests do. A function that reads a list takes a `Span`, and
  its caller passes a local array, which converts to one.
  Anything the library does to the host on its own initiative, such as allocating memory or reporting a failure, goes
  through the platform ([platform.md](platform.md)). That means its default backends, unless the build binds its own
  functions at compile time.
- **Compiler attributes** are spelled through core's macros in `pr/base.h`, which give each compiler its own spelling.
  Only that header writes an attribute directly (`lint.py`, `compiler-attribute`).
- **Threads and const.** In `lib/core` and the libraries that depend on it, a `const` operation is safe on a shared
  object. `lib/dom`, and each document library built on it, is single-threaded per agent ([dom.md](dom.md#threads)). A
  declaration that departs from its library's default says so.
- **One name, one type.** `PrX` and `pr_impl::X` are the same type. The only internal declaration that uses a public
  type's name is the base of its handle (`struct PrX : pr_impl::X {}`). A public enum, structure or typedef is used as
  it is. An internal type with a different meaning gets a different name.
- **Handles.** A handle is a C opaque type whose base is the internal type, as in `struct PrSketch : Sketch {};`. It is
  declared once the internal type is complete, after the `pr_impl` block of the source or internal header that defines
  the type. Creation allocates the opaque type, so a handle converts to its internal type, and the object is returned
  as its handle without a cast. If `PR_HANDLE_UPCAST` relates two handles, their internal types inherit the same way:
  a derived internal type has its base's opaque type as its base, declared in a header after the base's header. A
  downcast is a `static_cast` after a check of the object's kind. Internal code passes the internal type. The
  exception is a library that allocates every object as its handle and hands objects out routinely, such as `lib/dom`.
  There, internal code passes the handle.
- **Backends.** An interface declares a struct of function pointers for each kind of object that a backend
  implements, in its backend header, `pr/<tag>_backend.h`. Only backends include it, and the public header declares
  the same types as opaque handles. The struct holds only function pointers, one for each operation, so a call loads
  its function from the object itself. Each is named for the function that it implements, with an `Fn` suffix
  (`readFn` implements `prNetStreamRead`), so that the backend's own fields keep their natural names. An object that
  exists in large numbers may point to a shared table instead.
  A backend's state derives from the struct and holds everything else, including what the interface's functions read,
  such as a callback or the object's parent. The backend reaches its state with a `static_cast`. The interface keeps
  no state of its own: its functions check arguments, handle null members and dispatch, so a backend implements only
  the operations it supports. Code that several backends share, such as a cache, lives in `backends/shared`, and each
  backend embeds its state. A backend's constructor is
  `pr<Interface><Backend>Create`, in the backend's own header, and takes whatever only that backend needs. An
  interface is replaced whole. A host that wants to combine implementations, such as one image codec for PNG and
  another for everything else, writes a backend that dispatches to them.
- **Choosing a backend.** An interface with backends lists them in an enum, `Pr<Interface>Backend`, whose first member
  is `Default` and last is `Count`. It provides `pr<Interface>BackendFromName`, `pr<Interface>BackendName` (lowercase,
  as on a command line) and `pr<Interface>BackendIsBuilt`. Its generic constructor, `pr<Interface>Create`, takes the
  backend as its first parameter, followed by the parameters of a backend's own constructor. Options that every
  backend shares replace a backend's own options. It fails as the backend's constructor would. For a backend that the
  build does not include, it reports NotSupported or returns null. `Default` is the platform's own backend, or the
  first one the build includes.

## Reference

An illustrative module, `lib/sketch` (not in the repo), showing the shape of a public header and its source.

`lib/sketch/include/pr/sketch.h`:

```c
// Sketches: strokes recorded from pointer input.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/geom.h"
#include "pr/status.h"
#include "pr/raw_string.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Brushes

// The tip that strokes are drawn with. A brush is immutable and reference counted.
typedef struct PrSketchBrush PrSketchBrush;

// Creates a round brush of `diameter`, which must be finite and greater than zero.
const PrSketchBrush* prSketchBrushCreate(float diameter);

// Adds or releases a reference. The last release frees the brush.
void prSketchBrushRetain(const PrSketchBrush* brush);
void prSketchBrushRelease(const PrSketchBrush* PR_NULLABLE brush);

// ---------------------------------------------------------------------------------------------------------------------
// Sketches

// Strokes recorded in order.
typedef struct PrSketch PrSketch;

// How a stroke's points are joined.
enum {
    PrSketchJoin_Line,      // straight segments
    PrSketchJoin_Smooth,    // a curve through every point
    PrSketchJoin_Round,     // straight segments with each corner rounded to the brush's diameter, so that strokes
                            // drawn at any speed meet at the same corners
};
typedef uint8_t PrSketchJoin;

// The fields of a PrSketchStroke that override the sketch's values.
enum {
    PrSketchOverrideFlag_Width = 1u << 0,
    PrSketchOverrideFlag_Tint = 1u << 1,
    PrSketchOverrideFlag_All = PrSketchOverrideFlag_Width | PrSketchOverrideFlag_Tint,
};
typedef uint8_t PrSketchOverrideFlags;

// The maximum number of points in one stroke.
enum { kPrSketchStrokeMaxPoints = 4096 };

// How a sketch joins and simplifies its strokes.
typedef struct PrSketchOptions {
    PrSketchJoin join;
    float tolerance;            // how far a simplified stroke may lie from its points, in pixels
} PrSketchOptions;

// Smooth joins, at a tolerance of a quarter pixel.
PrSketchOptions prSketchOptionsDefault(void);

// One recorded stroke.
typedef struct PrSketchStroke {
    const PrSketchBrush* PR_NULLABLE brush;
    PrSketchOverrideFlags overrides;
    float width;                // used if `overrides` has PrSketchOverrideFlag_Width
    PrColor tint;               // used if `overrides` has PrSketchOverrideFlag_Tint

    // The stroke's points, valid until the sketch changes.
    uint32_t pointCount;
    const PrVec2* PR_NULLABLE points;
} PrSketchStroke;

// Called with each stroke, in order. The stroke is valid only during the call.
typedef void (*PrSketchStrokeFn)(void* PR_NULLABLE context, const PrSketchStroke* stroke);

// Creates an empty sketch. A null `options` means the defaults.
PrSketch* prSketchCreate(const PrSketchOptions* PR_NULLABLE options);

// Frees the sketch.
void prSketchDestroy(PrSketch* PR_NULLABLE sketch);

// Empties the sketch, keeping its storage.
void prSketchClear(PrSketch* sketch);

// Begins a stroke at `point`, drawn with `brush`, or ends the open stroke. A stroke must not already be open when one
// begins.
void prSketchStrokeBegin(PrSketch* sketch, const PrSketchBrush* PR_NULLABLE brush, PrVec2 point);
void prSketchStrokeEnd(PrSketch* sketch);

// Appends the strokes described by `text`, in SVG path data. Returns Syntax if the text does not parse, and leaves the
// sketch unchanged in that case.
PrStatus prSketchAppendPathData(PrSketch* sketch, PrStringView text);

// Returns true if the sketch has no strokes.
bool prSketchIsEmpty(const PrSketch* sketch);

// The bounds of all points, or an empty rectangle if the sketch has no points.
PrRect prSketchBounds(const PrSketch* sketch);

// Writes up to `capacity` strokes to `out` and returns the number of strokes.
uint32_t prSketchStrokes(const PrSketch* sketch, PrSketchStroke* PR_NULLABLE out, uint32_t capacity);

// Calls `fn` with each stroke.
void prSketchEachStroke(const PrSketch* sketch, PrSketchStrokeFn fn, void* PR_NULLABLE context);

// The sketch as SVG path data. The caller releases it with prRawStringReset.
PrRawString prSketchPathData(const PrSketch* sketch);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
```

`lib/sketch/src/sketch.cpp`:

```cpp
// Sketches: strokes recorded from pointer input.

#include "pr/sketch.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/float.h"
#include "pr/path.h"

#include "sketch/src/simplify.h"

namespace pr_impl {

namespace {

// A round brush.
struct SketchBrush {
    Atomic<uint32_t> references;
    float diameter;
};

// A stroke as the sketch stores it. Its points are a range of the sketch's points.
struct StrokeRecord {
    const PrSketchBrush* brush;
    uint32_t start;
    uint32_t count;
};

// The strokes of a sketch, in the order they began.
struct Sketch {
    PrSketchOptions options;
    RawArray<StrokeRecord> strokes;
    RawArray<PrVec2> points;
    bool open;                  // true while the last stroke accepts new points

    // The bounds of all points, or empty. Adding a point can only grow the bounds, so they are updated as each point is
    // added instead of being recomputed.
    PrRect bounds;
};

// The smallest simplification tolerance. Below it, simplification keeps every point.
constexpr float kMinTolerance = 1e-3f;

// Returns the tolerance that simplifies a stroke for `options`. A smooth join fits a curve through the remaining
// points, so it keeps more of them.
float toleranceOf(const PrSketchOptions& options) {
    float tolerance = options.tolerance > kMinTolerance ? options.tolerance : kMinTolerance;
    switch (options.join) {
        case PrSketchJoin_Smooth:
            return tolerance / 2;
        case PrSketchJoin_Line:
        case PrSketchJoin_Round:
            break;
    }
    return tolerance;
}

PrSketchStroke strokeOf(const Sketch* sketch, StrokeRecord record) {
    PrSketchStroke stroke{};
    stroke.brush = record.brush;
    stroke.pointCount = record.count;
    stroke.points = sketch->points.data + record.start;
    return stroke;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrSketchBrush : SketchBrush {};
struct PrSketch : Sketch {};

const PrSketchBrush* prSketchBrushCreate(float diameter) {
    PR_ASSERT(diameter > 0 && isFiniteF32(diameter));
    PrSketchBrush* brush = allocZeroed<PrSketchBrush>();
    brush->references.storeRelaxed(1);
    brush->diameter = diameter;
    return brush;
}

// ...

PrSketchOptions prSketchOptionsDefault(void) {
    PrSketchOptions options{};
    options.join = PrSketchJoin_Smooth;
    options.tolerance = 0.25f;
    return options;
}

PrSketch* prSketchCreate(const PrSketchOptions* PR_NULLABLE options) {
    PrSketch* sketch = allocZeroed<PrSketch>();
    sketch->options = options != nullptr ? *options : prSketchOptionsDefault();
    return sketch;
}

void prSketchDestroy(PrSketch* PR_NULLABLE sketch) {
    if (sketch == nullptr)
        return;
    for (uint32_t i = 0; i < sketch->strokes.count; ++i)
        prSketchBrushRelease(sketch->strokes.data[i].brush);
    sketch->strokes.reset();
    sketch->points.reset();
    prMemoryFree(sketch);
}

void prSketchStrokeBegin(PrSketch* sketch, const PrSketchBrush* PR_NULLABLE brush, PrVec2 point) {
    PR_ASSERT(!sketch->open);
    if (brush != nullptr)
        prSketchBrushRetain(brush);
    sketch->strokes.push({brush, sketch->points.count, 1});
    sketch->points.push(point);
    sketch->open = true;
}

uint32_t prSketchStrokes(const PrSketch* sketch, PrSketchStroke* PR_NULLABLE out, uint32_t capacity) {
    for (uint32_t i = 0; i < sketch->strokes.count && i < capacity; ++i)
        out[i] = strokeOf(sketch, sketch->strokes.data[i]);
    return sketch->strokes.count;
}
```

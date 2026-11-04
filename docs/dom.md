# DOM

## Modules

- `lib/dom`: the node tree, attributes, collections and events.
- `lib/css`: style sheets, their values and the cascade, and the presentation of a document: its styles projected
  onto a layout tree, and CSSOM View, which reads it (see [css.md](css.md)).
- `lib/xml` and `lib/svg` depend only on `dom` and `css`, so their documents work without HTML. `lib/mathml` is MathML's
  element model. It gives `css` the presentational hints of MathML's attributes and a box provider for its elements
  (see [css.md](css.md#mathml)). It depends on `dom`, `css` and `layout`, and HTML and SVG documents enable it.
- `lib/html`: parsing, element behavior and browsing contexts. It depends on the others and integrates them through
  hooks, and nothing depends on `html`.
- Resource loading and script evaluation belong to the host.

## Web interfaces in C

The C API follows Web IDL closely on purpose, so that a web developer finds each interface and member where they
expect it. It is written by hand, not generated, and departs from IDL where C does better: an enumeration in place of
a closed set of strings, and a `PrStringView` in place of a copied string if the bytes outlive the call.
`pr/css_view.h` is the CSSOM View module (element geometry and scrolling), which is unrelated to the HTML view that
presents a document.

- A handle has the same name as its engine type (`PrDomElement` derives `DomElement`). If several interfaces share one
  engine type, they are named for their base: `PrCssStyleRule` and `PrCssPageRule` both derive `PrCssRule`, and a
  `Kind` enum (`prCssRuleKind`) names the most specific interface.
- Every engine object is allocated as its handle, so internal code passes handles (`PrDomNode*`), and an engine type
  derives the handle of its base (`DomElement : PrDomNode`).
- `As` upcasts (`PR_HANDLE_UPCAST`). `Query` downcasts and returns null if the object is not of that type. Null
  converts to null.
- A dictionary's `Default` returns the spec's defaults and takes the inputs that they depend on. A null dictionary
  means the defaults. An optional scalar with no default is a pointer.
- Trees that script builds (`PrCssValue`) are built node by node through an object that validates them as it goes.
  Their nodes are numbered in preorder, with a `Next` that skips a subtree.

## Nodes and trees

- **Attributes are not children.** An attribute's parent link names its owner element, and its sibling links form the
  attribute list. `domNodeNext` visits neither attributes nor template contents.
- **Adoption moves, never replaces.** Nodes, handles and references move with the subtree. Insertion from another
  document adopts the nodes before linking them.
- **Names are interned** as document atoms. Known element names are `PrDomTag`s generated from webref. Unknown names
  are `PrDomTag_Unknown` and keep their spelling.

A tree is the unit that lives and dies together, and its references are counted at its root. A node owns its children,
its attributes, the shadow root that it hosts and its template contents. An element owns its style sheet, a sheet owns
its rules, and a rule owns the rules of its block and the sheet of its import. The roots are a document, an unowned
node, a sheet without an owner node or rule, and a removed rule.

- A counted reference to any object in a tree counts toward the root's total. The tree is freed when the total reaches
  zero.
- Removing a subtree makes it a tree of its own, and its references move with it. A removed subtree with no references
  is freed immediately.
- Each node counts the references to what it owns, so moving references costs O(depth), never O(size).
- Objects name other objects in their own tree without counting (parent, owner node, parent rule). A detached tree holds
  a reference to its document, and the document holds none back.
- Ranges, collections, mutation records and events hold references. An algorithm that keeps an object across a step
  that could release it holds a reference.
- A node that the engine creates is a root with no references, and is owned by the code that made it until it is
  attached or referenced.
- A **hold** is a reference for pending engine work (a task, reaction, playback or construction). The document's tree
  callback is told of a node's first and last hold, which keeps script objects alive.
- A document holds no counted reference outside itself. The things that it keeps for another holder (a selection's
  range, a node's observers) are weak references, or lists that the holder leaves before releasing.
- A window lives as long as its browsing context and is not counted.

## Where steps run (`dom_node.cpp`)

- `link` / `unlink`: the invariants of every tree, connected or not (live ranges, slot assignment). These change state
  only, and are skipped unless the document uses the feature (`DomDocument::ranges`, `slots`, `options`).
- `runConnectionSteps`: runs after a change completes, for the elements that it connected or disconnected, and may run
  script or mutate. It runs only for elements whose `hasConnectionSteps` is true.
- `attributeChanged`: runs on every attribute write.
- `childrenChanged`: a parent's children-changed steps.

Parsers insert with `domAttach` (linking, without connection steps), and run themselves the connection steps that the
parse needs. A new kind of step goes where steps of its kind run, and also into the parsers if it must run while
parsing.

## Element state (`dom_element_state.h`)

Element state has one authority. Selectors, focus, forms and events query it, and nothing reads the attributes behind
it. The host supplies what only it knows (`PrHtmlHost::elementState`, `prHtmlDocumentSetHover`), and the engine
derives the rest (for example, the :hover state of ancestors). State that the specs store instead of deriving (option
selectedness) lives on the node.

## Threads

State scoped to an agent or event loop belongs to `PrDomAgent`, which is the event loop that runs its windows' tasks,
together with the threads that its documents' presentations run on. Nothing is per-thread or global. An object is used
by one thread at a time, and a document and everything reachable from it are used on its agent's thread. `const` means
that no change is observable by the caller, although owned caches may change. Objects that can be shared across threads
are documented as such, and are immutable once shared (for example, the result of `prCssSelectorsCreate`).

A restyle is the one exception. It reads the document from several threads while the agent's thread waits for them
(fork/join), and nothing changes the document meanwhile. What it reads (attributes, classes, the tree, and the states
of the pseudo-classes that it computes on those threads) changes no cache. The elements that a document's IDs name are
found before the threads start, so that the lookups made for states write nothing.

## JavaScript bindings

Script runs the web's own tests and pages: WPT, the tools and the browser example. The bindings are written for
QuickJS, and a second engine is not planned.

- Script reaches the engine only through the C API. Bindings are written by hand in each module's QuickJS layer,
  `lib/<module>/quickjs`, against public headers. A layer's own API is C as well. There are no `.js` sources.
- The binding converts arguments and results as Web IDL specifies, and calls C. Algorithms over engine state belong in
  the module. Only state that exists for script stays in the binding: identity, [SameObject] objects, and object graphs
  that script builds.
- Anything that names a script-reachable object holds a reference, and every script-reachable object is reference
  counted.
- Core's QuickJS toolkit (`pr/quickjs_binding.h`) makes handles into objects: their classes, identity, lifetimes and
  the call's scope. Web IDL's (`pr/webidl_quickjs.h`) defines interfaces on it as Web IDL specifies: interface objects,
  legacy platform objects, iterables, setlike and maplike, and the conversions of arguments. An object's class is the
  most specific class that describes its handle. A module adds a classifier to the classes that it derives from (HTML
  adds one on `Element`).
- If C names an object without counting, script objects keep it alive: a node's object keeps its parent's object
  alive (`jsGroupJoin`). A parent's object holds only the child objects that have state, or that hold such objects. A
  tree with no such state is freed by reference counting alone. The engine reports moves through the tree callback,
  and holds pin objects (`jsPin`).
- A handle with a slot stores its object there (`JsClass::slot`, `prDomEventTargetBinding`). Other objects
  are found in a table.
- Setlike and maplike interfaces (`JsCollection`) keep a backing `Set` or `Map`, refilled from the C object, which owns
  the entries.
- Failures throw what the spec names for that member: a `DOMException` mapped from `PrStatus`, or a `TypeError` or
  `RangeError`. The mapping is per member. For example, `NotAllowed` becomes a HierarchyRequestError from `insertRule`
  and a NotAllowedError from `replaceSync`. `DOMException` lives in `lib/webidl`.
- HTML's layer runs a browsing context's scripts (`pr/html_quickjs.h`). An engine wraps the host that an application
  gives a browsing context, makes a realm for it and for each of its iframes, and defines in each realm the DOM's and
  HTML's bindings and those that the application lists, such as SVG's (`prJsSvgBind`). Which bindings a program has is
  which layers it links.

The mutation benchmark guards the ownership rules: `kept KB` must stay flat as `--runs` grows.

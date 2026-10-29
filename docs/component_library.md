# Writing a component library

A component library turns `lib/layout` and `lib/ui` into widgets: buttons, lists, menus, the parts of a design
system. Studio Kit (`lib/studio/kit`) is one, and this page uses it as the example. The rules here make a library
cheap to run every frame, easy to call, and able to build one tree together with other libraries. Each rule is
something that the substrate supports directly.

## A widget is a function

A widget takes its parent, its content and its style, and returns the node that it made:

```c
PrLayoutNode prKitButton(PrKit* kit, PrLayoutNode parent, PrKitButton button, PrKitButtonStyle style);
```

- The content holds what the call shows and binds: the widget's key first, then its text, its values and the pointers
  that it writes. The style holds the few choices that the design system offers for the widget's look, such as a
  variant or a size. A widget with no choices takes no style.
- Both are passed by value, and zero is the default in every field, so a C caller writes a compound literal and a C++
  caller a braced list. A style is usually a constant that an application names once and passes to many calls.
- A library is opinionated. Each widget takes the parameters that its design calls for and no more. Anything else is
  the caller's restyle of the node that the widget returns, which the library supports for placement
  (`prKitPlace`). A container's style places it, because laying out its children is what a container is for.

## Containers and items

A compound widget is a container and the items that the caller creates under it: a tab strip and its tabs, a list
view and its rows, a menu and its items. No content carries an array of items, and no call writes an array of nodes.

- The container provides what its items share as a context on its node (`prUiProvide`), and each item finds it from
  its parent (`prUiContext`). A list view provides its window and the node its rows go under; a popover provides what
  closes it, which any node inside it finds.
- An item is created under its container itself. An item created under anything else is a contract violation, which
  the library asserts.
- What a container computes for the caller after its items exist, such as a list's selection edit or a tree's fold,
  stays in its context, and a query on the container's node reads it once the items are created.

## Events are queried on nodes

What happened to a widget in a round is asked of the node that it returned. lib/ui answers activation
(`prUiActivated`), and a library marks its own events as signals on the node (`prUiSignal`, `prUiSignaled`).
Studio Kit's queries read both: `prKitActivated`, `prKitClosed`, `prKitRemoved`. An event that carries values
writes them through an out parameter (`prKitMoved(kit, strip, &from, &to)`).

## State: bound, kept or chosen

- A value that a widget edits, such as a flag or a number, is bound: the content holds a pointer, which the widget
  reads as it creates its nodes and writes in the round that edits it.
- A choice among items is not bound. Each item says whether it is selected, and reports its activation, so the
  caller's value keeps its own type.
- State that the caller does not bind is kept on the node by the library (`prUiState`), under the library's owner and
  a key. A popover's open flag and a group's disclosure are kept this way.

## Scope follows the tree

What a container sets for everything inside it is inherited from parent to child as nodes are created, so it holds
whatever order the round creates them in. A disabled container disables its subtree (`prUiIsDisabled`), and a
widget's own content can disable it alone. A library keeps no stack of scopes in call order.

## Keys and identity

A node with key 0 is matched by its parent and its place among its parent's unkeyed children of its kind, which suits
content that does not move. A keyed node is matched by its key wherever a round creates it, which is what rows that
reorder and panels that come and go need.

- A key is unique in the tree. A library makes one from a base and a local name or index (`prUiKey`, `prUiKeyIndex`).
  The base is identified by its own key if it has one, and otherwise by its node, which layout keeps while it matches
  the node. Items of a container make their keys under the container, so two containers never share one, whichever
  libraries made them.
- A key given twice in a frame is a caller's error. Layout creates the second node without the key and reports both
  nodes after the commit (`prLayoutTreeDuplicateKeys`), and lib/ui outlines both in the next frame.
- A keyed node is where layout's matching aligns again after a node comes or goes. A library that caches by position
  in the round aligns there too.

## Records

A node's record names one group of each kind (layout.md, Style records). A library makes groups from small recipes of
its own and lets layout store them, so that what a frame costs does not grow with the number of properties layout has.

- A group recipe is a small struct of plain data that writes one kind of group. Its type and bytes identify it. A part
  recipe is the group recipes of one node.
- The context finds a group by its recipe (`prUiGroup`). On a miss, the recipe makes the group, and layout creates it
  without hashing it (`prLayoutBoxStyleCreate` and the others). The context holds a reference to each group, and
  releases a group that no lookup has used for a while once no record names it (`prLayoutGroupReferences`).
- A memo keyed by the lookup's place in the round keeps the part recipe of the last frame and its record
  (`prUiMemoFind`, `prUiMemoKeep`). The place is the slot that the round's next creation continues
  (`prLayoutTreeFrameCursor`). An unchanged widget costs one comparison of bytes, and the memo needs no identity of its
  own: it is aligned wherever layout's matching is.
- The groups and the memo belong to the context, so every library on it shares them, and the context ages them by its
  rounds. A recipe names its owner, the object whose values it reads, such as a theme, so that two libraries' recipes
  never meet. A library releases what its owner made when those values change (`prUiForget`).
- A library written in C++ declares its recipes as types, and makes a part's record with `uiStyle` (`pr/ui_records.h`,
  lib/ui's C++ layer).
- A value that changes in every round, such as a fade, makes a group in each round that it changes. A value that
  holds still is found by its recipe again.

## Libraries side by side

An application composes libraries in one tree: a kit's widget under another library's node, and another library's
widget under a kit's node. So a library owns nothing that another library needs, and needs no call that only the
application could make.

- A library needs no call to begin a round. What it keeps from round to round lives on nodes (`prUiState`) or in the
  context's records, which the context ages.
- A library does not need to own the root. The application creates the root, as any library's widget or as a plain
  element, and a library's widgets work under it.
- What floats is created under the context's overlay (`prUiOverlay`), whichever library makes it, so that a menu
  opened from one library's panel floats over every library's widgets.
- A bound control reports its edit gestures to the context (`prUiEdit`). The application sets a sink on a node
  (`prUiSetEditSink`), and the gestures of every library's controls under that node reach it, so that one undo
  history covers them all.

The editor example composes Studio Kit with a HUD library of its own (`examples/editor/hud.h`). Its HUD panels hold
kit widgets, a kit menu opens from a HUD panel, kit tooltips anchor to HUD switches, and one edit sink counts the
gestures of both libraries.

## Looks are data

The theme holds the design system's values: its palette, its text roles with the tone and wrap that each takes by
default, and its sizes. A widget names a look, and the library computes the look's colors at the widget's state from
the node's input, crossfading between states where the look eases.

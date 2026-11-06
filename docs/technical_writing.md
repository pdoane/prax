# Technical writing

Comments and docs are technical writing for engineers who know the field. Write them the way a good technical editor
would demand: clear, precise, plain English. [conventions.md](conventions.md) says what a comment must cover. This file
says how it should read.

## The standard

Good technical writing is clear, precise and to the point. A short, dense sentence is fine if its meaning is plain. A
sentence fails if the reader cannot tell what it is about, or has to decode it. Padding fails too: every word should
carry something.

Test every sentence by reading it aloud. If you would stumble, or a colleague would ask "meaning what?", rewrite it.

- **One idea per sentence.** Subject, verb, object, in that order. Split a sentence that chains clauses with colons,
  semicolons or a run of "which", "so that" and "as".
- **Ordinary words in their ordinary order.**
- **Name things concretely.** Say "the node's parent", not "what the node hangs from". Use the field's terms and the
  names in the code. Do not invent a paraphrase or a metaphor for an established term.
- **Prefer verbs.** "Sorts the edges by weight", not "performs the sorting of edges by weight". Use the active voice.
- **State things directly.** Say what a thing is before comparing it to anything ("as a canvas does"). A comparison
  works as an example after the direct statement.
- **Use "if" for a condition and "when" for a time.**
- **Avoid stacked possessives.** "The last round's build's claims" is hard to parse. Rewrite it as "the claims of the
  previous build".
- **Make each pronoun's referent obvious.** If "it" could mean two things, use the noun.
- **Cut what carries nothing.** "Note that", "in order to", "basically", and restatements of the name or the type.

## What a comment says

Most declarations need one short sentence, and many need none. A comment says what the name and the type do not:
units, ranges, ownership, lifetime, threading, cost, invariants, and the reason for anything surprising.

- **Say what and why, not how.** Describe responsibilities, invariants and intent. Do not restate the steps of the
  implementation. The code already says them, and a comment that repeats them goes stale with the next change. If a
  step is not obvious, say why it is there.
- **Do not lean on the name.** `float width; // a stroke's` adds nothing. Say what the field holds, or leave the
  comment out.
- **Give threading, ownership, lifetime and cost a sentence each**, so that a reader can find them.
- **Do not narrate the history.** A comment describes the code as it is meant to be, not how it got there or what an
  earlier version did.

## Patterns to fix

Each pair is a real comment from this repository and its rewrite.

**A puzzle.** The original chains clauses and leaves out what they refer to, so it has to be decoded.

> Returns the repeats a press of `button` the node holds crossed in the round: the first `delay` seconds after the
> press, then one each `interval`.

> Returns the number of auto-repeat ticks in this round while the node holds `button` down. The first tick comes
> `delay` seconds after the press, and then one every `interval` seconds.

**Abstract nouns in place of the things themselves.**

> A node the round continues keeps its handle, so a query names the node input was routed to with the handle the
> build holds.

> A node that persists from the previous frame keeps its handle. Input routed to that handle still reaches the node.

**Stacked possessives.**

> // the keys the last round's build claimed, which this round's key presses route by

> // keys claimed by the previous build, used to route key presses in this round

**"Where" for "if".**

> The cursor the last round resolved, which a host shows: an arrow where no node requested one.

> The cursor for the host to show, as resolved by the last round. It is an arrow if no node requested a cursor.

**Comparison in place of a statement.**

> // A button's record, its fill `fill` where it is given, as a crossfade's is.

> // Returns the record for a button. A non-null `fill` replaces the variant's fill color, as in a crossfade between
> // states.

**A chain of clauses.**

> A layered layout puts nodes that do not link to each other in one layer, so an LR subgraph's unlinked nodes stand in
> a column: what such a subgraph's author wants is usually the row that the parent's direction gives them.

> A layered layout puts unlinked nodes in the same layer. In an LR subgraph, that stacks them in a column. Authors
> usually want a row, which the parent's direction gives.

## Grouping

A comment on its own line applies to every declaration after it, up to the next blank line, at file scope and among
fields alike. Check each such comment against everything in its group:

```cpp
// The number of rows.       <-- bad: the comment claims `columns` too
uint32_t rows;
uint32_t columns;
```

Fix it in one of three ways: end the group with a blank line after the declaration the comment describes, move the
comment to a trailing comment on that declaration, or rewrite the comment so that it describes the whole group.

## Laying out a structure's comments

A structure's comments are read together, so lay them out as a whole. Each field takes one of two forms:

- **A trailing comment** for a short note on one field: a unit, a range, a sentinel. Trailing comments in a structure
  start at one shared column.
- **A group comment** on its own line, left-aligned with the fields, for longer prose or for a note that covers several
  fields. A blank line ends the group.

Choose the form for each field by what it needs to say, and keep the structure readable as one piece: short notes
trailing, longer explanations above their fields, related fields grouped. When you edit one field's comment, look at
the whole structure again. If a trailing comment grows past the line limit, the fix is not to turn that one comment
into a group comment and leave the rest as they were. Shorten it, or reconsider the layout of the structure. The same
holds for any edit: change a comment so that it is consistent with its neighbors, not just correct on its own.

## As you work

Write comments to this standard as you write the code, not in a later pass. Before you finish a change, reread each
comment and doc that you wrote or touched beside the code it describes, and rewrite any sentence that falls short.

- **Keep comments true.** When you change code, check the comments that describe it and the comments elsewhere that
  cite it. Lint flags a cited name that no longer exists (`dangling-name`), but not a claim about behavior that
  changed.
- **A contract leads the code.** A comment that states a contract, such as a function's promise in a public header,
  says what the code is meant to do. The implementation may lag it while a feature is built. That gap is a missing
  feature, not a stale comment.
- **An explanation follows the code.** If a comment that explains an implementation disagrees with the code, fix the
  comment.
- **Cite specifications** in the form `css-display-4/#the-display-properties`.
- **Do not edit generated files.** Change the generator's comment templates instead.

`./dev.sh prose` checks a change that touches only comments. It confirms that the code is unchanged, rewraps the
comments, and reports what lint finds.

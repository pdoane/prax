# Technical writing

This guide is for prose passes: sessions whose only job is to rewrite the comments and documentation written since the
last pass. [conventions.md](conventions.md) says what a comment must contain. This file says how it should read.

Code written in the course of other work drifts toward a compressed, private style that is hard to read. A prose pass
fixes that drift. It changes no behavior and no code.

## The process

1. Pick the files: everything changed since the last pass, or the module you were given.
2. Rewrite the comments in each file, and the docs that describe it.
3. Run `./dev.sh prose [path...]`. It checks that the code is token for token what it was at HEAD once comments are
   removed, rewraps the comments, and reports what lint finds in those files. Fix every problem it reports.
4. Read each rewritten comment once more beside its declaration.
5. Check what a comment says about other files. A comment that cites another declaration, another module's behavior or
   a document drifts when that other code changes, and nothing in the file shows it. Open what it cites and confirm
   that the claim still holds. Lint flags a cited name that no longer exists (`dangling-name`), but not a claim about
   behavior that changed.

A pass that finds a bug or a misleading name reports it and leaves the code alone.

A comment that states a contract -- what a declaration promises its callers, above all in a public header -- describes
what the code is meant to do. The implementation may lag it while a feature is being built, and that gap is a missing
feature, not a stale comment. Where such a comment and the code disagree, the pass keeps the comment and reports the
difference.

A comment that explains how an implementation works -- its steps, their order, what a field holds, what a sweep frees --
describes the code as it is. Where it disagrees with the code, it is stale, and the pass corrects it. So is any comment
that names a parameter or field that does not exist, describes a different function, or sits above the wrong
declaration. Where it is unclear which kind a comment is, keep it and report it.

## What a reader needs

The reader is an engineer who knows the field and is reading this code for the first time. They read a comment to
learn what a declaration is for, what it promises, and what it requires of them. Write so that each comment can be
read on its own, quickly, without decoding.

- **Write full sentences with a subject and a verb.** A sentence that starts with a verb (`Creates`, `Returns true
  if`) is fine for a function, because the function is the subject. A field may be a noun phrase.
- **Keep "that" and "which".** Do not drop the relative pronoun to save a word.
- **One possessive per noun phrase.** "The round's input" is fine. "The last round's build's claims" is not.
- **Say "if" for a condition and "when" for a time.** Do not use "where" for either.
- **State things directly.** Do not describe a thing by comparison ("as a crossfade's is", "as a canvas does") when
  you can say what it is. A comparison is fine as an example after the direct statement.
- **Split long contracts into short sentences, and paragraphs if needed.** Put threading, ownership, lifetime and cost
  each in a sentence of its own.
- **Use the field's terms**, and the names the code uses. Do not invent a paraphrase for an established term.
- **Prefer the plain word.** "Number of" over "the count of the", "uses" over "takes" when nothing is taken, "is
  freed" over "goes".
- **Say what and why, not how.** Describe responsibilities, invariants and intent. Do not restate the steps of an
  implementation in prose: the code already says them, and a comment that repeats them goes stale with the next change.
  Where a step is not obvious, say why it is there.
- **Do not lean on the name.** A trailing comment such as `// a stroke's` adds nothing. Say what the field holds, or
  leave the comment out.

## Patterns to fix

Each pair is a real comment from this repository and its rewrite.

**Dropped relative pronouns and stacked clauses.**

> Returns the repeats a press of `button` the node holds crossed in the round: the first `delay` seconds after the
> press, then one each `interval`.

> Returns how many auto-repeat ticks occurred in the current round while the node held a press of `button`. The first
> tick occurs `delay` seconds after the press, and each later tick `interval` seconds after the previous one.

**A sentence that must be decoded.**

> A node the round continues keeps its handle, so a query names the node input was routed to with the handle the
> build holds.

> A node that the frame matches to a committed node keeps its handle, so the handle the build holds is the same handle
> that input was routed to.

**Stacked possessives.**

> // the keys the last round's build claimed, which this round's key presses route by

> // keys claimed during the previous round's build; key presses in this round are routed by them

**"Where" for "if".**

> The cursor the last round resolved, which a host shows: an arrow where no node requested one.

> Returns the arrow if no node requested a cursor.

**A noun phrase that should be a sentence.**

> // What a frame runs.

> // Parameters for one frame.

**Comparison in place of a statement.**

> // A button's record, its fill `fill` where it is given, as a crossfade's is.

> // Returns the record for a button. If `fill` is not null, it replaces the variant's fill color, as during a
> // crossfade between states.

## Grouping

A comment on its own line applies to every declaration after it, up to the next blank line, at file scope and among
fields alike. A pass checks each such comment against everything in its group:

```cpp
// The number of rows.       <-- bad: the comment claims `columns` too
uint32_t rows;
uint32_t columns;
```

Fix it in one of three ways: end the group with a blank line after the declaration the comment describes, move the
comment to a trailing comment on that declaration, or rewrite the comment so that it describes the whole group.

## What to keep

- Every fact. A rewrite says everything the original said, and nothing it did not. If the original is ambiguous, read
  the code and write what it does.
- Spec citations, in the form `css-display-4/#the-display-properties`.
- The rules in [conventions.md](conventions.md): what a comment leads with, the scope of a comment up to the next blank
  line, no copies of sets defined elsewhere, and citing specifications rather than other implementations.
- Comments that are already clear. A pass is not a reason to reword good prose.

## What not to do

- Do not change code, names or whitespace in code. `./dev.sh prose` rejects any change it sees.
- Do not keep a comment that only narrates the code. Remove it, or replace it with the reason the code is written
  that way.
- Do not add comments to declarations that had none, unless the declaration cannot be used correctly without one.
- Do not pad. A longer comment is better only if it is easier to read. Cut words that carry nothing.
- Do not edit generated files. Change the generator's comment templates instead.

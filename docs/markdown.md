# Markdown

`lib/markdown` parses Markdown into a document that a client walks. It depends on `lib/core`, `lib/unicode` (case
folding, case mapping and general categories), `lib/html_entity` (named character references) and `lib/tex` (math as
MathML, [tex.md](tex.md)), and on nothing that lays out or renders. `lib/html` builds a DOM from a document
([html.md](html.md#markdown)).

## Which behavior

[behavior.md](behavior.md) applies, with the weights that Markdown's sources call for.

- **CommonMark 0.31.2** is the authority for the core syntax. Its examples are a suite, which the engine passes byte for
  byte, and an implementation that departs from it is not evidence.
- **GFM** extends an older CommonMark. Its extensions follow cmark-gfm, which GitHub runs, and where GFM's core and
  current CommonMark disagree, CommonMark wins. The cases are the suite's errata.
- **Extensions with no specification** follow what GitHub renders, since READMEs, issues and the Markdown that language
  models write follow it: footnotes, alerts, heading ids and math. pandoc's subscripts and superscripts, and `==`
  highlights, are options for content that uses them.

`tools/suite/markdown/markdown_features.txt` rules on each extension that the engine has considered, with its reason.

Where the specifications leave the mechanics open, such as the order in which emphasis is resolved, the engine follows
the reference implementations' algorithms, because content relies on their results.

## Documents

- **The source arrives whole or in pieces.** A piece may end anywhere, including inside a code point or between the CR
  and LF of a line ending. Once the source ends, the document is the one that parsing the source whole gives.
- **Top-level blocks are final or tentative.** A block is final once a complete line after it closes it, and it never
  changes again, except as a late reference resolves. The tentative blocks, from the first open child of the root
  onward, are parsed again from their first line whenever the source grows. A line without a line ending closes no
  block, since the rest of the line can change what it is.
- **A line shows as it arrives only if its ending cannot change what it shows.** A last line without a line ending
  that is text is parsed as it arrives, which a reader sees as typing. One that may still become something that shows
  otherwise waits for its line ending: a line that starts with `[`, which may be a link reference definition that shows
  nothing, or with `<`, and a line that is only a marker so far, as `--` may be a setext underline. Otherwise a stream
  would show the line and then take it back, and the page would grow and shrink under the reader.
- **A parse again starts at the root.** The first tentative block started a child of the root, so parsing from its line
  with no open blocks gives what the whole parse gives. A final block parses alone the same way, which is how a late
  reference builds it again.
- **Late references.** A link reference definition can follow the links that use it. A block records the labels that
  no final definition resolved, and a definition that becomes final, or that tentative blocks gain or lose, builds again
  only the blocks that looked its label up.
- **Changes are splices.** A client takes the changes since it last read the blocks, each a range of blocks replaced by
  a range of blocks, and applies them in order. A block built again for a late reference is one block replaced by one.
  A client that cannot take back what it wrote walks each block once it is final, and only late references are lost to
  it.
- **Passes over the whole document** resolve what depends on more than one block: footnote numbers, which count
  footnotes in the order of their first references, and heading ids, which number the slugs that earlier headings took.
  Their results are read by the walk, so a block whose numbers change is reported as changed without being parsed
  again.
- **Front matter**, a leading block between `---` lines, keeps every block tentative while a source that starts with
  `---` has no closing line, since the lines after the opening fence may yet be its lines.

## Walks and HTML

- A walk calls back with each step of the top-level blocks that it covers: entering a node, leaving it, and text. The
  tree is private, so its representation can change without changing the clients.
- The paragraphs of a tight list's items are not walked, and their inlines are the items' children, as they are in
  the HTML.
- Footnote definitions are walked apart from the blocks, in the order of their numbers, and a reference that no
  definition has is walked as the text that it was written as.
- HTML is a sequence of events: start tags with their attributes, end tags, text and raw HTML. HTML text is one client
  of them, and the DOM builder is another, so the two give the same tree. The text is what the specifications'
  examples give.
- Math is MathML. `lib/tex` writes a `<math>` from the TeX, annotated with its source, and its elements are events
  like any others, so the DOM builder makes them MathML elements. Each formula is set alone, so a block's HTML depends
  only on the block. A macro that one formula defines does not reach the next, and each formula's equations are
  numbered from 1. A client that typesets TeX itself walks the document, whose Math nodes hold the TeX. A fenced code
  block whose info string is `math` is display math, as GitHub renders it, and is a MathBlock node.

## Cost

- Parsing is linear in the source. Each search that can fail for many openers remembers where it failed: closing
  backtick strings, the ends of raw HTML constructs and quoted attribute values, and closing dollar signs. cmark-gfm's
  pathological inputs are cases of the gfm suite, each with a deadline.
- A piece of source costs the parse of the tentative blocks, so a stream of many blocks costs little per piece, and one
  block that grows for its whole length, such as a long list, is parsed again each time.

## Testing

| suite | corpus |
| --- | --- |
| `commonmark` | CommonMark's spec.txt |
| `gfm` | GFM's spec.txt, with each example's extensions; cmark-gfm's extensions, regression and smart punctuation tests; its pathological inputs |
| `markdown-dom` | both suites' examples, as the DOM that the bridge builds compared with the HTML parser's tree of lib/markdown's HTML |

Every example is also parsed a byte at a time, and must give the same HTML both at its end and as the blocks that its
changes report. `./dev.sh markdown compare` sets the engine's HTML beside cmark-gfm's on any Markdown, which finds what
the examples miss. A difference is a lead rather than a failure, since CommonMark 0.31.2 changed some of what GFM 0.29
says.

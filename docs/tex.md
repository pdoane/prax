# TeX math

`lib/tex` reads formulas in the TeX language that LaTeX and amsmath define, and that MathJax, KaTeX and Temml read on
the web, and writes them as MathML Core (`pr/tex.h`). It depends only on `lib/core`. `lib/markdown` writes its math with
it ([markdown.md](markdown.md)).

## Which behavior

[behavior.md](behavior.md) applies. The authority is what TeX sets: The TeXbook's Appendix G for the math, and the
amsmath and LaTeX sources for their commands and environments. The engine's MathML layout follows MathML Core, so the
MathML carries what Core needs to set the formula as TeX does, and leaves out what Core's defaults already give.

The output is MathML that a MathML engine typesets natively. It is MathML Core where Core suffices, and MathML 4 where
TeX needs more, such as `<mpadded>`'s pseudo-units for laps, a negative `<mspace>` width for negative space,
`<menclose>` for frames, and the attributes of tables. CSS is written only where MathML has no form, as for the fonts of
text mode, bold operators, a frame of a color of its own, and what \class and \style ask for.

- **Spacing.** The atoms of each list take TeX's classes, and a Bin atom that TeX treats as Ord becomes Ord (rules 5 and
  6). Each pair of adjacent atoms takes the space that TeX's table gives their classes, and script styles drop the
  spaces that TeX drops. The space becomes an operator's `lspace` or `rspace`, written only where it differs from the
  operator dictionary's value for the form that Core gives the operator. Between two atoms that are not operators, such
  as a fraction and a letter, it is an `<mspace>`. The dictionary is MathML Core's, which `tools/gen/gen_mathml.py`
  generates for `lib/tex` and `lib/mathml` alike from its pinned tables, so the two cannot disagree on what is written
  out.
- **Styles.** A formula's styles are TeX's: display, text, script and scriptscript, as `displaystyle` and
  `scriptlevel`, which Core's defaults for fractions and scripts already follow. Core expresses the cramped styles with
  `math-shift`.
- **Limits.** An operator whose limits TeX sets above and below in display style, such as \sum and \lim, is an
  `<munderover>` whose operator has `movablelimits`, so that layout moves them to scripts in other styles. \limits and
  \nolimits fix the choice.
- **Delimiters.** \left and \right stretch. \big and its kin, and the delimiters of \binom and \genfrac, are the
  sizes of Computer Modern's delimiter variants that TeX selects, as `minsize` and `maxsize`. A delimiter without
  \left is not stretchy.
- **Accents.** TeX's accents, such as \hat and \vec, are accent overscripts, in the font's accent forms: \vec is
  U+20D7, the small arrow of TeX's vector accent. The rules, braces and arrows that TeX sets clear of the base, such as
  \overline, \underbrace and \overrightarrow, are over- and underscripts that are not accents. MathML Core places them
  by the font's overbar and underbar gaps, and they keep the base's size.
- **Fonts.** Alphabets such as \mathbb and \mathbf are Unicode's Mathematical Alphanumeric Symbols, with the letters
  that Unicode encodes elsewhere, such as ℝ. `mathvariant` is used only for `normal`, as Core defines it. \mathcal and
  \mathscr share Unicode's script alphabet. \mathscr's capitals carry U+FE01, the standardized variation selector for
  roundhand. \mathcal's are the plain capitals, which a math font draws as chancery. They carry no selector, because a
  math font that does not list U+FE00, as New Computer Modern Math does not, would send a capital with it to a fallback
  face.
- **Tables.** amsmath's alignments, matrices, cases and arrays, and amscd's diagrams, are `<mtable>`s. MathML 4's table
  attributes carry their column alignment, the space between columns and rows, and their rules. Every table states its
  spacing, even a table of one cell, so that it takes MathML 4's model, which has no space outside the outer cells,
  rather than the padding that MathML Core gives every cell. An array's padding outside its first and last columns is
  its frame's spacing or an `<mpadded>`'s. Each row has a strut, as TeX's rows do, so that rows of short content keep
  the baselineskip, and alignments add \jot between rows. In display math, align and flalign are as wide as the line.
  The space that their pairs of columns leave goes to columns of width `fit`, around and between the pairs for align
  and only between them for flalign. A table of tags is as wide as the
  line too, with its columns centered between two such columns, so that the tags sit at the margin.
- **Equations.** equation, align, gather, multline, flalign, alignat and eqnarray number their equations in display
  math, as amsmath does. Each row, or multline's last line, takes the next number of the document's counter, unless
  \notag or \nonumber exempts it or \tag replaces it. Their starred forms take \tag and number nothing. A row's tag is
  the label of an `<mlabeledtr>` in a table as wide as the line. \tag where nothing is numbered, in inline math or in
  aligned, is amsmath's "\tag not allowed here". \label names the tag of the equation that holds it, and \ref and
  \eqref write the tag of a label that the document defines by the end of their own formula, or LaTeX's ?? for a label
  that it does not.
- **Errors.** Nothing fails a formula. A construct that does not parse, or a control sequence that nothing defines,
  is an `<merror>` that holds its source, and the rest of the formula is set around it.
- **Macros.** \newcommand, \def and their kin define macros for the rest of the formula. \def's parameters may be
  delimited, as TeX matches them.
- **Documents.** A document (`PrTexDocument`) holds the state that formulas share, as the formulas of a LaTeX document
  do. It holds macro definitions, which a formula's \gdef adds to, the equation counter and the labels. A formula
  without one is a document of its own, whose equations are numbered from 1.

## Testing

`lib/tex/tests` checks the MathML of each construct exactly, including the spacing that TeX gives it.

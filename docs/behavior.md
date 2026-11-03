# Which behavior

This document describes how the engine decides what to do if a standard is a draft, has been revised, or leaves a case
open: CSS since CSS 2, the CSSOM, HTML and the DOM, SVG, and Lottie. No single source answers the question. The
specifications, the implementations, the issues and the tests each say something, and each can be wrong for the
engine. A decision is researched, judged, and discussed with the developer.

## The sources

- **Specifications**, and their drafts, say where a standard is going, but a draft is only a draft. It holds features
  that no implementation intends to ship, text that lags behind a resolution, and text that no implementation follows.
- **Implementations** show what content relies on: the browsers for the web, and for Lottie, After Effects, which
  authors the files, and the players that the files are made for, lottie-web foremost. An implementation may be out of
  date in a part that it has not revisited, and may intend to change. Agreement among all of them is strong evidence.
  A single implementation is weak evidence.
- **Issues** (w3c/csswg-drafts, whatwg, w3c/svgwg, and the Lottie specification's) are often the best record of the
  long-term direction: what was resolved, what an implementation said it will change, and why a specification says
  what it does. A resolution can be newer than both the specification's text and every implementation. What carries
  weight is a working group's resolution, the engines' developers agreeing in an issue, and an engine's developers
  saying what their engine will do. An open issue on its own is a report, a question or a proposal, which anyone can
  open: until it is resolved, it leaves the specification's text in force, including a proposal to drop a feature.
- **Tests** (WPT, and the SVG and Lottie suites) are another source of data. Some record what was settled, some are
  tentative or experimental, and any of them may lag behind in either direction.

## The judgement

- **Generally the implementations.** If they agree, the engine follows them, unless the issues show that the behavior
  is meant to change.
- **Toward the specification if they disagree**, read together with its issues. The engine prefers the behavior that
  the specification and its resolutions point to, and that some implementation already ships, over the behavior that
  the other implementations keep.
- **Performance counts.** A behavior that is complex or costly for little benefit may be skipped or simplified, with
  a stated justification. The engine serves an immediate-mode client as well as documents.
- **Useful and cheap is in scope** if a specification defines it, even though no major implementation has it yet.
- **A decision says why.** Each point records what the specifications, the implementations, the issues and the tests
  say, what the engine does, and the reason. Decisions are summarized for the developer, who needs to weigh in only on
  a case that the rules do not settle, or on a departure from what they give, which comes with a recommendation: for
  example, every browser does something that costs a lot for little benefit, and the engine skips it.

## Reading the sources

- Specifications are read verbatim, from their source: `./dev.sh spec <name> [pattern]` for the CSS drafts and the
  WHATWG standards. A tool that summarizes a page is not a source.
- Browsers are measured with `./dev.sh probe <page>`, and with `--screenshot` for what geometry cannot show. A
  screenshot is read at a scale at which the detail in question is unambiguous.
- Issues are read whole, with their comments and the minutes they link.

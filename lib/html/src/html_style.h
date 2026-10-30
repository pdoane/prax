// The user agent's style sheets for HTML documents
//
// html/#rendering - https://html.spec.whatwg.org/multipage/rendering.html
// css-lists-3 - https://drafts.csswg.org/css-lists-3/
// css-ui-4 - https://drafts.csswg.org/css-ui-4/
//
// The sheets hold every rule of html/#rendering that applies at the user agent's level. The rules that apply as
// presentational hints map attributes in code (css_hints.h). The rules are written for the cascade's index of rules: a
// selector names a type, a class or an attribute of its subject wherever it can, so that an element checks only the
// rules that can match it. Type selectors select HTML elements alone.
//
// A widget's look comes from the rules for every appearance, which hold html/#form-controls and the styles of the
// widget's own box, and from sheets for its native and base appearances (css-ui-4/#appearance-switching,
// PrCssSheetAppearance). A devolvable widget's own box has the same styles in its native and primitive appearances, as
// css-ui-4 asks. So an author who restates the native values devolves the widget without changing it. The native sheet
// styles the parts of devolvable widgets, and non-devolvable widgets whole. The native look is the same on every
// platform. Its boxes have the sizes that web content expects of each control, so that a page lays out as its author
// saw it.
//
// Links and mark take the system colors that follow the used color scheme (LinkText, VisitedText, ActiveText, Mark and
// MarkText), where html/#rendering gives fixed colors, which are those system colors' light values. A fixed color is
// unreadable on a dark scheme, and whatwg/html#12761 proposes the system colors for the standard.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The rules that apply to every document.
constexpr char kHtmlStyle[] = "@namespace \"http://www.w3.org/1999/xhtml\";\n"
    // html/#hidden-elements
    "area, base, basefont, datalist, head, link, meta, noembed, noframes, param, rp, script, style, template,"
    " title { display: none; }\n"
    "[hidden]:not([hidden=until-found i]):not(embed) { display: none; }\n"
    "[hidden=until-found i]:not(embed) { content-visibility: hidden; }\n"
    "embed[hidden] { display: inline; height: 0; width: 0; }\n"
    "input[type=hidden i] { display: none !important; }\n"
    "@media (scripting) { noscript { display: none !important; } }\n"
    // html/#the-page. A body has margins of 8px if no attribute gives them.
    "html, body { display: block; }\n"
    "body { margin: 8px; }\n"
    // html/#flow-content-3
    "address, blockquote, center, dialog, div, figure, figcaption, footer, form, header, hr, legend, listing, main,"
    " p, plaintext, pre, search, xmp { display: block; }\n"
    "blockquote, figure, listing, p, plaintext, pre, xmp { margin-block: 1em; }\n"
    "blockquote, figure { margin-inline: 40px; }\n"
    "address { font-style: italic; }\n"
    "listing, plaintext, pre, xmp { font-family: monospace; white-space: pre; }\n"
    "dialog:not([open]) { display: none; }\n"
    "dialog { position: absolute; inset-inline-start: 0; inset-inline-end: 0; width: fit-content;"
    " height: fit-content; margin: auto; border: solid; padding: 1em; background-color: Canvas; color: CanvasText; }\n"
    "dialog:modal { position: fixed; overflow: auto; inset-block: 0; max-width: calc(100% - 6px - 2em);"
    " max-height: calc(100% - 6px - 2em); }\n"
    "dialog::backdrop { background: rgba(0, 0, 0, 0.1); }\n"
    "[popover]:not(:popover-open):not(dialog[open]) { display: none; }\n"
    "dialog:popover-open { display: block; }\n"
    "[popover] { position: fixed; inset: 0; width: fit-content; height: fit-content; margin: auto; border: solid;"
    " padding: 0.25em; overflow: auto; color: CanvasText; background-color: Canvas; }\n"
    ":popover-open::backdrop { position: fixed; inset: 0; pointer-events: none !important;"
    " background-color: transparent; }\n"
    "slot { display: contents; }\n"
    // html/#phrasing-content-3
    "cite, dfn, em, i, var { font-style: italic; }\n"
    "b, strong { font-weight: bolder; }\n"
    "code, kbd, samp, tt { font-family: monospace; }\n"
    "big { font-size: larger; }\n"
    "small { font-size: smaller; }\n"
    "sub { vertical-align: sub; }\n"
    "sup { vertical-align: super; }\n"
    "sub, sup { line-height: normal; font-size: smaller; }\n"
    "ruby { display: ruby; }\n"
    "rt { display: ruby-text; }\n"
    ":link { color: LinkText; }\n"
    ":visited { color: VisitedText; }\n"
    ":link:active, :visited:active { color: ActiveText; }\n"
    ":link, :visited { text-decoration: underline; cursor: pointer; }\n"
    ":focus-visible { outline: auto; }\n"
    "mark { background: Mark; color: MarkText; }\n"
    "abbr[title], acronym[title] { text-decoration: dotted underline; }\n"
    "ins, u { text-decoration: underline; }\n"
    "del, s, strike { text-decoration: line-through; }\n"
    "q::before { content: open-quote; }\n"
    "q::after { content: close-quote; }\n"
    "br { display-outside: newline; }\n"
    "nobr { white-space: nowrap; }\n"
    "wbr { display-outside: break-opportunity; }\n"
    "nobr wbr { white-space: normal; }\n"
    // html/#bidi-rendering
    "[dir]:dir(ltr), bdi:dir(ltr), input[type=tel i]:dir(ltr) { direction: ltr; }\n"
    "[dir]:dir(rtl), bdi:dir(rtl) { direction: rtl; }\n"
    "address, blockquote, center, div, figure, figcaption, footer, form, header, hr, legend, listing, main, p,"
    " plaintext, pre, summary, xmp, article, aside, h1, h2, h3, h4, h5, h6, hgroup, nav, search, section, table,"
    " caption, colgroup, col, thead, tbody, tfoot, tr, td, th, dir, dd, dl, dt, menu, ol, ul, li, bdi, output,"
    " [dir=ltr i], [dir=rtl i], [dir=auto i] { unicode-bidi: isolate; }\n"
    "bdo, bdo[dir] { unicode-bidi: isolate-override; }\n"
    "input[dir=auto i]:is([type=search i], [type=tel i], [type=url i], [type=email i]), textarea[dir=auto i],"
    " pre[dir=auto i] { unicode-bidi: plaintext; }\n"
    // html/#sections-and-headings, with each heading's type for :heading
    "article, aside, h1, h2, h3, h4, h5, h6, hgroup, nav, section { display: block; }\n"
    "h1, h2, h3, h4, h5, h6 { font-weight: bold; }\n"
    "h1 { margin-block: 0.67em; font-size: 2em; }\n"
    "h2 { margin-block: 0.83em; font-size: 1.5em; }\n"
    "h3 { margin-block: 1em; font-size: 1.17em; }\n"
    "h4 { margin-block: 1.33em; font-size: 1em; }\n"
    "h5 { margin-block: 1.67em; font-size: 0.83em; }\n"
    "h6 { margin-block: 2.33em; font-size: 0.67em; }\n"
    // html/#lists
    "dir, dd, dl, dt, menu, ol, ul { display: block; }\n"
    "li { display: list-item; text-align: match-parent; }\n"
    "dir, dl, menu, ol, ul { margin-block: 1em; }\n"
    ":is(dir, dl, menu, ol, ul) :is(dir, dl, menu, ol, ul) { margin-block: 0; }\n"
    "dd { margin-inline-start: 40px; }\n"
    "dir, menu, ol, ul { padding-inline-start: 40px; }\n"
    "ol, ul, menu { counter-reset: list-item; }\n"
    "ol { list-style-type: decimal; }\n"
    "dir, menu, ul { list-style-type: disc; }\n"
    ":is(dir, menu, ol, ul) :is(dir, menu, ul) { list-style-type: circle; }\n"
    ":is(dir, menu, ol, ul) :is(dir, menu, ol, ul) :is(dir, menu, ul) { list-style-type: square; }\n"
    // html/#tables-2
    "table { display: table; }\n"
    "caption { display: table-caption; }\n"
    "colgroup, colgroup[hidden] { display: table-column-group; }\n"
    "col, col[hidden] { display: table-column; }\n"
    "thead, thead[hidden] { display: table-header-group; }\n"
    "tbody, tbody[hidden] { display: table-row-group; }\n"
    "tfoot, tfoot[hidden] { display: table-footer-group; }\n"
    "tr, tr[hidden] { display: table-row; }\n"
    "td, th { display: table-cell; }\n"
    "colgroup[hidden], col[hidden], thead[hidden], tbody[hidden], tfoot[hidden], tr[hidden] { visibility: collapse; }\n"
    "table { box-sizing: border-box; border-spacing: 2px; border-collapse: separate; text-indent: initial; }\n"
    "td, th { padding: 1px; }\n"
    "th { font-weight: bold; }\n"
    "caption { text-align: center; }\n"
    "thead, tbody, tfoot, table > tr { vertical-align: middle; }\n"
    "tr, td, th { vertical-align: inherit; }\n"
    "thead, tbody, tfoot, tr { border-color: inherit; }\n"
    // html/#form-controls
    "input, button, textarea { letter-spacing: initial; word-spacing: initial; line-height: initial; }\n"
    "input, select, button, textarea { text-transform: initial; text-indent: initial; text-shadow: initial; appearance: auto; }\n"
    // Controls take a font of 10pt (13.333px), the size that web content expects of them.
    "input, select, button, textarea { font: 10pt system-ui, sans-serif; }\n"
    "input:not([type=range i], [type=checkbox i], [type=radio i]) { overflow: clip !important; overflow-clip-margin: 0 !important; }\n"
    "input[type=image i] { overflow-clip-margin: content-box !important; }\n"
    "input, select, textarea { text-align: initial; }\n"
    ":autofill { field-sizing: fixed !important; }\n"
    "input:is([type=reset i], [type=button i], [type=submit i]), button { text-align: center; }\n"
    "input, button { display: inline-block; }\n"
    "input[type=hidden i], input[type=file i], input[type=image i] { appearance: none; }\n"
    "input:is([type=radio i], [type=checkbox i], [type=reset i], [type=button i], [type=submit i], [type=color i],"
    " [type=search i]), select, button { box-sizing: border-box; }\n"
    "textarea { white-space: pre-wrap; }\n"
    "input:not([type=file i], [type=range i], [type=checkbox i], [type=radio i]) { align-content: unsafe center; }\n"
    "button, input:is([type=color i], [type=reset i], [type=button i], [type=submit i]) { align-content: center; }\n"
    "::file-selector-button { align-content: center; }\n"
    // The engine's look for a text field, a one-line text control (html/#the-input-element-as-a-text-entry-widget),
    // which html leaves to the user agent. Its ::field-content holds its text, one line tall at least, which is no
    // shorter than a normal line height. A type that is missing or not a known type is text. The types are tested in
    // :where(), so that the base look's rules for every input outrank the rule.
    "input:where(:not([type=hidden i], [type=checkbox i], [type=radio i], [type=file i], [type=submit i],"
    " [type=image i], [type=reset i], [type=button i], [type=range i], [type=color i], [type=date i], [type=month i],"
    " [type=week i], [type=time i], [type=datetime-local i], [type=number i])) { border: 1px solid ButtonBorder;"
    " padding: 2px 3px; background-color: Field; color: FieldText; }\n"
    "input::field-content { display: block; min-block-size: 1lh; }\n"
    // html/#the-hr-element-2
    "hr { color: gray; border-style: inset; border-width: 1px; margin-block: 0.5em; margin-inline: auto; overflow: hidden; }\n"
    // html/#the-fieldset-and-legend-elements
    "fieldset { display: block; margin-inline: 2px; border: groove 2px ThreeDFace; padding-block: 0.35em 0.625em;"
    " padding-inline: 0.75em; min-inline-size: min-content; }\n"
    "legend { padding-inline: 2px; }\n"
    "legend[align=left i] { justify-self: left; }\n"
    "legend[align=center i] { justify-self: center; }\n"
    "legend[align=right i] { justify-self: right; }\n"
    // html/#the-meter-element-2, html/#the-progress-element-2
    "meter, progress { appearance: auto; }\n"
    // html/#embedded-content-rendering-rules, html/#images-3
    "iframe { border: 2px inset; }\n"
    "video { object-fit: contain; }\n"
    "img:is([sizes=\"auto\" i], [sizes^=\"auto,\" i]) { contain: size !important; contain-intrinsic-size: 300px 150px; }\n"
    // html/#the-details-and-summary-elements
    "details, summary { display: block; }\n"
    "details > summary:first-of-type { display: list-item; counter-increment: list-item 0; list-style: disclosure-closed inside; }\n"
    "details[open] > summary:first-of-type { list-style-type: disclosure-open; }\n"
    // html/#the-marquee-element-2
    "marquee { display: inline-block; text-align: initial; overflow: hidden !important; }\n"
    // html/#the-select-element-2, for a select rendered with native or primitive appearance
    "option { display: block; }\n"
    "optgroup { display: block; font-weight: bolder; }\n"
    "select { letter-spacing: initial; word-spacing: initial; line-height: initial; }\n"
    "select:not(:-pr-list-box) { align-content: center; display: inline-block; }\n"
    // css-ui-4/#inertness, which asks the HTML sheet for this rule and which html/#rendering does not hold yet: an
    // element with the inert attribute (html/#the-inert-attribute) is inert through the interactivity property, which
    // its descendants inherit. The element stays inert whatever its author's rules say.
    "[inert] { interactivity: inert !important; }\n";

// The base look of the widgets rendered with their base appearance (css-forms-1/#basic-appearance-stylesheet): a widget
// takes its colors and font from the page, inside a border of its text color.
constexpr char kHtmlStyleBase[] = "@namespace \"http://www.w3.org/1999/xhtml\";\n"
    "input, textarea, button, meter, progress { color: inherit; font: inherit; box-sizing: border-box;"
    " background-color: transparent; }\n"
    "input:not([type=file i], [type=range i], [type=checkbox i][switch]), textarea, button {"
    " border: 1px solid currentColor; background-color: light-dark(transparent, rgb(255 255 255 / 4%)); }\n"
    "input { align-content: unsafe center; align-items: unsafe center; }\n";

// The rules for the markers of every element (css-lists-3/#marker-properties).
constexpr char kHtmlStyleMarkers[] =
    "::marker, ::before::marker, ::after::marker { unicode-bidi: isolate; font-variant-numeric: tabular-nums;"
    " white-space: pre; text-transform: none; }\n";

// The rules that apply while the document is in quirks mode (html/#quirks-mode).
constexpr char kHtmlStyleQuirks[] = "@namespace \"http://www.w3.org/1999/xhtml\";\n"
    "form { margin-block-end: 1em; }\n"
    "li { list-style-position: inside; }\n"
    "li :is(dir, menu, ol, ul) { list-style-position: outside; }\n"
    ":is(dir, menu, ol, ul) :is(dir, menu, ol, ul, li) { list-style-position: unset; }\n"
    "table { font-weight: initial; font-style: initial; font-variant: initial; font-size: initial;"
    " line-height: initial; white-space: initial; text-align: initial; }\n"
    "input:not([type=image i]), textarea { box-sizing: border-box; }\n"
    "img[align=left i] { margin-right: 3px; }\n"
    "img[align=right i] { margin-left: 3px; }\n";

// The rules that apply in an HTML document (dom/#html-document).
constexpr char kHtmlStyleHtmlDocument[] = "@namespace \"http://www.w3.org/1999/xhtml\";\n"
    "table > form, thead > form, tbody > form, tfoot > form, tr > form { display: none !important; }\n";

// The rules that apply in a document built from Markdown (pr/html_markdown.h): a column of text that is comfortable to
// read, in either color scheme. Colors are mixed from the scheme's system colors, so that they follow it.
constexpr char kHtmlStyleMarkdown[] = "@namespace \"http://www.w3.org/1999/xhtml\";\n"
    "html { color-scheme: light dark; }\n"
    "body { max-width: 48em; margin: 0 auto; padding: 1em 2em; font-family: sans-serif; line-height: 1.5; }\n"
    "h1, h2 { padding-block-end: 0.3em; border-block-end: 1px solid color-mix(in srgb, CanvasText 20%, Canvas); }\n"
    "h1, h2, h3, h4, h5, h6 { margin-block: 1.5em 0.75em; line-height: 1.25; }\n"
    "code { padding: 0.1em 0.3em; border-radius: 4px; font-size: 0.9em;"
    " background-color: color-mix(in srgb, CanvasText 8%, Canvas); }\n"
    "pre { padding: 1em; border-radius: 6px; overflow: auto; line-height: 1.4;"
    " background-color: color-mix(in srgb, CanvasText 8%, Canvas); }\n"
    "pre > code { padding: 0; font-size: inherit; background-color: transparent; }\n"
    "blockquote { margin-inline: 0; padding-inline: 1em; color: color-mix(in srgb, CanvasText 70%, Canvas);"
    " border-inline-start: 0.25em solid color-mix(in srgb, CanvasText 25%, Canvas); }\n"
    "table { border-collapse: collapse; }\n"
    "th, td { padding: 0.3em 0.8em; border: 1px solid color-mix(in srgb, CanvasText 20%, Canvas); }\n"
    "hr { border: none; border-block-start: 2px solid color-mix(in srgb, CanvasText 20%, Canvas); }\n"
    "img { max-width: 100%; }\n"
    // Alerts, in the colors that GitHub gives each kind, and math, whose TeX is text until it typesets.
    ".markdown-alert { margin-block: 1em; padding: 0.5em 1em; border-inline-start: 0.25em solid var(--alert); }\n"
    ".markdown-alert > :last-child { margin-block-end: 0; }\n"
    ".markdown-alert-title { margin-block-start: 0; font-weight: 600; color: var(--alert); }\n"
    ".markdown-alert-note { --alert: #4493f8; }\n"
    ".markdown-alert-tip { --alert: #3fb950; }\n"
    ".markdown-alert-important { --alert: #ab7df8; }\n"
    ".markdown-alert-warning { --alert: #d29922; }\n"
    ".markdown-alert-caution { --alert: #f85149; }\n"
    ".math { font-family: math, serif; }\n"
    ".math-display { display: block; margin-block: 1em; text-align: center; overflow-x: auto; }\n"
    ".footnotes { margin-block-start: 2em; padding-block-start: 1em; font-size: 0.875em;"
    " border-block-start: 1px solid color-mix(in srgb, CanvasText 20%, Canvas); }\n"
    "li > input[type=checkbox]:first-child { margin-inline: 0 0.4em; }\n";

} // namespace pr_impl

PR_ASSUME_NONNULL_END

// Select Elements
//
// html - https://html.spec.whatwg.org/multipage/form-elements.html
//
// The state that the specs store for options and selectedcontent elements. An option's selectedness and dirtiness are
// flags of its node (DomNodeFlag_OptionSelected, DomNodeFlag_OptionDirty). The option insertion and removing steps keep
// them current as a select's list of options changes, in every tree and not only in a document's tree. A
// selectedcontent element holds a copy of the selected option of its select. A document runs none of these steps until
// it has created an option or a selectedcontent element (DomDocument::options).

#pragma once

#include "pr/array.h"
#include "pr/dom.h"
#include "pr/text_buffer.h"

#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if the option is selected (html/#concept-option-selectedness).
inline bool domOptionSelected(const PrDomElement* option) {
    return (option->flags & DomNodeFlag_OptionSelected) != 0;
}

// Returns true if the option is disabled (html/#concept-option-disabled).
bool domOptionDisabled(const PrDomElement* option);

// Writes the option's value (html/#concept-option-value) to `out`, which it clears first: its value attribute, or else
// its text (html/#dom-option-text).
void domOptionValue(const PrDomElement* option, TextBuffer* out);

// Writes the option's text (html/#dom-option-text) to `out`, which it clears first: its descendant text outside script
// elements, with its ASCII whitespace stripped and collapsed.
void domOptionText(const PrDomElement* option, TextBuffer* out);

// Sets the option's selectedness and marks it dirty, as a script's change does, then applies the select's rules
// (html/#dom-option-selected).
void domOptionSetSelected(PrDomElement* option, bool selected);

// Returns the index of the first selected option of the select's list of options, or -1
// (html/#dom-select-selectedindex).
int32_t domSelectSelectedIndex(PrDomElement* select);

// Makes the option at `index` of the select's list of options the only selected option, and marks it dirty. An index
// that names no option leaves none selected (html/#dom-select-selectedindex).
void domSelectSetSelectedIndex(PrDomElement* select, int32_t index);

// Makes the first option whose value is `value` the only selected option, and marks it dirty. A value that no option
// has leaves none selected (html/#dom-select-value).
void domSelectSetValue(PrDomElement* select, PrStringView value);

// Appends the options of the select's list of options (html/#concept-select-option-list) to `out`, in tree order.
void domSelectOptions(PrDomElement* select, Array<PrDomElement* PR_NONNULL>* out);

// Returns true if the select renders as a list box, and false if it renders as a drop-down box
// (html/#the-select-element-2). A select renders as a list box if it has the multiple attribute or a display size
// greater than 1. The spec lets a select with the multiple attribute and a display size of 1 render either way. The
// engine renders it as a list box.
bool domSelectListBox(const PrDomElement* select);

// Returns the element's nearest ancestor select, or null if there is none
// (html/#option-element-nearest-ancestor-select).
PrDomElement* PR_NULLABLE domNearestAncestorSelect(const PrDomElement* element);

// Runs the option insertion steps (html/#the-option-element) for the options in the tree at `node`, which has been
// linked under its parent. For each option whose nearest ancestor select changed, runs the selectedness setting
// algorithm of both selects. Does nothing in a document that has created no option.
void domSelectLinked(PrDomNode* node);

// Runs the option removing steps for the options in the tree at `node`, which has been removed from `parent`.
void domSelectUnlinked(PrDomNode* node, PrDomNode* parent);

// Updates the option after its selected attribute was added or removed. `present` is true if the attribute was added.
// The option's selectedness follows the attribute unless the option is dirty (html/#concept-option-selectedness).
void domOptionSelectedAttributeChanged(PrDomElement* option, bool present);

// Clones `option` into the selectedcontent of its select if the option is selected
// (html/#maybe-clone-an-option-into-selectedcontent). A parser calls this when it pops `option` off its stack of open
// elements.
void domOptionPopped(PrDomElement* option);

// Replaces the contents of the select's selectedcontent with a clone of its first selected option, or empties it if no
// option is selected (html/#update-a-select's-selectedcontent).
void domSelectUpdateSelectedContent(PrDomElement* select);

// Runs the selectedcontent removing steps for the selectedcontent elements in the tree at `node`, which has been
// removed from `parent`. Returns the select whose selectedcontent the steps update, or null if there is none.
PrDomElement* PR_NULLABLE domSelectedContentRemoved(const PrDomNode* node, PrDomNode* parent);

// Runs the selectedcontent post-connection steps (html/#the-selectedcontent-element) for an HTML selectedcontent
// element that has been connected. Does nothing for any other element.
void domSelectedContentConnected(PrDomElement* element);

} // namespace pr_impl

PR_ASSUME_NONNULL_END

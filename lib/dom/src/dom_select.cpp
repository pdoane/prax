#include "dom/src/dom_select.h"

#include "pr/ascii.h"
#include "pr/integer.h"

#include "dom/src/dom_hooks.h"

namespace pr_impl {
namespace {

bool isMultiple(const PrDomElement* select) {
    return domHasAttribute(select, "multiple");
}

// html/#concept-select-size, for a select without the multiple attribute: returns its size attribute as a non-negative
// integer (html/#rules-for-parsing-non-negative-integers), or 1 if parsing fails.
uint64_t displaySize(const PrDomElement* select) {
    PrStringView size = domAttributeValue(select, "size");
    size_t at = 0;
    while (at < size.size && asciiIsSpace(size.data[at]))
        ++at;
    if (at < size.size && size.data[at] == '+')
        ++at;
    uint64_t value = 0;
    if (parseU64Prefix(size.data + at, size.size - at, &value) == 0)
        return 1;
    return value;
}

// Sets the option's selectedness, which restyles it through :checked.
void setSelected(PrDomElement* option, bool selected) {
    if (domOptionSelected(option) == selected)
        return;
    if (selected)
        option->flags |= DomNodeFlag_OptionSelected;
    else
        option->flags &= DomNodeFlags(~DomNodeFlag_OptionSelected);
    domNotifyStatesChanged(option);
}

// Returns the nearest ancestor select of `element`. Searches the element's ancestors up to `top`, then `above` and its
// ancestors. `above` and its ancestors are the ancestors of `top` if `top` is linked under `above`. Linking or
// unlinking `top` leaves the ancestors below it unchanged.
PrDomElement* PR_NULLABLE nearestSelect(const PrDomElement* element, const PrDomNode* PR_NULLABLE top,
                                        PrDomNode* PR_NULLABLE above) {
    bool optgroup = false;
    PrDomNode* at = element == top ? above : element->parent;
    while (at != nullptr) {
        PrDomElement* ancestor = domElementFromNode(at);
        if (ancestor != nullptr && ancestor->space == PrDomNamespace_Html) {
            switch (ancestor->tag) {
                case PrDomTag_Datalist:
                case PrDomTag_Hr:
                case PrDomTag_Option:
                    return nullptr;
                case PrDomTag_Optgroup:
                    if (optgroup)
                        return nullptr;
                    optgroup = true;
                    break;
                case PrDomTag_Select:
                    return ancestor;
                default:
                    break;
            }
        }
        at = at == top ? above : at->parent;
    }
    return nullptr;
}

// Calls `visit` with each option of the select's list of options (html/#concept-select-option-list), in tree order.
template <typename Visit>
void eachOption(PrDomElement* select, Visit visit) {
    PrDomNode* node = select->first;
    while (node != nullptr) {
        PrDomElement* element = domElementFromNode(node);
        bool skip = false;
        if (element != nullptr && element->space == PrDomNamespace_Html) {
            switch (element->tag) {
                case PrDomTag_Option:
                    visit(element);
                    skip = true;
                    break;
                case PrDomTag_Select:
                case PrDomTag_Hr:
                case PrDomTag_Datalist:
                    skip = true;
                    break;
                case PrDomTag_Optgroup:
                    // An optgroup inside another optgroup is skipped, together with its contents.
                    for (PrDomNode* up = node->parent; up != select; up = up->parent)
                        skip = skip || domIsHtml(domElementFromNode(up), PrDomTag_Optgroup);
                    break;
                default:
                    break;
            }
        }
        node = skip ? domNodeNextSkipping(select, node) : domNodeNext(select, node);
    }
}

// html/#selectedness-setting-algorithm
void resetSelectedness(PrDomElement* select) {
    if (isMultiple(select))
        return;
    uint32_t selected = 0;
    PrDomElement* last = nullptr;
    PrDomElement* firstEnabled = nullptr;
    eachOption(select, [&](PrDomElement* option) {
        if (domOptionSelected(option)) {
            ++selected;
            last = option;
        }
        if (firstEnabled == nullptr && !domOptionDisabled(option))
            firstEnabled = option;
    });
    if (selected == 0) {
        if (firstEnabled != nullptr && displaySize(select) == 1)
            setSelected(firstEnabled, true);
        return;
    }
    if (selected > 1)
        eachOption(select, [&](PrDomElement* option) { setSelected(option, option == last); });
}

// Applies the rule for a select without the multiple attribute (html/#the-select-element). An option in its list of
// options becomes the only selected option if its selectedness becomes true, or if it joins the list with a
// selectedness of true.
void selectOnly(PrDomElement* select, PrDomElement* option) {
    if (domOptionSelected(option) && !isMultiple(select))
        eachOption(select, [&](PrDomElement* other) { setSelected(other, other == option); });
}

// html/#update-an-option's-nearest-ancestor-select, for an option whose nearest ancestor select changed from `before`
// to `after`.
void selectChanged(PrDomElement* option, PrDomElement* PR_NULLABLE before,
                   PrDomElement* PR_NULLABLE after) {
    if (before == after)
        return;
    if (before != nullptr)
        resetSelectedness(before);
    if (after != nullptr) {
        selectOnly(after, option);
        resetSelectedness(after);
    }
}

bool contentDisabled(const PrDomElement* element) {
    return (element->flags & DomNodeFlag_SelectedContentDisabled) != 0;
}

// html/#select-enabled-selectedcontent
PrDomElement* PR_NULLABLE enabledSelectedContent(PrDomElement* select) {
    if (isMultiple(select))
        return nullptr;
    for (PrDomNode* node = select->first; node != nullptr; node = domNodeNext(select, node)) {
        PrDomElement* element = domElementFromNode(node);
        if (domIsHtml(element, PrDomTag_Selectedcontent))
            return contentDisabled(element) ? nullptr : element;
    }
    return nullptr;
}

// html/#clone-an-option-into-a-selectedcontent
void cloneInto(const PrDomElement* option, PrDomElement* selectedcontent) {
    PrDomDocumentFragment* fragment = domDocumentFragmentCreate(option->document);
    for (const PrDomNode* child = option->first; child != nullptr; child = child->next) {
        if (PrDomNode* clone = domClone(child, option->document, true))
            domInsert(fragment, clone, nullptr);
    }
    domReplaceAll(selectedcontent, fragment);
}

} // namespace

bool domOptionDisabled(const PrDomElement* option) {
    if (domHasAttribute(option, "disabled"))
        return true;
    for (const PrDomElement* at = domElementFromNode(option->parent); at != nullptr;
         at = domElementFromNode(at->parent)) {
        if (at->space != PrDomNamespace_Html)
            continue;
        switch (at->tag) {
            case PrDomTag_Select:
            case PrDomTag_Hr:
            case PrDomTag_Datalist:
            case PrDomTag_Option:
                return false;
            case PrDomTag_Optgroup:
                return domHasAttribute(at, "disabled");
            default:
                break;
        }
    }
    return false;
}

// The text is the option's descendant text outside script elements, with its ASCII whitespace stripped and collapsed.
void domOptionValue(const PrDomElement* option, TextBuffer* out) {
    textClear(out);
    PrStringView value = domAttributeValue(option, "value");
    if (value.data != nullptr) {
        textAppend(out, value);
        return;
    }
    bool space = false;
    for (const PrDomNode* node = domNodeNext(option, option); node != nullptr;) {
        const PrDomElement* element = domElementFromNode(node);
        if (element != nullptr && domIsScript(element)) {
            node = domNodeNextSkipping(option, node);
            continue;
        }
        if (domIsText(node)) {
            PrStringView data = prStringView(&static_cast<const PrDomCharacterData*>(node)->data);
            for (size_t i = 0; i < data.size; ++i) {
                if (asciiIsSpace(data.data[i])) {
                    space = true;
                    continue;
                }
                if (space && out->text.count != 0)
                    textAppendChar(out, ' ');
                space = false;
                textAppendChar(out, data.data[i]);
            }
        }
        node = domNodeNext(option, node);
    }
}

void domSelectOptions(PrDomElement* select, Array<PrDomElement* PR_NONNULL>* out) {
    eachOption(select, [&](PrDomElement* option) { out->push(option); });
}

bool domSelectListBox(const PrDomElement* select) {
    return isMultiple(select) || displaySize(select) > 1;
}

PrDomElement* PR_NULLABLE domNearestAncestorSelect(const PrDomElement* element) {
    return nearestSelect(element, nullptr, nullptr);
}

void domSelectLinked(PrDomNode* node) {
    if (!node->document->options)
        return;
    for (PrDomNode* at = node; at != nullptr; at = domNodeNext(node, at)) {
        PrDomElement* option = domElementFromNode(at);
        if (domIsHtml(option, PrDomTag_Option))
            selectChanged(option, nearestSelect(option, node, nullptr),
                          nearestSelect(option, node, node->parent));
    }
}

void domSelectUnlinked(PrDomNode* node, PrDomNode* parent) {
    if (!node->document->options)
        return;
    for (PrDomNode* at = node; at != nullptr; at = domNodeNext(node, at)) {
        PrDomElement* option = domElementFromNode(at);
        if (domIsHtml(option, PrDomTag_Option))
            selectChanged(option, nearestSelect(option, node, parent),
                          nearestSelect(option, node, nullptr));
    }
}

void domOptionSelectedAttributeChanged(PrDomElement* option, bool present) {
    if ((option->flags & DomNodeFlag_OptionDirty) != 0)
        return;
    setSelected(option, present);
    if (PrDomElement* select = present ? domNearestAncestorSelect(option) : nullptr)
        selectOnly(select, option);
}

void domOptionPopped(PrDomElement* option) {
    PrDomElement* select = domNearestAncestorSelect(option);
    if (select == nullptr || !domOptionSelected(option))
        return;
    if (PrDomElement* selectedcontent = enabledSelectedContent(select))
        cloneInto(option, selectedcontent);
}

void domSelectUpdateSelectedContent(PrDomElement* select) {
    PrDomElement* selectedcontent = enabledSelectedContent(select);
    if (selectedcontent == nullptr)
        return;
    PrDomElement* selected = nullptr;
    eachOption(select, [&](PrDomElement* option) {
        if (selected == nullptr && domOptionSelected(option))
            selected = option;
    });
    if (selected != nullptr)
        cloneInto(selected, selectedcontent);
    else
        domReplaceAll(selectedcontent, nullptr);
}

PrDomElement* PR_NULLABLE domSelectedContentRemoved(const PrDomNode* node, PrDomNode* parent) {
    if (!node->document->options)
        return nullptr;
    bool removed = false;
    for (const PrDomNode* at = node; at != nullptr && !removed; at = domNodeNext(node, at)) {
        const PrDomElement* element = domElementFromNode(at);
        if (!domIsHtml(element, PrDomTag_Selectedcontent) || contentDisabled(element))
            continue;
        removed = true;
        for (const PrDomNode* up = at->parent; up != nullptr; up = up->parent)
            removed = removed && !domIsHtml(domElementFromNode(up), PrDomTag_Select);
    }
    if (!removed)
        return nullptr;
    for (PrDomNode* at = parent; at != nullptr; at = at->parent) {
        if (PrDomElement* select = domElementFromNode(at); domIsHtml(select, PrDomTag_Select))
            return select;
    }
    return nullptr;
}

void domSelectedContentConnected(PrDomElement* element) {
    if (!domIsHtml(element, PrDomTag_Selectedcontent) || !domIsConnected(element))
        return;
    PrDomElement* nearest = nullptr;
    bool disabled = false;
    for (PrDomNode* at = element->parent; at != nullptr && !disabled; at = at->parent) {
        PrDomElement* ancestor = domElementFromNode(at);
        if (domIsHtml(ancestor, PrDomTag_Select)) {
            if (nearest != nullptr)
                disabled = true;
            else
                nearest = ancestor;
        } else if (domIsHtml(ancestor, PrDomTag_Option)
                   || domIsHtml(ancestor, PrDomTag_Selectedcontent)) {
            disabled = true;
        }
    }
    if (disabled)
        element->flags |= DomNodeFlag_SelectedContentDisabled;
    else
        element->flags &= DomNodeFlags(~DomNodeFlag_SelectedContentDisabled);
    if (disabled || nearest == nullptr || isMultiple(nearest))
        return;
    domSelectUpdateSelectedContent(nearest);
    // html/#clear-a-select's-non-primary-selectedcontent-elements
    bool passed = false;
    for (PrDomNode* node = nearest->first; node != nullptr; node = domNodeNext(nearest, node)) {
        PrDomElement* other = domElementFromNode(node);
        if (!domIsHtml(other, PrDomTag_Selectedcontent))
            continue;
        if (passed)
            domReplaceAll(other, nullptr);
        passed = true;
    }
}

} // namespace pr_impl

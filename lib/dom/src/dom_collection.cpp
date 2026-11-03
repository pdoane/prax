#include "dom/src/dom_collection.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/string_ops.h"

#include "dom/src/dom_control.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_select.h"

namespace pr_impl {
namespace {

// dom/#concept-getelementsbyclassname: returns true if the element has every class of `wanted`. `atoms` holds the atoms
// of `wanted`. Classes compare ASCII case-insensitively in a quirks mode document.
bool hasClasses(const PrDomElement* element, const Array<PrStringView>& wanted, const Array<Atom>& atoms, bool quirks) {
    if (element->classes == kDomClassesNone)
        return false;
    DomClassList classes = domClassesList(element->document, element->classes);
    for (uint32_t i = 0; i < wanted.count; ++i) {
        bool found = false;
        for (uint32_t j = 0; !found && j < classes.count; ++j) {
            if (quirks)
                found = prAsciiEqualsFold(domClassesName(element->document, classes.atoms[j]), wanted.data[i]);
            else
                found = classes.atoms[j] == atoms.data[i];
        }
        if (!found)
            return false;
    }
    return true;
}

// Returns the first element after `after`, among the descendants of the collection's root, that has the collection's
// classes. Returns null if none does, which is always the case if the collection names no class.
PrDomElement* PR_NULLABLE nextWithClasses(const PrDomHtmlCollection* collection, PrDomNode* after) {
    Array<PrStringView> wanted;
    Array<Atom> atoms;
    domOrderedSetParse(prStringView(&collection->text), &wanted);
    const PrDomDocument* document = collection->root->document;
    bool quirks = document->quirks == DomQuirks_Yes;
    // If no element of the document has a class, no element of the collection has it.
    bool possible = wanted.count != 0;
    for (uint32_t i = 0; i < wanted.count; ++i) {
        atoms.push(domClassesFind(document, wanted.data[i]));
        possible = possible && (quirks || atoms.data[i] != kAtomNone);
    }
    PrDomElement* found = nullptr;
    for (PrDomNode* node = domNodeNext(collection->root, after); possible && found == nullptr && node != nullptr;
            node = domNodeNext(collection->root, node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element != nullptr && hasClasses(element, wanted, atoms, quirks))
            found = element;
    }
    wanted.reset();
    atoms.reset();
    return found;
}

bool collectionMatches(const PrDomHtmlCollection* collection, const PrDomElement* element) {
    switch (collection->filter) {
        case DomCollectionFilter_Images:
            return domIsHtml(element, PrDomTag_Img);
        case DomCollectionFilter_Embeds:
            return domIsHtml(element, PrDomTag_Embed);
        case DomCollectionFilter_Links:
            return (domIsHtml(element, PrDomTag_A) || domIsHtml(element, PrDomTag_Area)) &&
                domAttribute(element, PrDomNamespace_None, "href") != nullptr;
        case DomCollectionFilter_Forms:
            return domIsHtml(element, PrDomTag_Form);
        case DomCollectionFilter_Scripts:
            return domIsHtml(element, PrDomTag_Script);
        case DomCollectionFilter_Anchors:
            return domIsHtml(element, PrDomTag_A) && domAttribute(element, PrDomNamespace_None, "name") != nullptr;
        case DomCollectionFilter_FormControls:
            return domIsListed(element) && domFormOwner(element) == collection->root &&
                !(domIsHtml(element, PrDomTag_Input) && domElementInputType(element) == PrDomInputType_Image);
        case DomCollectionFilter_FieldSetElements:
            return domIsListed(element);
        case DomCollectionFilter_MapAreas:
            return domIsHtml(element, PrDomTag_Area);
        case DomCollectionFilter_SelectOptions:
            return domIsHtml(element, PrDomTag_Option) && domNearestAncestorSelect(element) == collection->root;
        case DomCollectionFilter_SelectedOptions:
            return domIsHtml(element, PrDomTag_Option) && domOptionSelected(element) &&
                domNearestAncestorSelect(element) == collection->root;
        case DomCollectionFilter_DataListOptions:
            return domIsHtml(element, PrDomTag_Option);
        default:
            break;
    }
    PrStringView requested = prStringView(&collection->text);
    if (requested == "*")
        return true;
    PrStringView name = domElementName(element);
    if (!domIsHtmlInHtmlDocument(element))
        return name == requested;
    // dom/#concept-getelementsbytagname: an HTML element in an HTML document matches the requested name in ASCII
    // lowercase.
    return prAsciiEqualsLowered(name, requested);
}

// Returns the first child at or after `child` that is the HTML element `tag`, or also `other`, or null.
PrDomElement* PR_NULLABLE childFrom(PrDomNode* PR_NULLABLE child, PrDomTag tag, PrDomTag other) {
    for (; child != nullptr; child = child->next) {
        PrDomElement* element = domElementFromNode(child);
        if (domIsHtml(element, tag) || domIsHtml(element, other))
            return element;
    }
    return nullptr;
}

// The order of the rows of a table's children: 0 for a thead, 1 for a tbody or a row, 2 for a tfoot, and 3 for any
// other child, which holds no rows of the table.
uint32_t rowOrder(PrDomNode* child) {
    PrDomElement* element = domElementFromNode(child);
    if (domIsHtml(element, PrDomTag_Thead))
        return 0;
    if (domIsHtml(element, PrDomTag_Tbody) || domIsHtml(element, PrDomTag_Tr))
        return 1;
    return domIsHtml(element, PrDomTag_Tfoot) ? 2 : 3;
}

// Returns the row of `table` after `after`, which is the table for its first row (html/#dom-table-rows).
PrDomElement* PR_NULLABLE nextTableRow(PrDomNode* table, PrDomNode* after) {
    PrDomNode* from = table->first;
    uint32_t order = 0;
    if (after != table) {
        if (after->parent == table) {
            from = after->next;
            order = 1;
        } else {
            if (PrDomElement* row = childFrom(after->next, PrDomTag_Tr, PrDomTag_Tr))
                return row;
            from = after->parent->next;
            order = rowOrder(after->parent);
        }
    }
    for (; order < 3; ++order, from = table->first) {
        for (PrDomNode* child = from; child != nullptr; child = child->next) {
            if (rowOrder(child) != order)
                continue;
            if (domIsHtml(domElementFromNode(child), PrDomTag_Tr))
                return domElementFromNode(child);
            if (PrDomElement* row = childFrom(child->first, PrDomTag_Tr, PrDomTag_Tr))
                return row;
        }
    }
    return nullptr;
}

PrDomElement* PR_NULLABLE nextCollectionElement(const PrDomHtmlCollection* collection, PrDomNode* after) {
    PrDomNode* afterChild = after == collection->root ? collection->root->first : after->next;
    switch (collection->filter) {
        case DomCollectionFilter_TableRows: return nextTableRow(collection->root, after);
        case DomCollectionFilter_TableBodies: return childFrom(afterChild, PrDomTag_Tbody, PrDomTag_Tbody);
        case DomCollectionFilter_ChildRows: return childFrom(afterChild, PrDomTag_Tr, PrDomTag_Tr);
        case DomCollectionFilter_RowCells: return childFrom(afterChild, PrDomTag_Td, PrDomTag_Th);
        default: break;
    }
    if (collection->filter == DomCollectionFilter_Children) {
        for (PrDomNode* node = after == collection->root ? after->first : after->next; node != nullptr; node =
                node->next) {
            if (PrDomElement* element = domElementFromNode(node))
                return element;
        }
        return nullptr;
    }
    if (collection->filter == DomCollectionFilter_ClassNames)
        return nextWithClasses(collection, after);
    // A form's controls are in the form's tree, which the walk starts from the root of.
    PrDomNode* scope = collection->root;
    if (collection->filter == DomCollectionFilter_FormControls) {
        scope = domNodeRoot(collection->root);
        if (after == collection->root)
            after = scope;
    }
    for (PrDomNode* node = domNodeNext(scope, after); node != nullptr; node = domNodeNext(scope, node)) {
        if (PrDomElement* element = domElementFromNode(node)) {
            if (collectionMatches(collection, element))
                return element;
        }
    }
    return nullptr;
}

// A token set read from the attribute. Its views point into its own copy of the value.
struct TokenSet {
    PrString text;
    Array<PrStringView> tokens;

    void reset() {
        prStringReset(&text);
        tokens.reset();
    }

    uint32_t find(PrStringView token) const {
        for (uint32_t i = 0; i < tokens.count; ++i) {
            if (tokens.data[i] == token)
                return i;
        }
        return tokens.count;
    }
};

const PrDomAttr* PR_NULLABLE attributeOf(const PrDomTokenList* list) {
    return domAttribute(list->element, PrDomNamespace_None, prStringView(&list->name));
}

// Returns the attribute's value, or the empty string if the element has no such attribute.
PrStringView tokensText(const PrDomTokenList* list) {
    const PrDomAttr* attribute = attributeOf(list);
    return attribute != nullptr ? prStringView(&attribute->value) : "";
}

// Fills `set` in place, because its tokens are views of its text, whose bytes may be held inside the PrString.
void readTokens(const PrDomTokenList* list, TokenSet* set) {
    set->text = prStringCopy(tokensText(list));
    domOrderedSetParse(prStringView(&set->text), &set->tokens);
}

// dom/#concept-dtl-update. Leaves an element with no attribute and no tokens unchanged.
void update(PrDomTokenList* list, const TokenSet& set) {
    if (attributeOf(list) == nullptr && set.tokens.count == 0)
        return;
    PrString value = prStringEmpty();
    for (uint32_t i = 0; i < set.tokens.count; ++i) {
        if (i != 0)
            prStringAppend(&value, " ");
        prStringAppend(&value, set.tokens.data[i]);
    }
    domSetAttribute(list->element, PrDomNamespace_None, prStringView(&list->name), prStringView(&value));
    prStringReset(&value);
}

PrStatus validate(PrStringView token) {
    if (token.size == 0)
        return PrStatus_Syntax;
    for (size_t i = 0; i < token.size; ++i) {
        if (prAsciiIsSpace(token.data[i]))
            return PrStatus_InvalidData;
    }
    return PrStatus_Ok;
}

PrStatus validate(const PrStringView* PR_NULLABLE tokens, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        PrStatus status = validate(tokens[i]);
        if (status != PrStatus_Ok)
            return status;
    }
    return PrStatus_Ok;
}

// Returns the node of a live list after `after`, or its first node for null.
PrDomNode* PR_NULLABLE nextListNode(const PrDomNodeList* list, PrDomNode* PR_NULLABLE after) {
    PrDomNode* owner = list->parent;
    if (list->filter == DomNodeListFilter_Children)
        return after == nullptr ? owner->first : after->next;
    PrDomNode* root = domNodeRoot(owner);
    PrStringView name = prStringView(&list->name);
    for (PrDomNode* node = domNodeNext(root, after != nullptr ? after : root); node != nullptr; node = domNodeNext(root, node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element == nullptr)
            continue;
        bool named = domElementId(element) == name || domAttributeValue(element, "name") == name;
        switch (list->filter) {
            case DomNodeListFilter_Labels:
                if (domIsHtml(element, PrDomTag_Label) && domLabeledControl(element) == owner)
                    return element;
                break;
            case DomNodeListFilter_Radio:
                if (named && domIsListed(element) && domFormOwner(element) == owner &&
                        !(domIsHtml(element, PrDomTag_Input) && domElementInputType(element) == PrDomInputType_Image))
                    return element;
                break;
            default:
                if (named && domIsHtml(element, PrDomTag_Img) && domFormOwner(element) == owner)
                    return element;
                break;
        }
    }
    return nullptr;
}

// Returns the cache, emptied if the tree of `root` changed since the cache was filled.
DomCollectionCache& current(DomCollectionCache& cache, const PrDomNode* root) {
    if (cache.document != root->document || cache.version != root->document->treeVersion) {
        cache.document = root->document;
        cache.version = root->document->treeVersion;
        cache.length = kDomCollectionLengthNone;
        cache.node = nullptr;
        cache.index = 0;
    }
    return cache;
}

} // namespace

PrDomHtmlCollection* domHtmlCollectionCreate(PrDomNode* root, DomCollectionFilter filter, PrStringView text) {
    PrDomHtmlCollection* collection = alloc<PrDomHtmlCollection>();
    *collection = PrDomHtmlCollection{{root, prStringCopy(text), filter, {}}};
    domNodeRetain(root);
    return collection;
}

uint32_t domHtmlCollectionLength(const PrDomHtmlCollection* collection) {
    DomCollectionCache& cache = current(collection->cache, collection->root);
    if (cache.length != kDomCollectionLengthNone)
        return cache.length;
    uint32_t count = 0;
    for (PrDomElement* element = nextCollectionElement(collection, collection->root);
            element != nullptr; element = nextCollectionElement(collection, element))
        ++count;
    cache.length = count;
    return count;
}

PrDomElement* PR_NULLABLE domHtmlCollectionItem(const PrDomHtmlCollection* collection, uint32_t index) {
    DomCollectionCache& cache = current(collection->cache, collection->root);
    if (cache.length != kDomCollectionLengthNone && index >= cache.length)
        return nullptr;
    // Start from the last element reached if it is at or before `index`, and from the start otherwise.
    bool resume = cache.node != nullptr && cache.index <= index;
    uint32_t at = resume ? cache.index : 0;
    PrDomElement* element;
    if (resume)
        element = static_cast<PrDomElement*>(cache.node);
    else
        element = nextCollectionElement(collection, collection->root);
    for (; element != nullptr && at != index; ++at)
        element = nextCollectionElement(collection, element);
    if (element == nullptr) {
        cache.length = at;
        return nullptr;
    }
    cache.node = element;
    cache.index = index;
    return element;
}

PrDomElement* PR_NULLABLE domHtmlCollectionNamedItem(const PrDomHtmlCollection* collection, PrStringView name) {
    if (name.size == 0)
        return nullptr;
    for (PrDomElement* element = nextCollectionElement(collection, collection->root);
            element != nullptr; element = nextCollectionElement(collection, element)) {
        if (domElementId(element) == name || (element->space == PrDomNamespace_Html &&
                domAttributeValue(element, "name") == name))
            return element;
    }
    return nullptr;
}

void domHtmlCollectionNames(const PrDomHtmlCollection* collection, Array<PrStringView>* out) {
    uint32_t start = out->count;
    auto add = [&](PrStringView name) {
        if (name.size == 0)
            return;
        for (uint32_t i = start; i < out->count; ++i) {
            if (out->data[i] == name)
                return;
        }
        out->push(name);
    };
    for (PrDomElement* element = nextCollectionElement(collection, collection->root);
            element != nullptr; element = nextCollectionElement(collection, element)) {
        add(domElementId(element));
        if (element->space == PrDomNamespace_Html)
            add(domAttributeValue(element, "name"));
    }
}

void domHtmlCollectionDestroy(PrDomHtmlCollection* PR_NULLABLE collection) {
    if (collection == nullptr)
        return;
    domNodeRelease(collection->root);
    prStringReset(&collection->text);
    prMemoryFree(collection);
}

PrDomNodeList* domNodeListCreate(PrDomNode* PR_NULLABLE parent) {
    PrDomNodeList* list = allocZeroed<PrDomNodeList>();
    list->parent = parent;
    if (parent != nullptr)
        domNodeRetain(parent);
    return list;
}

PrDomNodeList* domNodeListCreateFiltered(PrDomNode* node, DomNodeListFilter filter, PrStringView name) {
    PrDomNodeList* list = domNodeListCreate(node);
    list->filter = filter;
    list->name = prStringCopy(name);
    return list;
}

void domNodeListAssign(PrDomNodeList* list, PrDomNode* const PR_NONNULL* PR_NULLABLE nodes, uint32_t count) {
    domNodeRetainEach(nodes, count);
    domNodeRelease(list->parent);
    list->parent = nullptr;
    domNodeReleaseEach(list->nodes.data, list->nodes.count);
    list->nodes.clear();
    list->nodes.append(nodes, count);
}

uint32_t domNodeListLength(const PrDomNodeList* list) {
    if (list->parent == nullptr)
        return list->nodes.count;
    DomCollectionCache& cache = current(list->cache, list->parent);
    if (cache.length != kDomCollectionLengthNone)
        return cache.length;
    uint32_t count = 0;
    for (PrDomNode* node = nextListNode(list, nullptr); node != nullptr; node = nextListNode(list, node))
        ++count;
    cache.length = count;
    return count;
}

PrDomNode* PR_NULLABLE domNodeListItem(const PrDomNodeList* list, uint32_t index) {
    if (list->parent == nullptr)
        return index < list->nodes.count ? list->nodes.data[index] : nullptr;
    DomCollectionCache& cache = current(list->cache, list->parent);
    if (cache.length != kDomCollectionLengthNone && index >= cache.length)
        return nullptr;
    bool resume = cache.node != nullptr && cache.index <= index;
    uint32_t at = resume ? cache.index : 0;
    PrDomNode* child = resume ? cache.node : nextListNode(list, nullptr);
    for (; child != nullptr && at != index; ++at)
        child = nextListNode(list, child);
    if (child == nullptr) {
        cache.length = at;
        return nullptr;
    }
    cache.node = child;
    cache.index = index;
    return child;
}

void domNodeListDestroy(PrDomNodeList* PR_NULLABLE list) {
    if (list == nullptr)
        return;
    domNodeRelease(list->parent);
    domNodeReleaseEach(list->nodes.data, list->nodes.count);
    list->nodes.reset();
    prStringReset(&list->name);
    prMemoryFree(list);
}

PrDomTokenList* domTokenListCreate(PrDomElement* element, PrStringView name) {
    PrDomTokenList* list = allocZeroed<PrDomTokenList>();
    list->element = element;
    list->name = prStringCopy(name);
    domNodeRetain(element);
    return list;
}

void domTokenListDestroy(PrDomTokenList* PR_NULLABLE list) {
    if (list == nullptr)
        return;
    domNodeRelease(list->element);
    prStringReset(&list->name);
    prMemoryFree(list);
}

uint32_t domTokenListLength(const PrDomTokenList* list) {
    Array<PrStringView> tokens;
    domOrderedSetParse(tokensText(list), &tokens);
    uint32_t count = tokens.count;
    tokens.reset();
    return count;
}

PrStringView domTokenListItem(const PrDomTokenList* list, uint32_t index) {
    Array<PrStringView> tokens;
    domOrderedSetParse(tokensText(list), &tokens);
    PrStringView item = index < tokens.count ? tokens.data[index] : PrStringView{};
    tokens.reset();
    return item;
}

bool domTokenListContains(const PrDomTokenList* list, PrStringView token) {
    Array<PrStringView> tokens;
    domOrderedSetParse(tokensText(list), &tokens);
    bool found = false;
    for (uint32_t i = 0; !found && i < tokens.count; ++i)
        found = tokens.data[i] == token;
    tokens.reset();
    return found;
}

PrStatus domTokenListAdd(PrDomTokenList* list, const PrStringView* PR_NULLABLE tokens, uint32_t count) {
    PrStatus status = validate(tokens, count);
    if (status != PrStatus_Ok)
        return status;
    TokenSet set;
    readTokens(list, &set);
    for (uint32_t i = 0; i < count; ++i) {
        if (set.find(tokens[i]) == set.tokens.count)
            set.tokens.push(tokens[i]);
    }
    update(list, set);
    set.reset();
    return PrStatus_Ok;
}

PrStatus domTokenListRemove(PrDomTokenList* list, const PrStringView* PR_NULLABLE tokens, uint32_t count) {
    PrStatus status = validate(tokens, count);
    if (status != PrStatus_Ok)
        return status;
    TokenSet set;
    readTokens(list, &set);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = set.find(tokens[i]);
        if (at < set.tokens.count)
            set.tokens.erase(at);
    }
    update(list, set);
    set.reset();
    return PrStatus_Ok;
}

PrStatus domTokenListToggle(PrDomTokenList* list, PrStringView token, const bool* PR_NULLABLE force, bool* out) {
    PrStatus status = validate(token);
    if (status != PrStatus_Ok)
        return status;
    TokenSet set;
    readTokens(list, &set);
    uint32_t at = set.find(token);
    bool present = at < set.tokens.count;
    if (present && (force == nullptr || !*force)) {
        set.tokens.erase(at);
        update(list, set);
        present = false;
    } else if (!present && (force == nullptr || *force)) {
        set.tokens.push(token);
        update(list, set);
        present = true;
    }
    set.reset();
    *out = present;
    return PrStatus_Ok;
}

PrStatus domTokenListReplace(PrDomTokenList* list, PrStringView token, PrStringView newToken, bool* out) {
    if (token.size == 0 || newToken.size == 0)
        return PrStatus_Syntax;
    PrStatus status = validate(token);
    if (status == PrStatus_Ok)
        status = validate(newToken);
    if (status != PrStatus_Ok)
        return status;
    TokenSet set;
    readTokens(list, &set);
    uint32_t at = set.find(token);
    *out = at < set.tokens.count;
    if (*out) {
        // infra/#set-replace: the first of the two tokens is replaced by the new token, and the other is removed.
        uint32_t other = set.find(newToken);
        uint32_t first = other < at ? other : at;
        set.tokens.data[first] = newToken;
        uint32_t second = first == at ? other : at;
        if (second < set.tokens.count && second != first)
            set.tokens.erase(second);
        update(list, set);
    }
    set.reset();
    return PrStatus_Ok;
}

PrStringView domTokenListValue(const PrDomTokenList* list) {
    return tokensText(list);
}

void domTokenListSetValue(PrDomTokenList* list, PrStringView value) {
    domSetAttribute(list->element, PrDomNamespace_None, prStringView(&list->name), value);
}

void domOrderedSetParse(PrStringView text, Array<PrStringView>* out) {
    size_t at = 0;
    while (at < text.size) {
        while (at < text.size && prAsciiIsSpace(text.data[at]))
            ++at;
        size_t start = at;
        while (at < text.size && !prAsciiIsSpace(text.data[at]))
            ++at;
        if (at == start)
            break;
        PrStringView token = {text.data + start, at - start};
        bool seen = false;
        for (uint32_t i = 0; !seen && i < out->count; ++i)
            seen = out->data[i] == token;
        if (!seen)
            out->push(token);
    }
}

} // namespace pr_impl

// Classes
//
// dom - https://dom.spec.whatwg.org/#concept-class

#include "dom/src/dom_class.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/string_ops.h"

#include "dom/src/dom_node.h"

namespace pr_impl {

uint32_t domClassesAdd(PrDomDocument* document, PrStringView text) {
    if (document->classes == nullptr)
        document->classes = allocZeroed<DomClasses>();
    DomClasses* classes = document->classes;
    if (classes->last != kDomClassesNone && atomTableText(&classes->texts, classes->last - 1) == text)
        return classes->last;
    Atom known = atomTableFind(&classes->texts, text);
    if (known != kAtomNone) {
        classes->last = known + 1;
        return known + 1;
    }

    uint32_t first = classes->atoms.count;
    for (size_t at = 0; at < text.size;) {
        if (prAsciiIsSpace(text.data[at])) {
            ++at;
            continue;
        }
        size_t start = at;
        at = prAsciiFindSpace(text, at);
        Atom atom = atomTableAdd(&classes->names, prStringSlice(text, start, at - start));
        bool seen = false;
        for (uint32_t i = first; !seen && i < classes->atoms.count; ++i)
            seen = classes->atoms.data[i] == atom;
        if (!seen)
            classes->atoms.push(atom);
    }
    if (classes->atoms.count == first)
        return kDomClassesNone;
    if (classes->starts.count == 0)
        classes->starts.push(0);
    Atom list = atomTableAdd(&classes->texts, text);
    PR_ASSERT(list + 1 == classes->starts.count);
    classes->starts.push(classes->atoms.count);
    classes->last = list + 1;
    return list + 1;
}

DomClassList domClassesList(const PrDomDocument* document, uint32_t list) {
    const DomClasses* classes = document->classes;
    uint32_t start = classes->starts.data[list - 1];
    return DomClassList{classes->atoms.data + start, classes->starts.data[list] - start};
}

PrStringView domClassesText(const PrDomDocument* document, uint32_t list) {
    return atomTableText(&document->classes->texts, list - 1);
}

Atom domClassesFind(const PrDomDocument* document, PrStringView name) {
    return document->classes != nullptr ? atomTableFind(&document->classes->names, name) : kAtomNone;
}

PrStringView domClassesName(const PrDomDocument* document, Atom atom) {
    return atomTableText(&document->classes->names, atom);
}

bool domClassesHas(const PrDomDocument* document, uint32_t list, Atom atom) {
    DomClassList classes = domClassesList(document, list);
    for (uint32_t i = 0; i < classes.count; ++i) {
        if (classes.atoms[i] == atom)
            return true;
    }
    return false;
}

void domClassesDestroy(DomClasses* PR_NULLABLE classes) {
    if (classes == nullptr)
        return;
    atomTableReset(&classes->names);
    atomTableReset(&classes->texts);
    classes->starts.reset();
    classes->atoms.reset();
    prMemoryFree(classes);
}

} // namespace pr_impl

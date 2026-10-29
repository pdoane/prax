// Classes
//
// dom - https://dom.spec.whatwg.org/#concept-class
//
// The class names of a document's elements, interned for the document.

#pragma once

#include "pr/atom_table.h"
#include "pr/dom.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The list number of an element without classes.
constexpr uint32_t kDomClassesNone = 0;

// The class names of a document as atoms, and the lists of names that its class attributes hold, one list for each
// distinct text. Lists are numbered from one in the order in which their texts were read. The table only adds entries,
// and is freed with its document.
struct DomClasses {
    AtomTable names;
    AtomTable texts;            // list n is text n - 1
    Array<uint32_t> starts;     // first index in `atoms` of each list, then the end of the last list
    Array<Atom> atoms;          // the names of every list, list after list
    uint32_t last;              // the list last requested, which is most often the next one requested
};

// The class names of a list, each appearing once.
struct DomClassList {
    const Atom* PR_NULLABLE atoms;
    uint32_t count;
};

// Adds the list of the words in `text`, which is the value of a class attribute in the document, and returns the list's
// number. Each word appears once (dom/#concept-ordered-set-parser). A text read before returns its existing list.
// Returns kDomClassesNone if `text` holds no word.
uint32_t domClassesAdd(PrDomDocument* document, PrStringView text);

// Returns the class names of list number `list`.
DomClassList domClassesList(const PrDomDocument* document, uint32_t list);

// Returns the text that the list was read from.
PrStringView domClassesText(const PrDomDocument* document, uint32_t list);

// Returns the atom of the class name `name`, or kAtomNone if no list of the document holds it.
Atom domClassesFind(const PrDomDocument* document, PrStringView name);

// Returns the text of the class name `atom`.
PrStringView domClassesName(const PrDomDocument* document, Atom atom);

// Returns true if the list holds the class name `atom`.
bool domClassesHas(const PrDomDocument* document, uint32_t list, Atom atom);

// Frees the table.
void domClassesDestroy(DomClasses* PR_NULLABLE classes);

} // namespace pr_impl

PR_ASSUME_NONNULL_END

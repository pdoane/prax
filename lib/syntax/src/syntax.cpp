// Syntax: the registry of languages and category names, and the dispatch to a language's syntax trees.

#include "pr/syntax.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/atom_table.h"
#include "pr/regex.h"

namespace pr_impl {

namespace {

// The languages that a registry holds, and the category names that its trees have given, each an atom that is its
// PrSyntaxCategory.
struct SyntaxRegistry {
    Array<const PrSyntaxLanguage*> languages;
    Array<PrRegex*> namePatterns;   // by index of `languages`: its name pattern, or null for none
    AtomTable categories;
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrSyntaxRegistry : SyntaxRegistry {};

PrSyntaxRegistry* prSyntaxRegistryCreate(void) {
    return allocZeroed<PrSyntaxRegistry>();
}

void prSyntaxRegistryDestroy(PrSyntaxRegistry* PR_NULLABLE registry) {
    if (registry == nullptr)
        return;
    atomTableReset(&registry->categories);
    for (uint32_t i = 0; i < registry->namePatterns.count; ++i)
        prRegexDestroy(registry->namePatterns.data[i]);
    registry->namePatterns.reset();
    registry->languages.reset();
    prMemoryFree(registry);
}

void prSyntaxRegistryAdd(PrSyntaxRegistry* registry, const PrSyntaxLanguage* language) {
    registry->languages.push(language);
    // A pattern that does not compile matches no name.
    PrRegex* pattern = nullptr;
    if (language->namePattern.size != 0)
        prRegexCreate(language->namePattern, &pattern);
    registry->namePatterns.push(pattern);
}

const PrSyntaxLanguage* PR_NULLABLE prSyntaxRegistryFind(const PrSyntaxRegistry* registry, PrStringView name) {
    for (uint32_t i = registry->languages.count; i-- > 0;) {
        if (asciiEqualsFold(registry->languages.data[i]->name, name))
            return registry->languages.data[i];
    }
    // The names that an injection gives are short, so a name too long for the buffer matches no pattern.
    char lowered[64];
    if (name.size > sizeof(lowered))
        return nullptr;
    for (size_t i = 0; i < name.size; ++i)
        lowered[i] = asciiLower(name.data[i]);
    PrStringView folded{lowered, name.size};
    for (uint32_t i = registry->languages.count; i-- > 0;) {
        const PrRegex* pattern = registry->namePatterns.data[i];
        if (pattern != nullptr && prRegexSearch(pattern, folded, nullptr, 0))
            return registry->languages.data[i];
    }
    return nullptr;
}

const PrSyntaxLanguage* PR_NULLABLE prSyntaxRegistryFromPath(const PrSyntaxRegistry* registry, PrStringView path) {
    size_t slash = path.size;
    while (slash > 0 && path.data[slash - 1] != '/' && path.data[slash - 1] != '\\')
        --slash;
    PrStringView file{path.data + slash, path.size - slash};
    for (uint32_t i = registry->languages.count; i-- > 0;) {
        const PrSyntaxLanguage* language = registry->languages.data[i];
        for (uint32_t k = 0; k < language->fileNameCount; ++k) {
            if (asciiEqualsFold(language->fileNames[k], file))
                return language;
        }
    }
    size_t dot = file.size;
    while (dot > 0 && file.data[dot - 1] != '.')
        --dot;
    // A name that starts with its only dot, as a hidden file's does, has no extension.
    if (dot <= 1)
        return nullptr;
    PrStringView extension{file.data + dot, file.size - dot};
    for (uint32_t i = registry->languages.count; i-- > 0;) {
        const PrSyntaxLanguage* language = registry->languages.data[i];
        for (uint32_t k = 0; k < language->extensionCount; ++k) {
            if (asciiEqualsFold(language->extensions[k], extension))
                return language;
        }
    }
    return nullptr;
}

PrSyntaxCategory prSyntaxRegistryCategoryIntern(PrSyntaxRegistry* registry, PrStringView name) {
    return atomTableAdd(&registry->categories, name);
}

uint32_t prSyntaxRegistryCategoryCount(const PrSyntaxRegistry* registry) {
    return registry->categories.atoms.count;
}

PrStringView prSyntaxRegistryCategoryName(const PrSyntaxRegistry* registry, PrSyntaxCategory category) {
    PR_ASSERT(category < registry->categories.atoms.count);
    return atomTableText(&registry->categories, category);
}

PrSyntaxTree* prSyntaxTreeCreate(const PrSyntaxLanguage* language, PrSyntaxRegistry* registry, PrTextEditBuffer* buffer) {
    return language->create(language, registry, buffer);
}

void prSyntaxTreeDestroy(PrSyntaxTree* PR_NULLABLE tree) {
    if (tree != nullptr)
        tree->destroy(tree);
}

bool prSyntaxTreeUpdate(PrSyntaxTree* tree, double milliseconds) {
    return tree->update(tree, milliseconds);
}

bool prSyntaxTreeIsPending(const PrSyntaxTree* tree) {
    return tree->pending != nullptr && tree->pending(tree);
}

uint32_t prSyntaxTreeSpans(PrSyntaxTree* tree, uint32_t start, uint32_t end,
        PrSyntaxSpan* PR_NULLABLE out, uint32_t capacity) {
    PR_ASSERT(start <= end);
    return tree->spans(tree, start, end, out, capacity);
}

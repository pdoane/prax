// Syntax: the syntax trees of edited texts, which a language's grammar parses and each edit parses again incrementally,
// and the categories that the grammar gives ranges of the text, such as `keyword` or `string.special`.
//
// A language is an interface (PrSyntaxLanguage) that makes the syntax tree of a text edit buffer. A tree follows its
// buffer's edit stream, and is asked for the spans of the categories in a range, so the cost of each follows the edits
// and the range that is shown. What a category looks like is the host's decision.
//
// A registry holds the languages that a host links, finds them by name or by file, and gives each category name one
// number across its languages, so that a host maps names to looks once. One thread uses a registry, and everything made
// with it, at a time.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"
#include "pr/text_edit.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The languages that a host links, and the names of the categories that their trees give.
typedef struct PrSyntaxRegistry PrSyntaxRegistry;

// The syntax tree of one buffer in one language.
typedef struct PrSyntaxTree PrSyntaxTree;

// A language, which makes the syntax trees of buffers.
typedef struct PrSyntaxLanguage PrSyntaxLanguage;

// A category name, by its place in a registry's table of names.
typedef uint32_t PrSyntaxCategory;

// A range of the text of one category.
typedef struct PrSyntaxSpan {
    uint32_t start;
    uint32_t end;
    PrSyntaxCategory category;
} PrSyntaxSpan;

// ---------------------------------------------------------------------------------------------------------------------
// Languages

// Creates the syntax tree of `buffer` in `language`. The buffer must outlive the tree. `registry` interns the category
// names that the tree's spans give.
typedef PrSyntaxTree* PR_NONNULL (*PrSyntaxTreeCreateFn)(const PrSyntaxLanguage* language, PrSyntaxRegistry* registry,
    PrTextEditBuffer* buffer);

// A language: its names, and the constructor of its trees. A language is immutable, and outlives every registry that
// holds it and every tree that it makes.
struct PrSyntaxLanguage {
    PrStringView name;                          // as an injection or a host names it, such as "javascript"

    // A pattern (pr/regex.h) that the other names of the language match, as a fence of Markdown names its language,
    // such as `^(js|javascript)$`, or empty for none. The names are lowercased before they are matched.
    PrStringView namePattern;

    const PrStringView* PR_NULLABLE extensions; // without the dot, such as "js"
    uint32_t extensionCount;
    const PrStringView* PR_NULLABLE fileNames;  // whole names that imply the language, such as "Makefile"
    uint32_t fileNameCount;
    PrSyntaxTreeCreateFn create;
};

// ---------------------------------------------------------------------------------------------------------------------
// Registries

// Creates an empty registry, or frees one.
PrSyntaxRegistry* prSyntaxRegistryCreate(void);
void prSyntaxRegistryDestroy(PrSyntaxRegistry* PR_NULLABLE registry);

// Adds `language`, which must outlive the registry. A language added later wins a name, an extension or a file name
// that an earlier one has.
void prSyntaxRegistryAdd(PrSyntaxRegistry* registry, const PrSyntaxLanguage* language);

// Returns the language of `name`, or the language whose name pattern matches `name`, in any case, or null.
const PrSyntaxLanguage* PR_NULLABLE prSyntaxRegistryFind(const PrSyntaxRegistry* registry, PrStringView name);

// Returns the language of the file at `path`, by its whole file name and then by its extension, in any case, or null.
const PrSyntaxLanguage* PR_NULLABLE prSyntaxRegistryFromPath(const PrSyntaxRegistry* registry, PrStringView path);

// Returns the category of `name`, which the registry adds if it has no such category. Two names that differ name two
// categories, so `function.method` is not `function`.
PrSyntaxCategory prSyntaxRegistryCategoryIntern(PrSyntaxRegistry* registry, PrStringView name);

// The number of categories, and the name of each, which is valid while the registry lives. The count grows as trees
// meet new names.
uint32_t prSyntaxRegistryCategoryCount(const PrSyntaxRegistry* registry);
PrStringView prSyntaxRegistryCategoryName(const PrSyntaxRegistry* registry, PrSyntaxCategory category);

// ---------------------------------------------------------------------------------------------------------------------
// Syntax trees

// Creates the syntax tree of `buffer` in `language`, or frees one. A new tree has parsed nothing.
PrSyntaxTree* prSyntaxTreeCreate(const PrSyntaxLanguage* language, PrSyntaxRegistry* registry, PrTextEditBuffer* buffer);
void prSyntaxTreeDestroy(PrSyntaxTree* PR_NULLABLE tree);

// Parses what the buffer's edits since the last update changed, for about `milliseconds`, and returns true if the spans
// may differ from what they were before the call. A parse that runs out of time resumes at the next update, and until
// it finishes, the spans are those of the last parse that finished, moved by the edits. Infinity parses to the end. The
// cost of a parse follows what the edits changed, and the first parse reads the whole text.
bool prSyntaxTreeUpdate(PrSyntaxTree* tree, double milliseconds);

// Returns true if the last update ran out of time before its parse finished.
bool prSyntaxTreeIsPending(const PrSyntaxTree* tree);

// Writes the spans that intersect the bytes from `start` to `end`, as of the last parse that finished, to `out`, up to
// `capacity`, and returns their number. The spans are in order and do not overlap, and none is empty. Where categories
// nest, the innermost names the bytes, and of the categories of one range, the last that the grammar lists. A span may
// reach outside the range. Such a call has no side effects on what later calls return.
uint32_t prSyntaxTreeSpans(PrSyntaxTree* tree, uint32_t start, uint32_t end, PrSyntaxSpan* PR_NULLABLE out, uint32_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

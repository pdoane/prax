// Syntax: the syntax trees of edited texts, which a language's grammar parses and each edit parses again incrementally,
// and the categories that the grammar gives ranges of the text, such as `keyword` or `string.special`.
//
// A language is an interface (PrSyntaxLanguage) that makes the syntax tree of a text edit buffer. A tree follows its
// buffer's edit stream, and is asked for the spans of the categories in a range, so the cost of each follows the edits
// and the range that is shown. What a category looks like is the host's decision.
//
// A registry holds the languages that a host links, finds them by name or by media type, and gives each category name
// one number across its languages, so that a host maps names to looks once. One thread uses a registry, and everything
// made with it, at a time.
//
// A resource's language is the language of its media type. The registry reads no file names: what a resource's name
// tells about its text reaches the registry through the media type that a classifier gives it (pr/mime_sniff.h).

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

    // The media types of the texts that the language reads, each an essence, such as "application/json", or a
    // structured syntax suffix (RFC 6839), such as "+json", which covers every type whose subtype ends in it. A
    // language of none is one that only an injection reaches, by its name.
    const PrStringView* PR_NULLABLE mediaTypes;
    uint32_t mediaTypeCount;

    PrSyntaxTreeCreateFn create;
};

// ---------------------------------------------------------------------------------------------------------------------
// Registries

// Creates an empty registry, or frees one.
PrSyntaxRegistry* prSyntaxRegistryCreate(void);
void prSyntaxRegistryDestroy(PrSyntaxRegistry* PR_NULLABLE registry);

// Adds `language`, which must outlive the registry. A language added later wins a name or a media type that an earlier
// one has.
void prSyntaxRegistryAdd(PrSyntaxRegistry* registry, const PrSyntaxLanguage* language);

// Returns the language of `name`, or the language whose name pattern matches `name`, in any case, or null.
const PrSyntaxLanguage* PR_NULLABLE prSyntaxRegistryFind(const PrSyntaxRegistry* registry, PrStringView name);

// The number of languages, and each, in the order that they were added.
uint32_t prSyntaxRegistryLanguageCount(const PrSyntaxRegistry* registry);
const PrSyntaxLanguage* prSyntaxRegistryLanguage(const PrSyntaxRegistry* registry, uint32_t index);

// Returns the language of the media type `type`, in any case, or null. `type` is an essence or a whole type, whose
// parameters do not matter, such as a type that a classifier gives a resource (pr/mime_sniff.h). A language that lists
// the essence wins over one that lists its structured syntax suffix, so glTF's `model/gltf+json` reads as JSON.
const PrSyntaxLanguage* PR_NULLABLE prSyntaxRegistryFromMediaType(const PrSyntaxRegistry* registry, PrStringView type);

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

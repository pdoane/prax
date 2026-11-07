// The interface that syntax backends implement. Callers use the functions of pr/syntax.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/base.h"
#include "pr/syntax.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a syntax tree's backend, which a language implements, at the start of its state.
struct PrSyntaxTree {
    void (*destroyFn)(PrSyntaxTree* tree);

    // As prSyntaxTreeUpdate.
    bool (*updateFn)(PrSyntaxTree* tree, double milliseconds);

    // As prSyntaxTreeIsPending. May be null for a tree whose updates always finish.
    bool (*PR_NULLABLE pendingFn)(const PrSyntaxTree* tree);

    // As prSyntaxTreeSpans.
    uint32_t (*spansFn)(PrSyntaxTree* tree, uint32_t start, uint32_t end, PrSyntaxSpan* PR_NULLABLE out, uint32_t capacity);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

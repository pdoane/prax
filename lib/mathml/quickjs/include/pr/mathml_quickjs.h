// JavaScript binding of MathML.

#pragma once

#include "pr/mathml.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Binds the classes of MathML elements. The DOM's binding must be bound first.
void prJsMathMlBind(JSContext* ctx);

// The class of MathMLElement. Other modules add members to it.
extern JSClassID prJsMathMlElementClass;

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

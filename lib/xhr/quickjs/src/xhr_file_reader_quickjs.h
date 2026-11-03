// JavaScript binding of FileReader.

#pragma once

#include "pr/base.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Defines FileReader on the global object of `ctx`.
void jsXhrFileReaderBind(JSContext* ctx);

} // namespace pr_impl

PR_ASSUME_NONNULL_END

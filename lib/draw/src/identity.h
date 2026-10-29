// Identities of draw resources.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns an identity that no other image, list or filter has had during the life of the process.
uint64_t drawNextId();

} // namespace pr_impl

PR_ASSUME_NONNULL_END

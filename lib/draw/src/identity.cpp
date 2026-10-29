// Identities of draw resources.

#include "draw/src/identity.h"

#include "pr/atomic.h"

namespace pr_impl {
namespace {

Atomic<uint64_t> nextId;

} // namespace

uint64_t drawNextId() {
    return nextId.fetchAdd(1) + 1;
}

} // namespace pr_impl

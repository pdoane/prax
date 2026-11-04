// Deferred calls: a function that runs when its scope ends.

#pragma once

#include "pr/base.h"
#include "pr/type_traits.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Calls `fn` at the end of its scope, as in `Defer unmapping([&] { prFileUnmap(&mapping); });`.
template <typename Fn>
struct Defer {
    explicit Defer(Fn fn) : deferred(move(fn)) {}
    ~Defer() { deferred(); }
    Defer(const Defer&) = delete;
    Defer& operator=(const Defer&) = delete;

private:
    Fn deferred;
};

} // namespace pr

PR_ASSUME_NONNULL_END

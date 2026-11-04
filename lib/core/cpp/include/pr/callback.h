// Callbacks: a callable passed to a C API as the function and context pair that the API takes.

#pragma once

#include "pr/base.h"
#include "pr/type_traits.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// The function of a callback whose context is a `Fn`. It converts to the C API's function type, whatever its result and
// the parameters after the context, and the function it converts to calls the `Fn` that the context points to.
template <typename Fn>
struct ScopedCallback {
    template <typename Result, typename... Arguments>
    using Pointer = Result (*PR_NONNULL)(void* PR_NULLABLE context, Arguments... arguments);

    template <typename Result, typename... Arguments>
    operator Pointer<Result, Arguments...>() const {
        return [](void* PR_NULLABLE context, Arguments... arguments) -> Result {
            return (*static_cast<Fn*>(context))(forward<Arguments>(arguments)...);
        };
    }
};

// Returns the function for a callback to `fn`, whose context is `&fn`. `fn` must outlive every call that the API makes.
template <typename Fn>
ScopedCallback<Fn> scopedCallback(Fn&) {
    return ScopedCallback<Fn>{};
}

} // namespace pr

// The function and context that pass the lvalue `fn` as a C API's callback, so that a lambda with captures reads as one
// argument. The context is `fn` in the caller's scope, so the API must make its last call before that scope ends: a
// blocking or synchronous call, not one that posts the callback for later.
//
//   prNetPostBlocking(net, PR_SCOPED_CALLBACK(destroyOnLoop));
#define PR_SCOPED_CALLBACK(fn) ::pr::scopedCallback(fn), &(fn)

PR_ASSUME_NONNULL_END

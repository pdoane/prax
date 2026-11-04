// Functions as values: any callable with a given signature, such as a lambda with captures, held by a value that copies
// and destroys it.

#pragma once

#include "pr/assert.h"
#include "pr/base.h"
#include "pr/construct.h"
#include "pr/memory.h"
#include "pr/type_traits.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// A function of `Signature`, which a specialization below defines for a function type.
template <typename Signature>
struct Function;

// A callable that takes `Arguments` and returns `Result`. A callable that fits in three pointers is held inline, and a
// larger one on the heap. A function made with no callable, or with a null function pointer, is empty, and must not be
// called.
//
// A function holds a callable that can be copied, since the function copies it, and whose result converts to `Result`.
// A callable that can only be moved needs a type of its own.
template <typename Result, typename... Arguments>
struct Function<Result(Arguments...)> {
    Function() = default;

    template <typename Fn>
    Function(Fn&& fn) requires(!isSame<Decay<Fn>, Function> && isConstructible<Decay<Fn>, const Decay<Fn>&> &&
        requires(Decay<Fn>& held, Arguments&&... arguments) {
            requires isConvertible<decltype(held(forward<Arguments>(arguments)...)), Result>;
        }) {
        using Held = Decay<Fn>;
        // A function pointer can be null, and a null one makes an empty function.
        if constexpr (requires { static_cast<Held>(nullptr); }) {
            if (fn == nullptr)
                return;
        }
        if constexpr (sizeof(Held) <= sizeof(storage) && alignof(Held) <= alignof(void*)) {
            construct<Held>(storage, forward<Fn>(fn));
            operations = &Inline<Held>::kOperations;
        } else {
            Held* held = construct<Held>(prMemoryAlloc(sizeof(Held)), forward<Fn>(fn));
            *reinterpret_cast<Held**>(storage) = held;
            operations = &Heap<Held>::kOperations;
        }
    }

    Function(const Function& other) : operations(other.operations) {
        if (operations != nullptr)
            operations->copyTo(storage, other.storage);
    }
    Function(Function&& other) : operations(other.operations) {
        if (operations != nullptr)
            operations->moveTo(storage, other.storage);
        other.operations = nullptr;
    }
    Function& operator=(const Function& other) {
        if (this != &other) {
            reset();
            operations = other.operations;
            if (operations != nullptr)
                operations->copyTo(storage, other.storage);
        }
        return *this;
    }
    Function& operator=(Function&& other) {
        if (this != &other) {
            reset();
            operations = other.operations;
            if (operations != nullptr)
                operations->moveTo(storage, other.storage);
            other.operations = nullptr;
        }
        return *this;
    }
    ~Function() { reset(); }

    explicit operator bool() const { return operations != nullptr; }

    // Calls the callable. As with a lambda that is mutable, the call may change what the callable holds.
    Result operator()(Arguments... arguments) const {
        PR_ASSERT(operations != nullptr);
        return operations->call(const_cast<unsigned char*>(storage), forward<Arguments>(arguments)...);
    }

    // Destroys the callable, and leaves the function empty.
    void reset() {
        if (operations != nullptr)
            operations->destroyAt(storage);
        operations = nullptr;
    }

private:
    // What a function does with the callable that it holds, the same for every function of one callable's type.
    struct Operations {
        Result (*PR_NONNULL call)(void* held, Arguments&&... arguments);
        void (*copyTo)(void* to, const void* from);
        void (*moveTo)(void* to, void* from);
        void (*destroyAt)(void* held);
    };

    // A callable held in the function's storage.
    template <typename Held>
    struct Inline {
        static Held* PR_NONNULL of(const void* at) { return static_cast<Held*>(const_cast<void*>(at)); }
        static Result call(void* held, Arguments&&... arguments) {
            if constexpr (isSame<Result, void>)
                (*of(held))(forward<Arguments>(arguments)...);
            else
                return (*of(held))(forward<Arguments>(arguments)...);
        }
        static void copyTo(void* to, const void* from) { construct<Held>(to, *of(from)); }
        static void moveTo(void* to, void* from) {
            construct<Held>(to, move(*of(from)));
            destroy(of(from));
        }
        static void destroyAt(void* held) { destroy(of(held)); }
        static constexpr Operations kOperations = {call, copyTo, moveTo, destroyAt};
    };

    // A callable on the heap, which the function's storage points to.
    template <typename Held>
    struct Heap {
        static Held* PR_NONNULL of(const void* at) { return *static_cast<Held* const*>(at); }
        static Result call(void* held, Arguments&&... arguments) {
            if constexpr (isSame<Result, void>)
                (*of(held))(forward<Arguments>(arguments)...);
            else
                return (*of(held))(forward<Arguments>(arguments)...);
        }
        static void copyTo(void* to, const void* from) {
            *static_cast<Held**>(to) = construct<Held>(prMemoryAlloc(sizeof(Held)), *of(from));
        }
        static void moveTo(void* to, void* from) { *static_cast<Held**>(to) = of(from); }
        static void destroyAt(void* held) {
            Held* at = of(held);
            destroy(at);
            prMemoryFree(at);
        }
        static constexpr Operations kOperations = {call, copyTo, moveTo, destroyAt};
    };

    alignas(void*) unsigned char storage[3 * sizeof(void*)] = {};
    const Operations* PR_NULLABLE operations = nullptr;
};

} // namespace pr

PR_ASSUME_NONNULL_END

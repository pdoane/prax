// Atomic storage, whose operations acquire and release unless they are the relaxed forms.

#pragma once

#include "pr/base.h"

#if defined(_MSC_VER)
#include "pr/atomic_msvc.h"
#endif

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// The unsigned integer type of `Size` bytes.
template <size_t Size>
struct AtomicWord;
template <>
struct AtomicWord<1> {
    using Type = uint8_t;
};
template <>
struct AtomicWord<2> {
    using Type = uint16_t;
};
template <>
struct AtomicWord<4> {
    using Type = uint32_t;
};
template <>
struct AtomicWord<8> {
    using Type = uint64_t;
};

// True for the integral types, which fetch-add and fetch-sub take. MSVC has no __is_integral.
template <typename T>
inline constexpr bool kAtomicIntegral = false;
template <>
inline constexpr bool kAtomicIntegral<bool> = true;
template <>
inline constexpr bool kAtomicIntegral<char> = true;
template <>
inline constexpr bool kAtomicIntegral<signed char> = true;
template <>
inline constexpr bool kAtomicIntegral<unsigned char> = true;
template <>
inline constexpr bool kAtomicIntegral<char8_t> = true;
template <>
inline constexpr bool kAtomicIntegral<char16_t> = true;
template <>
inline constexpr bool kAtomicIntegral<char32_t> = true;
template <>
inline constexpr bool kAtomicIntegral<wchar_t> = true;
template <>
inline constexpr bool kAtomicIntegral<short> = true;
template <>
inline constexpr bool kAtomicIntegral<unsigned short> = true;
template <>
inline constexpr bool kAtomicIntegral<int> = true;
template <>
inline constexpr bool kAtomicIntegral<unsigned int> = true;
template <>
inline constexpr bool kAtomicIntegral<long> = true;
template <>
inline constexpr bool kAtomicIntegral<unsigned long> = true;
template <>
inline constexpr bool kAtomicIntegral<long long> = true;
template <>
inline constexpr bool kAtomicIntegral<unsigned long long> = true;

// Atomic storage for a trivially copyable value of 1, 2, 4 or 8 bytes, aligned to its size. A static instance starts at
// zero, and any other instance is stored to before it is shared. It cannot be copied or moved.
//
// Loads acquire and stores release. Exchange, CAS and fetch-add acquire and release, and a failed CAS acquires. The
// relaxed forms order nothing else. Fetch-add and fetch-sub need an integral `T`. CAS compares object representations,
// so every byte of `T` must have a defined value.
template <typename T>
struct Atomic {
    static_assert(__is_trivially_copyable(T));
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
private:
    using Word = typename AtomicWord<sizeof(T)>::Type;

    // The bits are mutable because a 64-bit load on 32-bit x86 is a compare-exchange, even on a const object.
    alignas(sizeof(T)) mutable Word bits;

    template <bool Relaxed>
    PR_FORCE_INLINE T loadImpl() const {
#if defined(_MSC_VER)
        const Word value = atomicMsvcLoad<Relaxed>(&bits);
#else
        const Word value = __atomic_load_n(&bits, Relaxed ? __ATOMIC_RELAXED : __ATOMIC_ACQUIRE);
#endif
        return __builtin_bit_cast(T, value);
    }

    template <bool Relaxed>
    PR_FORCE_INLINE void storeImpl(T value) {
        const Word word = __builtin_bit_cast(Word, value);
#if defined(_MSC_VER)
        atomicMsvcStore<Relaxed>(&bits, word);
#else
        __atomic_store_n(&bits, word, Relaxed ? __ATOMIC_RELAXED : __ATOMIC_RELEASE);
#endif
    }

    template <bool Relaxed>
    PR_FORCE_INLINE T exchangeImpl(T value) {
        const Word word = __builtin_bit_cast(Word, value);
#if defined(_MSC_VER)
        const Word previous = atomicMsvcExchange<Relaxed>(&bits, word);
#else
        const Word previous =
            __atomic_exchange_n(&bits, word, Relaxed ? __ATOMIC_RELAXED : __ATOMIC_ACQ_REL);
#endif
        return __builtin_bit_cast(T, previous);
    }

    template <bool Relaxed>
    PR_FORCE_INLINE T addImpl(Word operand)
        requires(kAtomicIntegral<T>)
    {
#if defined(_MSC_VER)
        const Word previous = atomicMsvcExchangeAdd<Relaxed>(&bits, operand);
#else
        const Word previous =
            __atomic_fetch_add(&bits, operand, Relaxed ? __ATOMIC_RELAXED : __ATOMIC_ACQ_REL);
#endif
        return __builtin_bit_cast(T, previous);
    }

    template <bool Relaxed>
    PR_FORCE_INLINE bool casImpl(T& expected, T desired) {
        const Word word = __builtin_bit_cast(Word, desired);
        Word observed = __builtin_bit_cast(Word, expected);
#if defined(_MSC_VER)
        const Word previous = atomicMsvcCompareExchange<Relaxed>(&bits, observed, word);
        const bool matched = previous == observed;
        observed = previous;
#else
        const bool matched = __atomic_compare_exchange_n(
            &bits, &observed, word, false, Relaxed ? __ATOMIC_RELAXED : __ATOMIC_ACQ_REL,
            Relaxed ? __ATOMIC_RELAXED : __ATOMIC_ACQUIRE);
#endif
        if (!matched)
            expected = __builtin_bit_cast(T, observed);
        return matched;
    }

public:
    Atomic() = default;
    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;

    // Reads the stored value.
    PR_FORCE_INLINE T load() const {
        return loadImpl<false>();
    }
    PR_FORCE_INLINE T loadRelaxed() const {
        return loadImpl<true>();
    }

    // Replaces the stored value.
    PR_FORCE_INLINE void store(T value) {
        storeImpl<false>(value);
    }
    PR_FORCE_INLINE void storeRelaxed(T value) {
        storeImpl<true>(value);
    }

    // Replaces the stored value and returns the previous value.
    PR_FORCE_INLINE T exchange(T value) {
        return exchangeImpl<false>(value);
    }
    PR_FORCE_INLINE T exchangeRelaxed(T value) {
        return exchangeImpl<true>(value);
    }

    // Stores `desired` and returns true if the stored value equals `expected`. Otherwise writes the stored value to
    // `expected` and returns false. Never fails spuriously.
    PR_FORCE_INLINE bool cas(T& expected, T desired) {
        return casImpl<false>(expected, desired);
    }
    PR_FORCE_INLINE bool casRelaxed(T& expected, T desired) {
        return casImpl<true>(expected, desired);
    }

    // Adds `value` to the stored value, wrapping on overflow, and returns the previous value. The operand is a variable
    // because MSVC does not parse a bit cast in the arguments of a call with template arguments.
    PR_FORCE_INLINE T fetchAdd(T value)
        requires(kAtomicIntegral<T>)
    {
        const Word operand = __builtin_bit_cast(Word, value);
        return addImpl<false>(operand);
    }
    PR_FORCE_INLINE T fetchAddRelaxed(T value)
        requires(kAtomicIntegral<T>)
    {
        const Word operand = __builtin_bit_cast(Word, value);
        return addImpl<true>(operand);
    }

    // Subtracts `value` from the stored value, wrapping on underflow, and returns the previous value.
    PR_FORCE_INLINE T fetchSub(T value)
        requires(kAtomicIntegral<T>)
    {
        const Word operand = Word(0) - __builtin_bit_cast(Word, value);
        return addImpl<false>(operand);
    }
    PR_FORCE_INLINE T fetchSubRelaxed(T value)
        requires(kAtomicIntegral<T>)
    {
        const Word operand = Word(0) - __builtin_bit_cast(Word, value);
        return addImpl<true>(operand);
    }
};

} // namespace pr

PR_ASSUME_NONNULL_END

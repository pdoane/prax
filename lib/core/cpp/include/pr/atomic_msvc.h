// Atomics on MSVC: the intrinsics behind atomic.h, for x86, x64 and ARM64.

#pragma once

#include "pr/base.h"

#if !defined(_M_IX86) && !defined(_M_X64) && !defined(_M_ARM64) && !defined(_M_ARM64EC)
#error "MSVC atomics require x86, x64, or ARM64"
#endif

// Interlocked exchange, add and compare-exchange.
extern "C" char _InterlockedExchange8(volatile char*, char);
#pragma intrinsic(_InterlockedExchange8)
extern "C" short _InterlockedExchange16(volatile short*, short);
#pragma intrinsic(_InterlockedExchange16)
extern "C" long _InterlockedExchange(volatile long*, long);
#pragma intrinsic(_InterlockedExchange)
#if !defined(_M_IX86)
extern "C" __int64 _InterlockedExchange64(volatile __int64*, __int64);
#pragma intrinsic(_InterlockedExchange64)
#endif
extern "C" char _InterlockedExchangeAdd8(volatile char*, char);
#pragma intrinsic(_InterlockedExchangeAdd8)
extern "C" short _InterlockedExchangeAdd16(volatile short*, short);
#pragma intrinsic(_InterlockedExchangeAdd16)
extern "C" long _InterlockedExchangeAdd(volatile long*, long);
#pragma intrinsic(_InterlockedExchangeAdd)
#if !defined(_M_IX86)
extern "C" __int64 _InterlockedExchangeAdd64(volatile __int64*, __int64);
#pragma intrinsic(_InterlockedExchangeAdd64)
#endif
extern "C" char _InterlockedCompareExchange8(volatile char*, char, char);
#pragma intrinsic(_InterlockedCompareExchange8)
extern "C" short _InterlockedCompareExchange16(volatile short*, short, short);
#pragma intrinsic(_InterlockedCompareExchange16)
extern "C" long _InterlockedCompareExchange(volatile long*, long, long);
#pragma intrinsic(_InterlockedCompareExchange)
extern "C" __int64 _InterlockedCompareExchange64(volatile __int64*, __int64, __int64);
#pragma intrinsic(_InterlockedCompareExchange64)

// A compiler barrier: the compiler moves no memory access across it.
extern "C" void _ReadWriteBarrier(void);
#pragma intrinsic(_ReadWriteBarrier)

// ARM64: the interlocked operations without barriers (`_nf`), volatile loads and stores, and the intrinsics that order
// them.
#if defined(_M_ARM64) || defined(_M_ARM64EC)
extern "C" char _InterlockedExchangeAdd8_nf(volatile char*, char);
#pragma intrinsic(_InterlockedExchangeAdd8_nf)
extern "C" short _InterlockedExchangeAdd16_nf(volatile short*, short);
#pragma intrinsic(_InterlockedExchangeAdd16_nf)
extern "C" long _InterlockedExchangeAdd_nf(volatile long*, long);
#pragma intrinsic(_InterlockedExchangeAdd_nf)
extern "C" __int64 _InterlockedExchangeAdd64_nf(volatile __int64*, __int64);
#pragma intrinsic(_InterlockedExchangeAdd64_nf)
extern "C" char _InterlockedExchange8_nf(volatile char*, char);
#pragma intrinsic(_InterlockedExchange8_nf)
extern "C" short _InterlockedExchange16_nf(volatile short*, short);
#pragma intrinsic(_InterlockedExchange16_nf)
extern "C" long _InterlockedExchange_nf(volatile long*, long);
#pragma intrinsic(_InterlockedExchange_nf)
extern "C" __int64 _InterlockedExchange64_nf(volatile __int64*, __int64);
#pragma intrinsic(_InterlockedExchange64_nf)
extern "C" char _InterlockedCompareExchange8_nf(volatile char*, char, char);
#pragma intrinsic(_InterlockedCompareExchange8_nf)
extern "C" short _InterlockedCompareExchange16_nf(volatile short*, short, short);
#pragma intrinsic(_InterlockedCompareExchange16_nf)
extern "C" long _InterlockedCompareExchange_nf(volatile long*, long, long);
#pragma intrinsic(_InterlockedCompareExchange_nf)
extern "C" __int64 _InterlockedCompareExchange64_nf(volatile __int64*, __int64, __int64);
#pragma intrinsic(_InterlockedCompareExchange64_nf)
extern "C" char __iso_volatile_load8(const volatile char*);
#pragma intrinsic(__iso_volatile_load8)
extern "C" void __iso_volatile_store8(volatile char*, char);
#pragma intrinsic(__iso_volatile_store8)
extern "C" short __iso_volatile_load16(const volatile short*);
#pragma intrinsic(__iso_volatile_load16)
extern "C" void __iso_volatile_store16(volatile short*, short);
#pragma intrinsic(__iso_volatile_store16)
extern "C" int __iso_volatile_load32(const volatile int*);
#pragma intrinsic(__iso_volatile_load32)
extern "C" void __iso_volatile_store32(volatile int*, int);
#pragma intrinsic(__iso_volatile_store32)
extern "C" __int64 __iso_volatile_load64(const volatile __int64*);
#pragma intrinsic(__iso_volatile_load64)
extern "C" void __iso_volatile_store64(volatile __int64*, __int64);
#pragma intrinsic(__iso_volatile_store64)

#if defined(__clang__)
// Clang-cl has no LDAR/STLR intrinsics, so a data memory barrier orders its volatile loads and stores.
extern "C" void __dmb(unsigned int);
#pragma intrinsic(__dmb)
#else
extern "C" unsigned char __load_acquire8(const volatile unsigned char*);
#pragma intrinsic(__load_acquire8)
extern "C" void __stlr8(volatile unsigned char*, unsigned char);
#pragma intrinsic(__stlr8)
extern "C" unsigned short __load_acquire16(const volatile unsigned short*);
#pragma intrinsic(__load_acquire16)
extern "C" void __stlr16(volatile unsigned short*, unsigned short);
#pragma intrinsic(__stlr16)
extern "C" unsigned int __load_acquire32(const volatile unsigned int*);
#pragma intrinsic(__load_acquire32)
extern "C" void __stlr32(volatile unsigned int*, unsigned int);
#pragma intrinsic(__stlr32)
extern "C" unsigned __int64 __load_acquire64(const volatile unsigned __int64*);
#pragma intrinsic(__load_acquire64)
extern "C" void __stlr64(volatile unsigned __int64*, unsigned __int64);
#pragma intrinsic(__stlr64)
#endif
#endif

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Stores `desired` at `target` if the bits there equal `expected`, and returns the bits that were there. The ordering
// is acquire-release, or relaxed if `Relaxed`.
template <bool Relaxed, typename Word>
PR_FORCE_INLINE Word atomicMsvcCompareExchange(Word* target, Word expected, Word desired) {
    if constexpr (sizeof(Word) == 1) {
        volatile char* address = reinterpret_cast<volatile char*>(target);
        const char wanted = __builtin_bit_cast(char, desired);
        const char compare = __builtin_bit_cast(char, expected);
#if defined(_M_ARM64) || defined(_M_ARM64EC)
        if constexpr (Relaxed)
            return __builtin_bit_cast(Word, _InterlockedCompareExchange8_nf(address, wanted, compare));
#endif
        return __builtin_bit_cast(Word, _InterlockedCompareExchange8(address, wanted, compare));
    } else if constexpr (sizeof(Word) == 2) {
        volatile short* address = reinterpret_cast<volatile short*>(target);
        const short wanted = __builtin_bit_cast(short, desired);
        const short compare = __builtin_bit_cast(short, expected);
#if defined(_M_ARM64) || defined(_M_ARM64EC)
        if constexpr (Relaxed)
            return __builtin_bit_cast(Word, _InterlockedCompareExchange16_nf(address, wanted, compare));
#endif
        return __builtin_bit_cast(Word, _InterlockedCompareExchange16(address, wanted, compare));
    } else if constexpr (sizeof(Word) == 4) {
        volatile long* address = reinterpret_cast<volatile long*>(target);
        const long wanted = __builtin_bit_cast(long, desired);
        const long compare = __builtin_bit_cast(long, expected);
#if defined(_M_ARM64) || defined(_M_ARM64EC)
        if constexpr (Relaxed)
            return __builtin_bit_cast(Word, _InterlockedCompareExchange_nf(address, wanted, compare));
#endif
        return __builtin_bit_cast(Word, _InterlockedCompareExchange(address, wanted, compare));
    } else if constexpr (sizeof(Word) == 8) {
        volatile __int64* address = reinterpret_cast<volatile __int64*>(target);
        const __int64 wanted = __builtin_bit_cast(__int64, desired);
        const __int64 compare = __builtin_bit_cast(__int64, expected);
#if defined(_M_ARM64) || defined(_M_ARM64EC)
        if constexpr (Relaxed)
            return __builtin_bit_cast(Word, _InterlockedCompareExchange64_nf(address, wanted, compare));
#endif
        return __builtin_bit_cast(Word, _InterlockedCompareExchange64(address, wanted, compare));
    }
}

// Loads the bits at `target`. The ordering is acquire, or relaxed if `Relaxed`.
template <bool Relaxed, typename Word>
PR_FORCE_INLINE Word atomicMsvcLoad(const Word* target) {
#if defined(_M_ARM64) || defined(_M_ARM64EC)
    Word value;
    if constexpr (sizeof(Word) == 1) {
#if !defined(__clang__)
        if constexpr (!Relaxed)
            return Word(__load_acquire8(reinterpret_cast<const volatile unsigned char*>(target)));
#endif
        value = __builtin_bit_cast(Word, __iso_volatile_load8(reinterpret_cast<const volatile char*>(target)));
    } else if constexpr (sizeof(Word) == 2) {
#if !defined(__clang__)
        if constexpr (!Relaxed)
            return Word(__load_acquire16(reinterpret_cast<const volatile unsigned short*>(target)));
#endif
        value = __builtin_bit_cast(Word, __iso_volatile_load16(reinterpret_cast<const volatile short*>(target)));
    } else if constexpr (sizeof(Word) == 4) {
#if !defined(__clang__)
        if constexpr (!Relaxed)
            return Word(__load_acquire32(reinterpret_cast<const volatile unsigned int*>(target)));
#endif
        value = __builtin_bit_cast(Word, __iso_volatile_load32(reinterpret_cast<const volatile int*>(target)));
    } else if constexpr (sizeof(Word) == 8) {
#if !defined(__clang__)
        if constexpr (!Relaxed)
            return Word(__load_acquire64(reinterpret_cast<const volatile unsigned __int64*>(target)));
#endif
        value = __builtin_bit_cast(Word, __iso_volatile_load64(reinterpret_cast<const volatile __int64*>(target)));
    }
#if defined(__clang__)
    if constexpr (!Relaxed)
        __dmb(0xb); // inner-shareable barrier after the load, to acquire
#endif
    return value;
#else
#if defined(_M_IX86)
    if constexpr (sizeof(Word) == 8)
        return atomicMsvcCompareExchange<Relaxed>(const_cast<Word*>(target), Word(0), Word(0));
#endif
    const Word value = *static_cast<const volatile Word*>(target);
    if constexpr (!Relaxed)
        _ReadWriteBarrier();
    return value;
#endif
}

// Stores `desired` at `target` and returns the bits that were there. The ordering is acquire-release, or relaxed if
// `Relaxed`.
template <bool Relaxed, typename Word>
PR_FORCE_INLINE Word atomicMsvcExchange(Word* target, Word desired) {
#if defined(_M_IX86)
    if constexpr (sizeof(Word) == 8) {
        Word expected = atomicMsvcLoad<true>(target);
        for (;;) {
            const Word previous = atomicMsvcCompareExchange<Relaxed>(target, expected, desired);
            if (previous == expected)
                return previous;
            expected = previous;
        }
    }
#endif
    if constexpr (sizeof(Word) == 1) {
        volatile char* address = reinterpret_cast<volatile char*>(target);
        const char wanted = __builtin_bit_cast(char, desired);
#if defined(_M_ARM64) || defined(_M_ARM64EC)
        if constexpr (Relaxed)
            return __builtin_bit_cast(Word, _InterlockedExchange8_nf(address, wanted));
#endif
        return __builtin_bit_cast(Word, _InterlockedExchange8(address, wanted));
    } else if constexpr (sizeof(Word) == 2) {
        volatile short* address = reinterpret_cast<volatile short*>(target);
        const short wanted = __builtin_bit_cast(short, desired);
#if defined(_M_ARM64) || defined(_M_ARM64EC)
        if constexpr (Relaxed)
            return __builtin_bit_cast(Word, _InterlockedExchange16_nf(address, wanted));
#endif
        return __builtin_bit_cast(Word, _InterlockedExchange16(address, wanted));
    } else if constexpr (sizeof(Word) == 4) {
        volatile long* address = reinterpret_cast<volatile long*>(target);
        const long wanted = __builtin_bit_cast(long, desired);
#if defined(_M_ARM64) || defined(_M_ARM64EC)
        if constexpr (Relaxed)
            return __builtin_bit_cast(Word, _InterlockedExchange_nf(address, wanted));
#endif
        return __builtin_bit_cast(Word, _InterlockedExchange(address, wanted));
    }
#if !defined(_M_IX86)
    else if constexpr (sizeof(Word) == 8) {
        volatile __int64* address = reinterpret_cast<volatile __int64*>(target);
        const __int64 wanted = __builtin_bit_cast(__int64, desired);
#if defined(_M_ARM64) || defined(_M_ARM64EC)
        if constexpr (Relaxed)
            return __builtin_bit_cast(Word, _InterlockedExchange64_nf(address, wanted));
#endif
        return __builtin_bit_cast(Word, _InterlockedExchange64(address, wanted));
    }
#endif
}

// Adds `operand` to the bits at `target`, wrapping on overflow, and returns the bits that were there. The ordering is
// acquire-release, or relaxed if `Relaxed`.
template <bool Relaxed, typename Word>
PR_FORCE_INLINE Word atomicMsvcExchangeAdd(Word* target, Word operand) {
#if defined(_M_IX86)
    if constexpr (sizeof(Word) == 8) {
        Word expected = atomicMsvcLoad<true>(target);
        for (;;) {
            const Word previous = atomicMsvcCompareExchange<Relaxed>(target, expected, Word(expected + operand));
            if (previous == expected)
                return previous;
            expected = previous;
        }
    }
#endif
    if constexpr (sizeof(Word) == 1) {
        volatile char* address = reinterpret_cast<volatile char*>(target);
        const char value = __builtin_bit_cast(char, operand);
#if defined(_M_ARM64) || defined(_M_ARM64EC)
        if constexpr (Relaxed)
            return __builtin_bit_cast(Word, _InterlockedExchangeAdd8_nf(address, value));
#endif
        return __builtin_bit_cast(Word, _InterlockedExchangeAdd8(address, value));
    } else if constexpr (sizeof(Word) == 2) {
        volatile short* address = reinterpret_cast<volatile short*>(target);
        const short value = __builtin_bit_cast(short, operand);
#if defined(_M_ARM64) || defined(_M_ARM64EC)
        if constexpr (Relaxed)
            return __builtin_bit_cast(Word, _InterlockedExchangeAdd16_nf(address, value));
#endif
        return __builtin_bit_cast(Word, _InterlockedExchangeAdd16(address, value));
    } else if constexpr (sizeof(Word) == 4) {
        volatile long* address = reinterpret_cast<volatile long*>(target);
        const long value = __builtin_bit_cast(long, operand);
#if defined(_M_ARM64) || defined(_M_ARM64EC)
        if constexpr (Relaxed)
            return __builtin_bit_cast(Word, _InterlockedExchangeAdd_nf(address, value));
#endif
        return __builtin_bit_cast(Word, _InterlockedExchangeAdd(address, value));
    }
#if !defined(_M_IX86)
    else if constexpr (sizeof(Word) == 8) {
        volatile __int64* address = reinterpret_cast<volatile __int64*>(target);
        const __int64 value = __builtin_bit_cast(__int64, operand);
#if defined(_M_ARM64) || defined(_M_ARM64EC)
        if constexpr (Relaxed)
            return __builtin_bit_cast(Word, _InterlockedExchangeAdd64_nf(address, value));
#endif
        return __builtin_bit_cast(Word, _InterlockedExchangeAdd64(address, value));
    }
#endif
}

// Stores `desired` at `target`. The ordering is release, or relaxed if `Relaxed`.
template <bool Relaxed, typename Word>
PR_FORCE_INLINE void atomicMsvcStore(Word* target, Word desired) {
#if defined(_M_ARM64) || defined(_M_ARM64EC)
#if defined(__clang__)
    if constexpr (!Relaxed)
        __dmb(0xb); // inner-shareable barrier before the store, to release
#endif
    if constexpr (sizeof(Word) == 1) {
#if !defined(__clang__)
        if constexpr (!Relaxed) {
            __stlr8(reinterpret_cast<volatile unsigned char*>(target), static_cast<unsigned char>(desired));
            return;
        }
#endif
        __iso_volatile_store8(reinterpret_cast<volatile char*>(target), __builtin_bit_cast(char, desired));
    } else if constexpr (sizeof(Word) == 2) {
#if !defined(__clang__)
        if constexpr (!Relaxed) {
            __stlr16(reinterpret_cast<volatile unsigned short*>(target), static_cast<unsigned short>(desired));
            return;
        }
#endif
        __iso_volatile_store16(reinterpret_cast<volatile short*>(target), __builtin_bit_cast(short, desired));
    } else if constexpr (sizeof(Word) == 4) {
#if !defined(__clang__)
        if constexpr (!Relaxed) {
            __stlr32(reinterpret_cast<volatile unsigned int*>(target), static_cast<unsigned int>(desired));
            return;
        }
#endif
        __iso_volatile_store32(reinterpret_cast<volatile int*>(target), __builtin_bit_cast(int, desired));
    } else if constexpr (sizeof(Word) == 8) {
#if !defined(__clang__)
        if constexpr (!Relaxed) {
            __stlr64(reinterpret_cast<volatile unsigned __int64*>(target), static_cast<unsigned __int64>(desired));
            return;
        }
#endif
        __iso_volatile_store64(reinterpret_cast<volatile __int64*>(target), __builtin_bit_cast(__int64, desired));
    }
#else
#if defined(_M_IX86)
    if constexpr (sizeof(Word) == 8) {
        atomicMsvcExchange<Relaxed>(target, desired);
        return;
    }
#endif
    if constexpr (!Relaxed)
        _ReadWriteBarrier();
    *static_cast<volatile Word*>(target) = desired;
#endif
}

} // namespace pr

PR_ASSUME_NONNULL_END

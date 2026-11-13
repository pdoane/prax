# Platform

The platform is what the library does to the host on its own initiative: allocating memory, reporting a failure it
cannot continue past, starting threads and locking between them, reading clocks, reading and writing files by path,
reserving address space, reading the environment, and reading random bytes. What an embedder hands the library already,
such as a window's surface or a font's bytes, does not go through it. Atomics are instructions, not a facility, and stay
in the toolkit (`pr/atomic.h`). The CPU counters that the benchmarks read are tooling, not part of the platform.

Core's C API is the platform's interface (`pr/memory.h`, `pr/assert.h`, `pr/thread.h`, `pr/mutex.h`, `pr/time.h`,
`pr/file.h`, `pr/vmem.h`, `pr/random.h`), and every library reaches the host through it. Each facility is bound at
compile time, so the library calls the function that implements it directly, with no table between them.

## Groups

A build binds a group whole or not at all, because the members of a group work only together. A condition from one
implementation does not wait with a mutex from another, and one allocator does not free memory from another.

| group | macros | default |
| --- | --- | --- |
| memory | `PR_PLATFORM_ALLOC_BYTES`, `REALLOC_BYTES` and `FREE_BYTES`, with `ALLOC_ZEROED_BYTES`, `USABLE_BYTES` and `TRIM_BYTES` if the embedder has them | the C library, or mimalloc with `PR_PLATFORM_MIMALLOC` |
| failure | `PR_PLATFORM_FAIL`, and `PR_ASSERT`, which replaces the library's assertions | POSIX, or the Windows family on Windows |
| threads | `PR_PLATFORM_THREAD_SUPPORTED`, `_START`, `_JOIN`, `_HARDWARE_COUNT` and `_SLEEP`, `PR_PLATFORM_MUTEX_INIT`, `_RESET`, `_LOCK` and `_UNLOCK`, and `PR_PLATFORM_CONDITION_INIT`, `_RESET`, `_WAIT`, `_WAIT_FOR`, `_SIGNAL` and `_BROADCAST` | POSIX, or the Windows family on Windows, or none with `PR_PLATFORM_THREADS` off |
| files | `PR_PLATFORM_FILE_MAP`, `_UNMAP`, `_INFO`, `_SAME`, `_EACH_ENTRY`, `_WRITE`, `_REMOVE`, `_CREATE_DIRECTORY`, `_USER_DIRECTORY`, `_WORKING_DIRECTORY`, `_STREAM_WRITE` and `_STREAM_READ` | POSIX, or the Windows family on Windows |
| address space | `PR_PLATFORM_VMEM_RESERVE`, `_COMMIT` and `_FREE` | POSIX, or the Windows family on Windows |
| clocks | `PR_PLATFORM_TIME_PERF_NOW`, `_PERF_NANOSECONDS`, `_PERF_MILLISECONDS` and `_MONOTONIC_MILLISECONDS` | POSIX, or the Apple family on Apple targets, or the Windows family on Windows |
| system time | `PR_PLATFORM_TIME_SYSTEM_NOW` and `_LOCAL_OFFSET_SECONDS` | POSIX, or the Windows family on Windows |
| environment | `PR_PLATFORM_ENVIRONMENT_VARIABLE` and `_HOST_NAME` | POSIX, or the Windows family on Windows |
| random bytes | `PR_PLATFORM_RANDOM_BYTES` | POSIX, or the Windows family on Windows |

Each macro behaves as the backend function of the same name. For example, `PR_PLATFORM_MUTEX_LOCK(mutex)` behaves as
`prCorePosixMutexLock(mutex)`, and `PR_PLATFORM_ALLOC_BYTES(size)` behaves as `prCoreLibcAllocBytes(size)`. A
binding of `PR_PLATFORM_FAIL` does not return. `PR_NORETURN` on the binding's declaration lets the compiler check that.

## Binding

A build names a header in `PR_USER_CONFIG`. It is a CMake variable that reaches every target, so that every file of the
library sees the same configuration. The header defines the macros of the groups that it binds. A binding may forward
to a backend's functions, to change part of their behavior and keep the rest. A binding that must choose at run time,
such as one for a binding's prebuilt library, calls through a table of its own and pays for the indirection alone.

A group that the configuration binds replaces its default, and the others keep theirs. A target that no family serves
has no default for the groups other than memory, so its configuration binds each of them that the build uses. The
source that uses a group checks that the group is bound, so a build that never compiles threads need not bind them.

The configuration changes what the library does, and never the layout of a type in its API. `PrThread`, `PrMutex` and
`PrCondition` hold fixed storage, which a platform's thread, mutex and condition fit in. So bindings built against the
plain headers agree with a configured library.

## Backends

A backend is a family of implementations under `lib/core/backends/<family>`, and its functions are named for the family.
Core's library holds every family that its target has, because each group is bound at compile time rather than through a
table, so a family's sources may call core's functions as any of core's do. A family declares each group in a header and
implements it in a source of its own, so that a program links only the groups it uses. For example,
`pr/core_posix_thread.h` declares `prCorePosixMutexLock`. A family serves every target that shares it, so one target's
defaults may come from several families. The `libc` and `mimalloc` families serve every target, the `posix` family
serves the Unix-like ones, the `apple` family serves clocks on Apple's targets from Mach's time base, and the `windows`
family serves Windows: it fails without the C runtime's abort dialog, maps files with file mapping objects, runs Win32
threads with slim reader/writer locks and condition variables, reads the performance counter and the precise system
time, and reserves with `VirtualAlloc`.

The clocks keep the promises of `pr/time.h` on every target. The performance counter is the cheapest reading that the
target has. The monotonic clock advances while the system sleeps: `mach_continuous_time` on Apple's targets,
`CLOCK_BOOTTIME` on Linux, and the performance counter on Windows.

A build without threads binds them to the `no_threads` family, which serves every target. No thread starts, a mutex
does nothing, and core fails a wait on a condition, because nothing else could signal it. The CMake option
`PR_PLATFORM_THREADS` chooses this family. It is off on the web, where a page shares memory between threads only if it
is served to allow that, and on everywhere else. `prThreadSupported` tells a program which it has. A task pool with no
threads runs its work on the thread that posts it.

Memory comes from the C library unless the build sets `PR_PLATFORM_MIMALLOC`, a CMake option that makes mimalloc the
default instead. A library leaves the choice of allocator to its embedder, and `./dev.sh` sets the option for
development and the benchmarks.

A family for one platform, such as `apple`, `linux`, `windows` or `wasm`, holds what that platform does its own way. On
that target, its defaults replace the shared family's for the groups it implements. `lib/core/src/config.h` chooses
each group's default by target. A target that has no default for a group binds the group in its configuration.

## Platform helpers

Core also provides helpers for one platform under `lib/core/platform/<platform>`, which it builds and exposes on that
platform only. A helper serves any code that calls that platform's own API, such as the backends of every library for
that platform, and binds no group. A helper is not a backend: what it does has no other implementation to choose, so a
library that includes it depends on core and on nothing that a program could replace. Its header and functions are
named for the platform: `pr/windows_string.h` converts between UTF-8 and the UTF-16 of Windows' wide-character
functions, `pr/apple_string.h` between UTF-8 and the CFString and NSString of Apple's frameworks and between paths and
file URLs, and on the web `prWebString`, of `web_string.js`, makes a page's string of a view that a JavaScript library
receives as its pointer and its size. A helper's header is C, as core's are, and may add C++ and Objective-C forms for
those languages' callers. A helper calls the platform for what the platform does, as these call its own conversions,
rather than reimplementing it.

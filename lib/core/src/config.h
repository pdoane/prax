// The build's configuration. The header named by PR_USER_CONFIG binds groups of the platform's facilities to the
// embedder's functions, and the default backends serve each group that the header leaves unbound. A group is bound
// whole. Each macro behaves as the backend function of its name, as PR_PLATFORM_MUTEX_LOCK behaves as
// prCorePosixMutexLock. The configuration never changes the layout of a type in the API, but it does change what the
// library does, so every file must see the same configuration.

#pragma once

#if defined(PR_USER_CONFIG)
#include PR_USER_CONFIG
#endif

// The three memory functions are bound together or not at all, and the optional two only with them.
#if defined(PR_PLATFORM_ALLOC_BYTES) != defined(PR_PLATFORM_FREE_BYTES)
#error "PR_PLATFORM_ALLOC_BYTES and PR_PLATFORM_FREE_BYTES are defined together or not at all"
#endif
#if defined(PR_PLATFORM_ALLOC_BYTES) != defined(PR_PLATFORM_REALLOC_BYTES)
#error "PR_PLATFORM_ALLOC_BYTES and PR_PLATFORM_REALLOC_BYTES are defined together or not at all"
#endif
#if defined(PR_PLATFORM_ALLOC_ZEROED_BYTES) && !defined(PR_PLATFORM_ALLOC_BYTES)
#error "PR_PLATFORM_ALLOC_ZEROED_BYTES comes with PR_PLATFORM_ALLOC_BYTES"
#endif
#if defined(PR_PLATFORM_USABLE_BYTES) && !defined(PR_PLATFORM_ALLOC_BYTES)
#error "PR_PLATFORM_USABLE_BYTES comes with PR_PLATFORM_ALLOC_BYTES"
#endif
#if defined(PR_PLATFORM_TRIM_BYTES) && !defined(PR_PLATFORM_ALLOC_BYTES)
#error "PR_PLATFORM_TRIM_BYTES comes with PR_PLATFORM_ALLOC_BYTES"
#endif

// The other groups are bound together or not at all. A build without the defaults binds each group that it compiles.
// The source that uses a group checks that the group is bound.
#if defined(PR_PLATFORM_FAIL) != defined(PR_PLATFORM_CRASH_HANDLER)
#error "PR_PLATFORM_FAIL and PR_PLATFORM_CRASH_HANDLER are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_THREAD_JOIN)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_THREAD_JOIN are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_THREAD_HARDWARE_COUNT)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_THREAD_HARDWARE_COUNT are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_THREAD_SLEEP)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_THREAD_SLEEP are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_THREAD_SUPPORTED)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_THREAD_SUPPORTED are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_MUTEX_INIT)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_MUTEX_INIT are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_MUTEX_RESET)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_MUTEX_RESET are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_MUTEX_LOCK)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_MUTEX_LOCK are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_MUTEX_UNLOCK)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_MUTEX_UNLOCK are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_CONDITION_INIT)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_CONDITION_INIT are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_CONDITION_RESET)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_CONDITION_RESET are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_CONDITION_WAIT)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_CONDITION_WAIT are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_CONDITION_WAIT_FOR)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_CONDITION_WAIT_FOR are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_CONDITION_SIGNAL)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_CONDITION_SIGNAL are defined together or not at all"
#endif
#if defined(PR_PLATFORM_THREAD_START) != defined(PR_PLATFORM_CONDITION_BROADCAST)
#error "PR_PLATFORM_THREAD_START and PR_PLATFORM_CONDITION_BROADCAST are defined together or not at all"
#endif
#if defined(PR_PLATFORM_FILE_MAP) != defined(PR_PLATFORM_FILE_UNMAP)
#error "PR_PLATFORM_FILE_MAP and PR_PLATFORM_FILE_UNMAP are defined together or not at all"
#endif
#if defined(PR_PLATFORM_FILE_MAP) != defined(PR_PLATFORM_FILE_INFO)
#error "PR_PLATFORM_FILE_MAP and PR_PLATFORM_FILE_INFO are defined together or not at all"
#endif
#if defined(PR_PLATFORM_FILE_MAP) != defined(PR_PLATFORM_FILE_SAME)
#error "PR_PLATFORM_FILE_MAP and PR_PLATFORM_FILE_SAME are defined together or not at all"
#endif
#if defined(PR_PLATFORM_FILE_MAP) != defined(PR_PLATFORM_FILE_EACH_ENTRY)
#error "PR_PLATFORM_FILE_MAP and PR_PLATFORM_FILE_EACH_ENTRY are defined together or not at all"
#endif
#if defined(PR_PLATFORM_FILE_MAP) != defined(PR_PLATFORM_FILE_WRITE)
#error "PR_PLATFORM_FILE_MAP and PR_PLATFORM_FILE_WRITE are defined together or not at all"
#endif
#if defined(PR_PLATFORM_FILE_MAP) != defined(PR_PLATFORM_FILE_REMOVE)
#error "PR_PLATFORM_FILE_MAP and PR_PLATFORM_FILE_REMOVE are defined together or not at all"
#endif
#if defined(PR_PLATFORM_FILE_MAP) != defined(PR_PLATFORM_FILE_CREATE_DIRECTORY)
#error "PR_PLATFORM_FILE_MAP and PR_PLATFORM_FILE_CREATE_DIRECTORY are defined together or not at all"
#endif
#if defined(PR_PLATFORM_FILE_MAP) != defined(PR_PLATFORM_FILE_USER_DIRECTORY)
#error "PR_PLATFORM_FILE_MAP and PR_PLATFORM_FILE_USER_DIRECTORY are defined together or not at all"
#endif
#if defined(PR_PLATFORM_FILE_MAP) != defined(PR_PLATFORM_FILE_STREAM_WRITE)
#error "PR_PLATFORM_FILE_MAP and PR_PLATFORM_FILE_STREAM_WRITE are defined together or not at all"
#endif
#if defined(PR_PLATFORM_FILE_MAP) != defined(PR_PLATFORM_FILE_STREAM_READ)
#error "PR_PLATFORM_FILE_MAP and PR_PLATFORM_FILE_STREAM_READ are defined together or not at all"
#endif
#if defined(PR_PLATFORM_FILE_MAP) != defined(PR_PLATFORM_FILE_WORKING_DIRECTORY)
#error "PR_PLATFORM_FILE_MAP and PR_PLATFORM_FILE_WORKING_DIRECTORY are defined together or not at all"
#endif
#if defined(PR_PLATFORM_VMEM_RESERVE) != defined(PR_PLATFORM_VMEM_COMMIT)
#error "PR_PLATFORM_VMEM_RESERVE and PR_PLATFORM_VMEM_COMMIT are defined together or not at all"
#endif
#if defined(PR_PLATFORM_VMEM_RESERVE) != defined(PR_PLATFORM_VMEM_FREE)
#error "PR_PLATFORM_VMEM_RESERVE and PR_PLATFORM_VMEM_FREE are defined together or not at all"
#endif
#if defined(PR_PLATFORM_TIME_MONOTONIC_MILLISECONDS) != defined(PR_PLATFORM_TIME_WALL_MILLISECONDS)
#error "PR_PLATFORM_TIME_MONOTONIC_MILLISECONDS and PR_PLATFORM_TIME_WALL_MILLISECONDS are defined together or not at all"
#endif
#if defined(PR_PLATFORM_TIME_MONOTONIC_MILLISECONDS) != defined(PR_PLATFORM_TIME_LOCAL_OFFSET_MILLISECONDS)
#error "PR_PLATFORM_TIME_MONOTONIC_MILLISECONDS and PR_PLATFORM_TIME_LOCAL_OFFSET_MILLISECONDS are defined together or not at all"
#endif

// The default backends of each target. The C library serves memory on every target, or mimalloc if the build chooses it
// (PR_PLATFORM_MIMALLOC). POSIX serves the other groups on the Unix-like targets, and the Windows family on Windows.
// Each group's backend is declared only if the build leaves the group unbound.
#if !defined(PR_PLATFORM_NO_DEFAULT) && !defined(__unix__) && !defined(__APPLE__) && !defined(_WIN32)
#error "this target has no default platform; bind every group in PR_USER_CONFIG"
#endif

// The default memory, if the build binds none.
#if !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_ALLOC_BYTES) && defined(PR_PLATFORM_MIMALLOC)
#include "pr/core_mimalloc.h"
#define PR_PLATFORM_ALLOC_BYTES(size) prCoreMimallocAllocBytes(size)
#define PR_PLATFORM_ALLOC_ZEROED_BYTES(size) prCoreMimallocAllocZeroedBytes(size)
#define PR_PLATFORM_REALLOC_BYTES(memory, size) prCoreMimallocReallocBytes(memory, size)
#define PR_PLATFORM_FREE_BYTES(memory) prCoreMimallocFreeBytes(memory)
#define PR_PLATFORM_USABLE_BYTES(memory) prCoreMimallocUsableBytes(memory)
#define PR_PLATFORM_TRIM_BYTES() prCoreMimallocTrimBytes()
#elif !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_ALLOC_BYTES)
#include "pr/core_libc.h"
#define PR_PLATFORM_ALLOC_BYTES(size) prCoreLibcAllocBytes(size)
#define PR_PLATFORM_ALLOC_ZEROED_BYTES(size) prCoreLibcAllocZeroedBytes(size)
#define PR_PLATFORM_REALLOC_BYTES(memory, size) prCoreLibcReallocBytes(memory, size)
#define PR_PLATFORM_FREE_BYTES(memory) prCoreLibcFreeBytes(memory)
#define PR_PLATFORM_USABLE_BYTES(memory) prCoreLibcUsableBytes(memory)
#define PR_PLATFORM_TRIM_BYTES() prCoreLibcTrimBytes()
#endif

// The default report of a failure, from Windows or POSIX, if the build binds none.
#if !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_FAIL) && defined(_WIN32)
#include "pr/core_windows_fail.h"
#define PR_PLATFORM_FAIL(message, file, line) prCoreWindowsFailed(message, file, line)
#define PR_PLATFORM_CRASH_HANDLER() prCoreWindowsCrashHandler()
#elif !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_FAIL)
#include "pr/core_posix_fail.h"
#define PR_PLATFORM_FAIL(message, file, line) prCorePosixFailed(message, file, line)
#define PR_PLATFORM_CRASH_HANDLER() prCorePosixCrashHandler()
#endif

// The default threads, mutexes and conditions, if the build binds none. A build without threads
// (PR_PLATFORM_NO_THREADS) takes the no_threads family, with the target's own sleep. Any other takes Windows or POSIX.
#if !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_THREAD_START) && defined(PR_PLATFORM_NO_THREADS)
#include "pr/core_no_threads_thread.h"
#define PR_PLATFORM_THREAD_SUPPORTED() false
#define PR_PLATFORM_THREAD_START(thread, run, context) prCoreNoThreadsThreadStart(thread, run, context)
#define PR_PLATFORM_THREAD_JOIN(thread) prCoreNoThreadsThreadJoin(thread)
#define PR_PLATFORM_THREAD_HARDWARE_COUNT() prCoreNoThreadsThreadHardwareCount()
#if defined(_WIN32)
#include "pr/core_windows_thread.h"
#define PR_PLATFORM_THREAD_SLEEP(milliseconds) prCoreWindowsThreadSleep(milliseconds)
#else
#include "pr/core_posix_thread.h"
#define PR_PLATFORM_THREAD_SLEEP(milliseconds) prCorePosixThreadSleep(milliseconds)
#endif
#define PR_PLATFORM_MUTEX_INIT(mutex) prCoreNoThreadsMutexInit(mutex)
#define PR_PLATFORM_MUTEX_RESET(mutex) prCoreNoThreadsMutexReset(mutex)
#define PR_PLATFORM_MUTEX_LOCK(mutex) prCoreNoThreadsMutexLock(mutex)
#define PR_PLATFORM_MUTEX_UNLOCK(mutex) prCoreNoThreadsMutexUnlock(mutex)
#define PR_PLATFORM_CONDITION_INIT(condition) prCoreNoThreadsConditionInit(condition)
#define PR_PLATFORM_CONDITION_RESET(condition) prCoreNoThreadsConditionReset(condition)
#define PR_PLATFORM_CONDITION_WAIT(condition, mutex) prCoreNoThreadsConditionWait(condition, mutex)
#define PR_PLATFORM_CONDITION_WAIT_FOR(condition, mutex, ms) prCoreNoThreadsConditionWaitFor(condition, mutex, ms)
#define PR_PLATFORM_CONDITION_SIGNAL(condition) prCoreNoThreadsConditionSignal(condition)
#define PR_PLATFORM_CONDITION_BROADCAST(condition) prCoreNoThreadsConditionBroadcast(condition)
#elif !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_THREAD_START) && defined(_WIN32)
#include "pr/core_windows_thread.h"
#define PR_PLATFORM_THREAD_SUPPORTED() true
#define PR_PLATFORM_THREAD_START(thread, run, context) prCoreWindowsThreadStart(thread, run, context)
#define PR_PLATFORM_THREAD_JOIN(thread) prCoreWindowsThreadJoin(thread)
#define PR_PLATFORM_THREAD_HARDWARE_COUNT() prCoreWindowsThreadHardwareCount()
#define PR_PLATFORM_THREAD_SLEEP(milliseconds) prCoreWindowsThreadSleep(milliseconds)
#define PR_PLATFORM_MUTEX_INIT(mutex) prCoreWindowsMutexInit(mutex)
#define PR_PLATFORM_MUTEX_RESET(mutex) prCoreWindowsMutexReset(mutex)
#define PR_PLATFORM_MUTEX_LOCK(mutex) prCoreWindowsMutexLock(mutex)
#define PR_PLATFORM_MUTEX_UNLOCK(mutex) prCoreWindowsMutexUnlock(mutex)
#define PR_PLATFORM_CONDITION_INIT(condition) prCoreWindowsConditionInit(condition)
#define PR_PLATFORM_CONDITION_RESET(condition) prCoreWindowsConditionReset(condition)
#define PR_PLATFORM_CONDITION_WAIT(condition, mutex) prCoreWindowsConditionWait(condition, mutex)
#define PR_PLATFORM_CONDITION_WAIT_FOR(condition, mutex, ms) prCoreWindowsConditionWaitFor(condition, mutex, ms)
#define PR_PLATFORM_CONDITION_SIGNAL(condition) prCoreWindowsConditionSignal(condition)
#define PR_PLATFORM_CONDITION_BROADCAST(condition) prCoreWindowsConditionBroadcast(condition)
#elif !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_THREAD_START)
#include "pr/core_posix_thread.h"
#define PR_PLATFORM_THREAD_SUPPORTED() true
#define PR_PLATFORM_THREAD_START(thread, run, context) prCorePosixThreadStart(thread, run, context)
#define PR_PLATFORM_THREAD_JOIN(thread) prCorePosixThreadJoin(thread)
#define PR_PLATFORM_THREAD_HARDWARE_COUNT() prCorePosixThreadHardwareCount()
#define PR_PLATFORM_THREAD_SLEEP(milliseconds) prCorePosixThreadSleep(milliseconds)
#define PR_PLATFORM_MUTEX_INIT(mutex) prCorePosixMutexInit(mutex)
#define PR_PLATFORM_MUTEX_RESET(mutex) prCorePosixMutexReset(mutex)
#define PR_PLATFORM_MUTEX_LOCK(mutex) prCorePosixMutexLock(mutex)
#define PR_PLATFORM_MUTEX_UNLOCK(mutex) prCorePosixMutexUnlock(mutex)
#define PR_PLATFORM_CONDITION_INIT(condition) prCorePosixConditionInit(condition)
#define PR_PLATFORM_CONDITION_RESET(condition) prCorePosixConditionReset(condition)
#define PR_PLATFORM_CONDITION_WAIT(condition, mutex) prCorePosixConditionWait(condition, mutex)
#define PR_PLATFORM_CONDITION_WAIT_FOR(condition, mutex, ms) prCorePosixConditionWaitFor(condition, mutex, ms)
#define PR_PLATFORM_CONDITION_SIGNAL(condition) prCorePosixConditionSignal(condition)
#define PR_PLATFORM_CONDITION_BROADCAST(condition) prCorePosixConditionBroadcast(condition)
#endif

// The default files, from Windows or POSIX, if the build binds none.
#if !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_FILE_MAP) && defined(_WIN32)
#include "pr/core_windows_file.h"
#define PR_PLATFORM_FILE_MAP(path, out) prCoreWindowsFileMap(path, out)
#define PR_PLATFORM_FILE_UNMAP(mapping) prCoreWindowsFileUnmap(mapping)
#define PR_PLATFORM_FILE_INFO(path, out) prCoreWindowsFileInfo(path, out)
#define PR_PLATFORM_FILE_SAME(a, b) prCoreWindowsFileSame(a, b)
#define PR_PLATFORM_FILE_EACH_ENTRY(path, fn, context) prCoreWindowsFileEachEntry(path, fn, context)
#define PR_PLATFORM_FILE_WRITE(path, bytes) prCoreWindowsFileWrite(path, bytes)
#define PR_PLATFORM_FILE_REMOVE(path) prCoreWindowsFileRemove(path)
#define PR_PLATFORM_FILE_CREATE_DIRECTORY(path) prCoreWindowsFileCreateDirectory(path)
#define PR_PLATFORM_FILE_USER_DIRECTORY(directory) prCoreWindowsFileUserDirectory(directory)
#define PR_PLATFORM_FILE_WORKING_DIRECTORY() prCoreWindowsFileWorkingDirectory()
#define PR_PLATFORM_FILE_STREAM_WRITE(stream, bytes) prCoreWindowsFileStreamWrite(stream, bytes)
#define PR_PLATFORM_FILE_STREAM_READ(stream, out, capacity) prCoreWindowsFileStreamRead(stream, out, capacity)
#elif !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_FILE_MAP)
#include "pr/core_posix_file.h"
#define PR_PLATFORM_FILE_MAP(path, out) prCorePosixFileMap(path, out)
#define PR_PLATFORM_FILE_UNMAP(mapping) prCorePosixFileUnmap(mapping)
#define PR_PLATFORM_FILE_INFO(path, out) prCorePosixFileInfo(path, out)
#define PR_PLATFORM_FILE_SAME(a, b) prCorePosixFileSame(a, b)
#define PR_PLATFORM_FILE_EACH_ENTRY(path, fn, context) prCorePosixFileEachEntry(path, fn, context)
#define PR_PLATFORM_FILE_WRITE(path, bytes) prCorePosixFileWrite(path, bytes)
#define PR_PLATFORM_FILE_REMOVE(path) prCorePosixFileRemove(path)
#define PR_PLATFORM_FILE_CREATE_DIRECTORY(path) prCorePosixFileCreateDirectory(path)
#define PR_PLATFORM_FILE_USER_DIRECTORY(directory) prCorePosixFileUserDirectory(directory)
#define PR_PLATFORM_FILE_WORKING_DIRECTORY() prCorePosixFileWorkingDirectory()
#define PR_PLATFORM_FILE_STREAM_WRITE(stream, bytes) prCorePosixFileStreamWrite(stream, bytes)
#define PR_PLATFORM_FILE_STREAM_READ(stream, out, capacity) prCorePosixFileStreamRead(stream, out, capacity)
#endif

// The default address space, from Windows or POSIX, if the build binds none.
#if !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_VMEM_RESERVE) && defined(_WIN32)
#include "pr/core_windows_vmem.h"
#define PR_PLATFORM_VMEM_RESERVE(size) prCoreWindowsVmemReserve(size)
#define PR_PLATFORM_VMEM_COMMIT(base, size) prCoreWindowsVmemCommit(base, size)
#define PR_PLATFORM_VMEM_FREE(base, size) prCoreWindowsVmemFree(base, size)
#elif !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_VMEM_RESERVE)
#include "pr/core_posix_vmem.h"
#define PR_PLATFORM_VMEM_RESERVE(size) prCorePosixVmemReserve(size)
#define PR_PLATFORM_VMEM_COMMIT(base, size) prCorePosixVmemCommit(base, size)
#define PR_PLATFORM_VMEM_FREE(base, size) prCorePosixVmemFree(base, size)
#endif

// The default clocks, from Windows or POSIX, if the build binds none.
#if !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_TIME_MONOTONIC_MILLISECONDS) && defined(_WIN32)
#include "pr/core_windows_time.h"
#define PR_PLATFORM_TIME_MONOTONIC_MILLISECONDS() prCoreWindowsTimeMonotonicMilliseconds()
#define PR_PLATFORM_TIME_WALL_MILLISECONDS() prCoreWindowsTimeWallMilliseconds()
#define PR_PLATFORM_TIME_LOCAL_OFFSET_MILLISECONDS(wall) prCoreWindowsTimeLocalOffsetMilliseconds(wall)
#elif !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_TIME_MONOTONIC_MILLISECONDS)
#include "pr/core_posix_time.h"
#define PR_PLATFORM_TIME_MONOTONIC_MILLISECONDS() prCorePosixTimeMonotonicMilliseconds()
#define PR_PLATFORM_TIME_WALL_MILLISECONDS() prCorePosixTimeWallMilliseconds()
#define PR_PLATFORM_TIME_LOCAL_OFFSET_MILLISECONDS(wall) prCorePosixTimeLocalOffsetMilliseconds(wall)
#endif

// The default environment, from Windows or POSIX, if the build binds none.
#if !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_ENVIRONMENT_VARIABLE) && defined(_WIN32)
#include "pr/core_windows_environment.h"
#define PR_PLATFORM_ENVIRONMENT_VARIABLE(name) prCoreWindowsEnvironmentVariable(name)
#define PR_PLATFORM_ENVIRONMENT_HOST_NAME() prCoreWindowsEnvironmentHostName()
#elif !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_ENVIRONMENT_VARIABLE)
#include "pr/core_posix_environment.h"
#define PR_PLATFORM_ENVIRONMENT_VARIABLE(name) prCorePosixEnvironmentVariable(name)
#define PR_PLATFORM_ENVIRONMENT_HOST_NAME() prCorePosixEnvironmentHostName()
#endif

// The default random bytes, from Windows or POSIX, if the build binds none.
#if !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_RANDOM_BYTES) && defined(_WIN32)
#include "pr/core_windows_random.h"
#define PR_PLATFORM_RANDOM_BYTES(out, size) prCoreWindowsRandomBytes(out, size)
#elif !defined(PR_PLATFORM_NO_DEFAULT) && !defined(PR_PLATFORM_RANDOM_BYTES)
#include "pr/core_posix_random.h"
#define PR_PLATFORM_RANDOM_BYTES(out, size) prCorePosixRandomBytes(out, size)
#endif

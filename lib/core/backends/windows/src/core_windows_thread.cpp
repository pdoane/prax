// The Windows backend of threads.

#include "pr/core_windows_thread.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdlib.h>

namespace pr_impl {

namespace {

// The stack of each thread (prThreadStart). It is reserved, and the system commits it as the thread uses it.
constexpr unsigned kStackBytes = 8u << 20;

// The function and context that a thread runs. The thread receives this structure as its argument, and frees it.
struct Start {
    PrThreadRunFn run;
    void* PR_NULLABLE context;
};

unsigned __stdcall begin(void* PR_NULLABLE argument) {
    Start copy = *static_cast<Start*>(argument);
    free(argument);
    copy.run(copy.context);
    return 0;
}

HANDLE* nativeOf(PrThread* thread) {
    static_assert(sizeof(HANDLE) <= sizeof(thread->storage));
    static_assert(alignof(HANDLE) <= alignof(PrThread));
    return reinterpret_cast<HANDLE*>(thread->storage);
}

SRWLOCK* nativeOf(PrMutex* mutex) {
    static_assert(sizeof(SRWLOCK) <= sizeof(mutex->storage));
    static_assert(alignof(SRWLOCK) <= alignof(PrMutex));
    return reinterpret_cast<SRWLOCK*>(mutex->storage);
}

CONDITION_VARIABLE* nativeOf(PrCondition* condition) {
    static_assert(sizeof(CONDITION_VARIABLE) <= sizeof(condition->storage));
    static_assert(alignof(CONDITION_VARIABLE) <= alignof(PrCondition));
    return reinterpret_cast<CONDITION_VARIABLE*>(condition->storage);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prCoreWindowsThreadStart(PrThread* thread, PrThreadRunFn run, void* PR_NULLABLE context) {
    Start* start = static_cast<Start*>(malloc(sizeof(Start)));
    if (start == nullptr)
        return false;
    *start = Start{run, context};
    // Unlike CreateThread, _beginthreadex sets up the C runtime's state for the thread.
    uintptr_t handle = _beginthreadex(nullptr, kStackBytes, begin, start, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (handle == 0) {
        free(start);
        return false;
    }
    *nativeOf(thread) = reinterpret_cast<HANDLE>(handle);
    return true;
}

void prCoreWindowsThreadJoin(PrThread* thread) {
    WaitForSingleObject(*nativeOf(thread), INFINITE);
    CloseHandle(*nativeOf(thread));
}

uint32_t prCoreWindowsThreadHardwareCount(void) {
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count > 0 ? uint32_t(count) : 1;
}

void prCoreWindowsThreadSleep(double milliseconds) {
    if (!(milliseconds > 0))
        return;
    // Sleep rounds up to the scheduler's tick, which is often 15.6 ms. A high-resolution timer waits about as long as
    // asked.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        Sleep(DWORD(milliseconds + 0.5));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG(milliseconds * 1e4);   // relative, in units of 100 ns
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, INFINITE);
    CloseHandle(timer);
}

void prCoreWindowsMutexInit(PrMutex* mutex) {
    InitializeSRWLock(nativeOf(mutex));
}

// A slim reader/writer lock holds no resources.
void prCoreWindowsMutexReset(PrMutex* mutex) {
    (void)mutex;
}

void prCoreWindowsMutexLock(PrMutex* mutex) {
    AcquireSRWLockExclusive(nativeOf(mutex));
}

void prCoreWindowsMutexUnlock(PrMutex* mutex) {
    ReleaseSRWLockExclusive(nativeOf(mutex));
}

void prCoreWindowsConditionInit(PrCondition* condition) {
    InitializeConditionVariable(nativeOf(condition));
}

// A condition variable holds no resources.
void prCoreWindowsConditionReset(PrCondition* condition) {
    (void)condition;
}

void prCoreWindowsConditionWait(PrCondition* condition, PrMutex* mutex) {
    SleepConditionVariableSRW(nativeOf(condition), nativeOf(mutex), INFINITE, 0);
}

bool prCoreWindowsConditionWaitFor(PrCondition* condition, PrMutex* mutex, double milliseconds) {
    // A wait past a day is a day, which keeps it below INFINITE. A caller that waits for a state tests it again after a
    // wake in any case.
    double bounded = milliseconds == milliseconds && milliseconds < 86400000 ? milliseconds : 86400000;
    DWORD wait = DWORD(bounded + 0.5);
    return SleepConditionVariableSRW(nativeOf(condition), nativeOf(mutex), wait, 0) != 0 ||
        GetLastError() != ERROR_TIMEOUT;
}

void prCoreWindowsConditionSignal(PrCondition* condition) {
    WakeConditionVariable(nativeOf(condition));
}

void prCoreWindowsConditionBroadcast(PrCondition* condition) {
    WakeAllConditionVariable(nativeOf(condition));
}

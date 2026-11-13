#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/base.h"
#include "pr/base64.h"
#include "pr/checksum.h"
#include "pr/core_libc.h"
#include "pr/core_mimalloc.h"
#if defined(_WIN32)
#include "pr/core_windows_fail.h"
#include "pr/core_windows_file.h"
#include "pr/core_windows_random.h"
#include "pr/core_windows_thread.h"
#include "pr/core_windows_time.h"
#include "pr/core_windows_vmem.h"
#else
#include "pr/core_posix_fail.h"
#include "pr/core_posix_file.h"
#include "pr/core_posix_random.h"
#include "pr/core_posix_thread.h"
#include "pr/core_posix_time.h"
#include "pr/core_posix_vmem.h"
#endif
#if defined(__APPLE__)
#include "pr/core_apple_time.h"
#endif
#include "pr/date_time.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/float_convert.h"
#include "pr/hash.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/pixels.h"
#include "pr/random.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/utf8.h"
#include "pr/vec.h"
#include "pr/vmem.h"

int prTestCoreCApi(void);

// The public headers compiled as C. Allocates through the mimalloc backend, as a binding that forwards to it would.
int prTestCoreCApi(void) {
    void* memory = prCoreMimallocAllocBytes(16);
    memory = prCoreMimallocReallocBytes(memory, 64);
    size_t usable = prCoreMimallocUsableBytes(memory);
    prCoreMimallocFreeBytes(memory);

    char* bytes = (char*)prMemoryAllocZeroed(8);
    bytes = (char*)prMemoryRealloc(bytes, 32);
    PR_ASSERT(bytes[7] == 0);
    prMemoryFree(bytes);

    PrMutex mutex;
    prMutexInit(&mutex);
    prMutexLock(&mutex);
    prMutexUnlock(&mutex);
    prMutexReset(&mutex);
    double now = prTimeMonotonicMilliseconds();
    PrTimePerf start = prTimePerfNow();
    double elapsed = prTimePerfMilliseconds(start, prTimePerfNow());
    PrDateTime epoch = prDateTimeFromDays(0);
    PrDateTime today = prDateTimeFromSystem(prTimeSystemNow(), 0);

    PrTextBuffer text = {0};
    prTextBufferAppend(&text, PR_SV("Platform "));
    prTextBufferAppendU64(&text, 42, 10);
    prTextBufferAsciiLower(&text);
    bool texts = prStringEndsWith(prTextBufferView(&text), PR_SV(" 42")) && prAsciiIsDigit('4') &&
        prHashBytes(text.data, text.size, 0) != 0 && prF64Abs(-kPrF64Pi) == kPrF64Pi;
    prTextBufferReset(&text);

    PrStringView name = PR_SV("platform");
    PrVec2 point = {1, 2};
    return !texts || usable < 64 || name.size != 8 || point.y != 2 || !(now >= 0) || prThreadHardwareCount() == 0 ||
        epoch.year != 1970 || !(elapsed >= 0) || today.year < 2026;
}

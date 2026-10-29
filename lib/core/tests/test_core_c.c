#include "pr/assert.h"
#include "pr/base.h"
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
#include "pr/file.h"
#include "pr/float16.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/pixels.h"
#include "pr/random.h"
#include "pr/status.h"
#include "pr/string.h"
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

    PrStringView name = PR_SV("platform");
    PrVec2 point = {1, 2};
    return usable < 64 || name.size != 8 || point.y != 2 || !(now >= 0) || prThreadHardwareCount() == 0;
}

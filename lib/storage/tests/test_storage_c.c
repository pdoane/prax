#include "pr/storage_disk.h"
#include "pr/storage_memory.h"

int prTestStorageCApi(void);

// The public headers compiled as C. A store in memory holds a record.
int prTestStorageCApi(void) {
    PrStorage* storage = prStorageMemoryCreate(16, PrStorageEviction_None);
    const uint8_t bytes[] = {1, 2, 3};
    PrByteView view = {bytes, sizeof(bytes)};
    int result = !prStorageWrite(storage, PR_SV("key"), view) || prStorageSize(storage) != 6 || prStorageCount(storage) != 1;
    prStorageDestroy(storage);
    prStorageDestroy(NULL);
    return result;
}

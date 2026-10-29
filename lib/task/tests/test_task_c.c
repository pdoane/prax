#include "pr/task.h"

int prTestTaskCApi(void);

// The public headers compile as C. Destroying no pool does nothing.
int prTestTaskCApi(void) {
    PrTaskFn fn = NULL;
    prTaskPoolDestroy(NULL);
    return fn != NULL;
}

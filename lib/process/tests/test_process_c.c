#include "pr/process.h"
#if defined(_WIN32)
#include "pr/process_windows.h"
#else
#include "pr/process_posix.h"
#endif

int prTestProcessCApi(void);

// The public headers compiled as C. Destroying no process or launcher does nothing.
int prTestProcessCApi(void) {
    PrProcessDesc desc = {0};
    desc.errors = PrProcessErrors_Discard;
    prProcessDestroy(NULL);
#if defined(_WIN32)
    PrProcessLauncher* launcher = prProcessWindowsCreate();
#else
    PrProcessLauncher* launcher = prProcessPosixCreate();
#endif
    prProcessLauncherDestroy(launcher);
    prProcessLauncherDestroy(NULL);
    return desc.argumentCount == 0 ? 0 : 1;
}

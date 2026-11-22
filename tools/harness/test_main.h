// What the main() of the tests gives the tests: the end of their run, where the things that the cases share are torn
// down.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// Registers `fn` to be called with `context` when the binary's run of its tests ends, before its statics are destroyed.
// What the tests share, such as a GPU device, then outlives every case that the run chose. The calls go in the reverse
// order of the registrations.
void atTestRunEnd(void (*fn)(void* PR_NULLABLE context), void* PR_NULLABLE context);

} // namespace harness

PR_ASSUME_NONNULL_END

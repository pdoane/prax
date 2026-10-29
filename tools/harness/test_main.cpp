// The main() of every module's tests: the crash handler, then doctest's run of the binary's tests.

#include "pr/assert.h"

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

using namespace pr;

int main(int argc, char** argv) {
    prCrashHandler();
    return doctest::Context(argc, argv).run();
}

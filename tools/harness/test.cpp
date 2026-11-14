// What tools/harness/test.h gives the tests beyond doctest: a folder of each binary's own, a word of its own, and the
// files that tests read. It compiles once, so it may reach the standard library directly, as test.h may not.

#include "tools/harness/test.h"

#include "pr/environment.h"
#include "pr/file.h"

#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace pr;

namespace {

// The binary's folder, once it is made, which is removed as the binary ends. A file that a program of a test still
// holds open stays, and a later binary of the same process id removes it.
struct Folder {
    std::filesystem::path path;

    ~Folder() {
        std::error_code ignored;
        if (!path.empty())
            std::filesystem::remove_all(path, ignored);
    }
};

Folder gFolder;

} // namespace

namespace harness {

bool gpuTests() {
    PrRawString value = prEnvironmentVariable("PR_TEST_NO_GPU");
    bool set = !prRawStringIsNull(&value);
    prRawStringReset(&value);
    return !set;
}

String runTag() {
#if defined(_WIN32)
    return String(std::to_string(_getpid()).c_str());
#else
    return String(std::to_string(getpid()).c_str());
#endif
}

String testFolder() {
    if (gFolder.path.empty()) {
        std::filesystem::path path = std::filesystem::temp_directory_path() / "prax-tests" / runTag().data();
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
        std::filesystem::create_directories(path);
        // The real path: programs that report their directory resolve links, as macOS's /var to /private/var. Under
        // Node.js, Emscripten's paths are POSIX paths, and it resolves a Windows path with a drive as a relative path.
#if defined(__EMSCRIPTEN__)
        gFolder.path = path;
#else
        gFolder.path = std::filesystem::canonical(path);
#endif
    }
    return String(gFolder.path.generic_string().c_str());
}

Array<uint8_t> readFile(PrStringView path) {
    PrByteView mapping{};
    REQUIRE_MESSAGE(prFileMap(path, &mapping), "cannot read ", String(path));
    Array<uint8_t> bytes;
    bytes.append(mapping.data, uint32_t(mapping.size));
    prFileUnmap(&mapping);
    return bytes;
}

String readText(PrStringView path) {
    Array<uint8_t> bytes = readFile(path);
    return String(PrStringView{reinterpret_cast<const char*>(bytes.begin()), bytes.count()});
}

} // namespace harness

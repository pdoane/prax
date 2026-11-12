// What tools/harness/test.h gives the tests beyond doctest: a folder of each binary's own, and a word of its own.

#include "tools/harness/test.h"

#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

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

std::string runTag() {
#if defined(_WIN32)
    return std::to_string(_getpid());
#else
    return std::to_string(getpid());
#endif
}

std::string testFolder() {
    if (gFolder.path.empty()) {
        std::filesystem::path path = std::filesystem::temp_directory_path() / "prax-tests" / runTag();
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
        std::filesystem::create_directories(path);
        // The real path: programs that report their directory resolve links, as macOS's /var to /private/var.
        gFolder.path = std::filesystem::canonical(path);
    }
    return gFolder.path.generic_string();
}

} // namespace harness

#include "tools/harness/harness_streams.h"

#include <initializer_list>
#include <stdio.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>

using namespace pr;

#endif

namespace harness {

// The runner's answers, which go to the standard output as it was when the capture opened, and the temporary files that
// a run's writes go to.
struct HarnessCapture {
    FILE* answers;
    FILE* out;
    FILE* err;
    int savedErr;
};

namespace {

// The POSIX functions of the C runtime, which Windows' runtime names with an underscore.
#if defined(_WIN32)
int duplicate(int descriptor) {
    return _dup(descriptor);
}
void replace(int from, int into) {
    _dup2(from, into);
}
void closeDescriptor(int descriptor) {
    _close(descriptor);
}
FILE* streamOf(int descriptor) {
    return _fdopen(descriptor, "wb");
}
int descriptorOf(FILE* file) {
    return _fileno(file);
}
void empty(FILE* file) {
    _chsize_s(_fileno(file), 0);
}
#else
int duplicate(int descriptor) {
    return dup(descriptor);
}
void replace(int from, int into) {
    dup2(from, into);
}
void closeDescriptor(int descriptor) {
    close(descriptor);
}
FILE* streamOf(int descriptor) {
    return fdopen(descriptor, "wb");
}
int descriptorOf(FILE* file) {
    return fileno(file);
}
void empty(FILE* file) {
    ftruncate(fileno(file), 0);
}
#endif

// Reads the whole of the temporary file `file` and empties it.
std::string drain(FILE* file) {
    std::string text;
    fflush(file);
    rewind(file);
    char buffer[4096];
    for (size_t n; (n = fread(buffer, 1, sizeof(buffer), file)) != 0;)
        text.append(buffer, n);
    rewind(file);
    empty(file);
    return text;
}

} // namespace

void harnessStreamsBinary() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
}

HarnessCapture* PR_NULLABLE harnessCaptureOpen() {
    int output = duplicate(descriptorOf(stdout));
    if (output < 0)
        return nullptr;
    HarnessCapture* capture = new HarnessCapture{streamOf(output), tmpfile(), tmpfile(), -1};
    if (capture->answers == nullptr || capture->out == nullptr || capture->err == nullptr) {
        harnessCaptureClose(capture);
        return nullptr;
    }
    return capture;
}

void harnessCaptureBegin(HarnessCapture* capture) {
    fflush(stdout);
    fflush(stderr);
    capture->savedErr = duplicate(descriptorOf(stderr));
    replace(descriptorOf(capture->out), descriptorOf(stdout));
    replace(descriptorOf(capture->err), descriptorOf(stderr));
}

void harnessCaptureEnd(HarnessCapture* capture, std::string* out, std::string* err) {
    fflush(stdout);
    fflush(stderr);
    replace(capture->savedErr, descriptorOf(stderr));
    closeDescriptor(capture->savedErr);
    capture->savedErr = -1;
    *out = drain(capture->out);
    *err = drain(capture->err);
}

void harnessCaptureAnswer(HarnessCapture* capture, int status, const std::string& out, const std::string& err) {
    fprintf(capture->answers, "%d %zu %zu\n", status, out.size(), err.size());
    fwrite(out.data(), 1, out.size(), capture->answers);
    fwrite(err.data(), 1, err.size(), capture->answers);
    fflush(capture->answers);
}

void harnessCaptureClose(HarnessCapture* PR_NULLABLE capture) {
    if (capture == nullptr)
        return;
    for (FILE* file : {capture->answers, capture->out, capture->err}) {
        if (file != nullptr)
            fclose(file);
    }
    delete capture;
}

} // namespace harness

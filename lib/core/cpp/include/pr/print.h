// Printing: formatted text (pr/format.h) written to the process's output and error streams.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/file.h"
#include "pr/format.h"
#include "pr/string_ops.h"
#include "pr/writer.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// A writer to one of the process's standard streams, through a buffer of its own. The text reaches the stream when the
// writer is flushed, which its destructor does.
struct StreamWriter : Writer {
    explicit StreamWriter(PrFileStream target) :
            Writer{.drain = writeTo, .context = this, .buffer = storage, .capacity = sizeof(storage)}, stream(target) {}
    ~StreamWriter() { flush(); }
    StreamWriter(const StreamWriter&) = delete;
    StreamWriter& operator=(const StreamWriter&) = delete;

private:
    PrFileStream stream;
    char storage[1024];

    static bool writeTo(void* PR_NULLABLE context, PrStringView text) {
        PrByteView bytes{prStringBytes(text), text.size};
        return prFileStreamWrite(static_cast<StreamWriter*>(context)->stream, bytes);
    }
};

// Writes `format`, with its fields replaced by `arguments`, to `stream`. The text goes to the stream before the call
// returns. Returns false if the stream cannot be written.
template <typename... Arguments>
bool printTo(PrFileStream stream, PrStringView format, const Arguments&... arguments) {
    StreamWriter writer(stream);
    formatTo(writer, format, arguments...);
    return writer.flush();
}

// Write to the output stream, and to the error stream.
template <typename... Arguments>
bool print(PrStringView format, const Arguments&... arguments) {
    return printTo(PrFileStream_Output, format, arguments...);
}
template <typename... Arguments>
bool printError(PrStringView format, const Arguments&... arguments) {
    return printTo(PrFileStream_Error, format, arguments...);
}

} // namespace pr

PR_ASSUME_NONNULL_END

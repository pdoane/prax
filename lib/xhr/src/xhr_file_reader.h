// FileReader's state.

#pragma once

#include "pr/xhr_file_reader.h"

#include "dom/src/dom_event.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A reader, whose read runs in tasks of its client. Each task checks `generation`, so that a read that ended runs no
// more.
struct XhrFileReader : DomCountedTarget {
    PrXhrClient client;
    PrXhrFileReaderPendingFn PR_NULLABLE pending;
    void* PR_NULLABLE context;
    const PrFetchBlob* PR_NULLABLE blob;    // the blob of the read under way
    const PrFetchBlob* PR_NULLABLE result;  // the blob of an ArrayBuffer result
    PrRawString encoding;
    PrRawString text;
    uint32_t resultVersion;
    uint32_t generation;                    // changes when a read ends, which its waiting tasks then see
    uint32_t tasks;                         // the tasks that are queued
    PrXhrFileReaderState state;
    PrXhrFileReaderFormat format;
    uint8_t step;                           // the step that the next task runs
    bool hasResult;
};

} // namespace pr_impl

// C opaque types
struct PrXhrFileReader : pr_impl::XhrFileReader {};

PR_ASSUME_NONNULL_END

// The standard streams of a harness: bytes in and out on every platform, and what a served run writes to them.

#pragma once

#include "pr/base.h"

#include <string>

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// Makes the standard streams carry bytes unchanged, which on Windows they do not by default, so that a runner that
// counts the bytes of an answer reads what the harness wrote.
void harnessStreamsBinary();

// Answers to a runner on the standard output, while each run's own writes to the standard output and the standard error
// are captured apart from the answers.
struct HarnessCapture;

// Begins answering on the standard output as it is now. Returns null if the streams cannot be duplicated.
HarnessCapture* PR_NULLABLE harnessCaptureOpen();

// Sends what follows to the standard output and the standard error into the capture, until harnessCaptureEnd.
void harnessCaptureBegin(HarnessCapture* capture);

// Restores the standard error, and moves what the run wrote to each stream since harnessCaptureBegin into `out` and
// `err`.
void harnessCaptureEnd(HarnessCapture* capture, std::string* out, std::string* err);

// Writes an answer to the runner: the run's status, and what it wrote to each stream.
void harnessCaptureAnswer(HarnessCapture* capture, int status, const std::string& out, const std::string& err);

// Stops answering, and frees the capture.
void harnessCaptureClose(HarnessCapture* PR_NULLABLE capture);

} // namespace harness

PR_ASSUME_NONNULL_END

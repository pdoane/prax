// Processes over pipes (process_shared_piped.h): the caller's reads and writes, and what the backend's thread records.

#include "process/backends/shared/src/process_shared_piped.h"

#include "pr/assert.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

void wakeCaller(ProcessPiped* process) {
    if (process->wake != nullptr)
        process->wake(process->wakeContext);
}

void writeProcess(PrProcess* base, PrByteView bytes) {
    ProcessPiped* process = static_cast<ProcessPiped*>(base);
    prMutexLock(&process->mutex);
    bool dropped = process->inputClosing || process->inputBroken;
    if (!dropped) {
        if (process->queuedStart != 0 && process->queuedStart == process->queued.count) {
            process->queued.clear();
            process->queuedStart = 0;
        }
        PR_ASSERT(bytes.size <= UINT32_MAX - process->queued.count);
        process->queued.append(bytes.data, uint32_t(bytes.size));
    }
    prMutexUnlock(&process->mutex);
    if (!dropped)
        process->notify(process);
}

void closeInput(PrProcess* base) {
    ProcessPiped* process = static_cast<ProcessPiped*>(base);
    prMutexLock(&process->mutex);
    bool closing = !process->inputClosing;
    process->inputClosing = true;
    prMutexUnlock(&process->mutex);
    if (closing)
        process->notify(process);
}

size_t readProcess(PrProcess* base, PrProcessStream stream, uint8_t* PR_NULLABLE output, size_t capacity) {
    ProcessPiped* process = static_cast<ProcessPiped*>(base);
    prMutexLock(&process->mutex);
    ProcessReceived& received = process->received[stream];
    bool full = received.bytes.count - received.start >= kProcessReceivedMaxBytes;
    uint32_t available = received.bytes.count - received.start;
    uint32_t taken = capacity < available ? uint32_t(capacity) : available;
    if (taken != 0)
        prMemCopy(output, received.bytes.data + received.start, taken);
    received.start += taken;
    if (received.start == received.bytes.count) {
        received.bytes.clear();
        received.start = 0;
    }
    bool freed = full && received.bytes.count - received.start < kProcessReceivedMaxBytes;
    prMutexUnlock(&process->mutex);
    if (freed)
        process->notify(process);
    return taken;
}

PrProcessInfo infoOf(const PrProcess* base) {
    ProcessPiped* process = const_cast<ProcessPiped*>(static_cast<const ProcessPiped*>(base));
    prMutexLock(&process->mutex);
    const ProcessReceived& output = process->received[PrProcessStream_Output];
    PrProcessInfo info{
        .outputEnded = output.ended && output.start == output.bytes.count,
        .exited = process->exited,
        .signaled = process->signaled,
        .code = process->code,
    };
    prMutexUnlock(&process->mutex);
    return info;
}

} // namespace

void processPipedInit(ProcessPiped* process, const PrProcessDesc* desc, ProcessPipedNotifyFn notify) {
    process->writeFn = writeProcess;
    process->closeInputFn = closeInput;
    process->readFn = readProcess;
    process->infoFn = infoOf;
    process->notify = notify;
    process->wake = desc->wake;
    process->wakeContext = desc->wakeContext;
    process->receive = desc->receive;
    process->receiveContext = desc->receiveContext;
    prMutexInit(&process->mutex);
    bool errorPiped = desc->streams == PrProcessStreams_Pipes && desc->errors == PrProcessErrors_Pipe;
    process->received[PrProcessStream_Error].ended = !errorPiped;
}

void processPipedReset(ProcessPiped* process) {
    for (ProcessReceived& received : process->received)
        received.bytes.reset();
    process->queued.reset();
    prMutexReset(&process->mutex);
}

bool processPipedReceive(ProcessPiped* process, PrProcessStream stream, const uint8_t* data, size_t size) {
    PR_ASSERT(size <= kProcessReadChunkBytes);
    if (process->receive != nullptr) {
        process->receive(process->receiveContext, process, stream, PrByteView{data, size});
        return true;
    }
    prMutexLock(&process->mutex);
    ProcessReceived& received = process->received[stream];
    bool waiting = received.bytes.count > received.start;
    if (received.start != 0 && !waiting) {
        received.bytes.clear();
        received.start = 0;
    }
    received.bytes.append(data, uint32_t(size));
    bool room = received.bytes.count - received.start < kProcessReceivedMaxBytes;
    prMutexUnlock(&process->mutex);
    if (!waiting)
        wakeCaller(process);
    return room;
}

bool processPipedHasRoom(ProcessPiped* process, PrProcessStream stream) {
    prMutexLock(&process->mutex);
    const ProcessReceived& received = process->received[stream];
    bool room = received.bytes.count - received.start < kProcessReceivedMaxBytes;
    prMutexUnlock(&process->mutex);
    return room;
}

void processPipedEnd(ProcessPiped* process, PrProcessStream stream) {
    prMutexLock(&process->mutex);
    process->received[stream].ended = true;
    prMutexUnlock(&process->mutex);
    wakeCaller(process);
}

void processPipedExit(ProcessPiped* process, bool signaled, int32_t code) {
    prMutexLock(&process->mutex);
    process->exited = true;
    process->signaled = signaled;
    process->code = code;
    prMutexUnlock(&process->mutex);
    wakeCaller(process);
}

} // namespace pr_impl

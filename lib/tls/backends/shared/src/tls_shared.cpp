// What the TLS backends that run an engine over a transport share (tls_shared.h).

#include "tls/backends/shared/src/tls_shared.h"

#include "pr/assert.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

void deliverFail(void* PR_NULLABLE context) {
    TlsSharedSession* session = static_cast<TlsSharedSession*>(context);
    tlsSharedDeliver(session, PrNetStreamEvent_Fail);
    tlsSharedRelease(session);
}

// Resumes reading, from a task, once the client read enough to make room: first the records that the engine holds, then
// the transport's bytes.
void resume(void* PR_NULLABLE context) {
    TlsSharedSession* session = static_cast<TlsSharedSession*>(context);
    if (!session->dead && session->state == TlsSharedState_Open) {
        session->engine->advance(session);
        if (!session->dead && session->state == TlsSharedState_Open)
            session->engine->drainTransport(session);
    }
    tlsSharedRelease(session);
}

} // namespace

void tlsSharedHold(TlsSharedSession* session) {
    ++session->holds;
}

void tlsSharedRelease(TlsSharedSession* session) {
    PR_ASSERT(session->holds != 0);
    if (--session->holds == 0 && session->dead)
        session->engine->free(session);
}

void tlsSharedDeliver(TlsSharedSession* session, PrNetStreamEvent event) {
    if (!session->dead && session->stream.event != nullptr)
        session->stream.event(session->stream.context, &session->stream, event);
}

void tlsSharedFail(TlsSharedSession* session, PrNetError error, PrTlsFailure failure) {
    if (session->state == TlsSharedState_Failed)
        return;
    session->state = TlsSharedState_Failed;
    session->error = error;
    session->failure = failure;
    tlsSharedDeliver(session, PrNetStreamEvent_Fail);
}

void tlsSharedFailLater(TlsSharedSession* session, PrNetError error, PrTlsFailure failure) {
    if (session->state == TlsSharedState_Failed)
        return;
    session->state = TlsSharedState_Failed;
    session->error = error;
    session->failure = failure;
    tlsSharedHold(session);
    prNetPost(session->stream.net, deliverFail, session);
}

void tlsSharedTransportEvent(void* PR_NULLABLE context, PrNetStream* transport, PrNetStreamEvent event) {
    TlsSharedSession* session = static_cast<TlsSharedSession*>(context);
    tlsSharedHold(session);
    switch (event) {
        case PrNetStreamEvent_Open:
            break;
        case PrNetStreamEvent_Readable:
            session->engine->drainTransport(session);
            break;
        case PrNetStreamEvent_Writable:
            if (session->writableOwed && session->unsent.count == 0 && prNetStreamQueued(transport) == 0) {
                session->writableOwed = false;
                tlsSharedDeliver(session, PrNetStreamEvent_Writable);
            }
            break;
        case PrNetStreamEvent_End:
            session->transportEnded = true;
            session->engine->drainTransport(session);
            break;
        case PrNetStreamEvent_Fail:
            tlsSharedFail(session, prNetStreamError(transport), PrTlsFailure_None);
            break;
    }
    tlsSharedRelease(session);
}

size_t tlsSharedRead(PrNetStream* stream, uint8_t* output, size_t capacity) {
    TlsSharedSession* session = tlsSharedSessionFromStream(stream);
    uint32_t available = session->received.count - session->receivedStart;
    uint32_t taken = capacity < available ? uint32_t(capacity) : available;
    memCopy(output, session->received.data + session->receivedStart, taken);
    session->receivedStart += taken;
    if (session->receivedStart == session->received.count) {
        session->received.clear();
        session->receivedStart = 0;
    }
    if (session->stalled && session->received.count - session->receivedStart < kTlsSharedReceivedMaxBytes) {
        session->stalled = false;
        tlsSharedHold(session);
        prNetPost(session->stream.net, resume, session);
    }
    return taken;
}

PrStatus tlsSharedFinish(PrNetStream* stream) {
    TlsSharedSession* session = tlsSharedSessionFromStream(stream);
    if (session->finishing)
        return PrStatus_Ok;
    session->finishing = true;
    if (session->state == TlsSharedState_Open)
        session->engine->sendCloseNotify(session);
    return PrStatus_Ok;
}

size_t tlsSharedQueued(const PrNetStream* stream) {
    const TlsSharedSession* session = tlsSharedSessionFromStream(stream);
    size_t queued = session->unsent.count;
    if (session->transport != nullptr)
        queued += prNetStreamQueued(session->transport);
    return queued;
}

PrNetError tlsSharedError(const PrNetStream* stream) {
    return tlsSharedSessionFromStream(stream)->error;
}

PrTlsSessionInfo tlsSharedInfo(const PrTlsSession* tlsSession) {
    const TlsSharedSession* session = static_cast<const TlsSharedSession*>(tlsSession);
    return PrTlsSessionInfo{
        .version = session->version,
        .failure = session->failure,
        .protocol = session->protocol,
    };
}

} // namespace pr_impl

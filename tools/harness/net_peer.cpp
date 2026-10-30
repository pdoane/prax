// The peer that tools/harness/net_peer.py runs for the tests of lib/net and lib/tls, and a probe that drives one stream
// from a test's thread.

#include "tools/harness/net_peer.h"

#include "pr/mem.h"
#include "pr/string_ops.h"

#include <stdlib.h>

using namespace pr;

namespace harness {

namespace {

// Returns the port that follows `name` and a space at the start of a line of `ports`, or zero.
uint16_t portOf(PrStringView ports, PrStringView name) {
    size_t line = 0;
    while (line < ports.size) {
        size_t end = line;
        while (end < ports.size && ports.data[end] != '\n')
            ++end;
        PrStringView text{ports.data + line, end - line};
        if (text.size > name.size && PrStringView{text.data, name.size} == name && text.data[name.size] == ' ') {
            uint32_t port = 0;
            for (size_t i = name.size + 1; i < text.size && text.data[i] >= '0' && text.data[i] <= '9'; ++i)
                port = port * 10 + uint32_t(text.data[i] - '0');
            return port <= 65535 ? uint16_t(port) : 0;
        }
        line = end + 1;
    }
    return 0;
}

void signal(NetProbe* probe) {
    prConditionBroadcast(&probe->changed);
    prMutexUnlock(&probe->mutex);
}

void event(void* PR_NULLABLE context, PrNetStream* stream, PrNetStreamEvent kind) {
    NetProbe* probe = static_cast<NetProbe*>(context);
    if (kind == PrNetStreamEvent_Open && probe->script.inspect != nullptr)
        probe->script.inspect(probe->script.context, stream);
    prMutexLock(&probe->mutex);
    probe->events.push(kind);
    if (kind == PrNetStreamEvent_Readable) {
        uint8_t chunk[4096];
        for (size_t got = prNetStreamRead(stream, chunk, sizeof(chunk)); got != 0; got =
                prNetStreamRead(stream, chunk, sizeof(chunk)))
            probe->received.append(chunk, uint32_t(got));
    }
    bool done = kind == PrNetStreamEvent_End || kind == PrNetStreamEvent_Fail;
    if (done)
        probe->error = prNetStreamError(stream);
    prMutexUnlock(&probe->mutex);
    if (done && probe->script.inspect != nullptr)
        probe->script.inspect(probe->script.context, stream);
    if (done) {
        prMutexLock(&probe->mutex);
        probe->done = true;
        signal(probe);
    }
}

void start(void* PR_NULLABLE context) {
    NetProbe* probe = static_cast<NetProbe*>(context);
    PrNetStream* stream = nullptr;
    PrStatus status = probe->script.open(probe->script.context, probe->net, event, probe, &stream);
    PrStatus finished = PrStatus_Ok;
    if (stream != nullptr) {
        prNetStreamWrite(stream, probe->script.payload);
        if (probe->script.finish)
            finished = prNetStreamFinish(stream);
    }
    prMutexLock(&probe->mutex);
    probe->stream = stream;
    probe->finished = finished;
    probe->status = status;
    if (status != PrStatus_Ok)
        probe->done = true;
    signal(probe);
}

void destroy(void* PR_NULLABLE context) {
    NetProbe* probe = static_cast<NetProbe*>(context);
    prNetStreamDestroy(probe->stream);
    prMutexLock(&probe->mutex);
    probe->stream = nullptr;
    probe->destroyed = true;
    signal(probe);
}

} // namespace

bool netPeerOpen(NetPeer* out) {
    *out = NetPeer{};
    const char* directory = getenv("PR_NET_PEER");
    if (directory == nullptr)
        return false;
    char path[1024];
    size_t size = 0;
    while (directory[size] != 0 && size + 16 < sizeof(path)) {
        path[size] = directory[size];
        ++size;
    }
    memCopy(path + size, "/ports", 6);
    PrFileMapping ports{};
    if (!prFileMap(PrStringView{path, size + 6}, &ports))
        return false;
    PrStringView text{reinterpret_cast<const char*>(ports.data), ports.size};
    out->tcpEcho = portOf(text, "tcp_echo");
    out->tlsEcho = portOf(text, "tls_echo");
    out->tlsTruncating = portOf(text, "tls_truncating");
    out->tlsExpired = portOf(text, "tls_expired");
    out->tlsUntrusted = portOf(text, "tls_untrusted");
    out->http = portOf(text, "http");
    out->https = portOf(text, "https");
    out->ws = portOf(text, "ws");
    out->wss = portOf(text, "wss");
    out->comfy = portOf(text, "comfy");
    out->closed = portOf(text, "closed");
    prFileUnmap(&ports);
    memCopy(path + size, "/ca.der", 7);
    return prFileMap(PrStringView{path, size + 7}, &out->ca);
}

void netPeerReset(NetPeer* peer) {
    prFileUnmap(&peer->ca);
}

void netLatchInit(NetLatch* latch) {
    prMutexInit(&latch->mutex);
    prConditionInit(&latch->changed);
    latch->open = false;
}

void netLatchReset(NetLatch* latch) {
    prConditionReset(&latch->changed);
    prMutexReset(&latch->mutex);
}

void netLatchOpen(NetLatch* latch) {
    prMutexLock(&latch->mutex);
    latch->open = true;
    prConditionBroadcast(&latch->changed);
    prMutexUnlock(&latch->mutex);
}

void netLatchWait(NetLatch* latch) {
    prMutexLock(&latch->mutex);
    while (!latch->open)
        prConditionWait(&latch->changed, &latch->mutex);
    latch->open = false;
    prMutexUnlock(&latch->mutex);
}

void netProbeRun(NetProbe* probe, PrNet* net, const NetProbeScript* script) {
    probe->net = net;
    probe->script = *script;
    prMutexInit(&probe->mutex);
    prConditionInit(&probe->changed);
    prNetPost(net, start, probe);
    prMutexLock(&probe->mutex);
    while (!probe->done)
        prConditionWait(&probe->changed, &probe->mutex);
    prMutexUnlock(&probe->mutex);
}

bool netProbeReceived(const NetProbe* probe, PrStringView text) {
    return PrStringView{reinterpret_cast<const char*>(probe->received.data), probe->received.count} == text;
}

uint32_t netProbeEventCount(const NetProbe* probe, PrNetStreamEvent event) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < probe->events.count; ++i)
        count += probe->events.data[i] == event ? 1 : 0;
    return count;
}

void netProbeReset(NetProbe* probe) {
    prNetPost(probe->net, destroy, probe);
    prMutexLock(&probe->mutex);
    while (!probe->destroyed)
        prConditionWait(&probe->changed, &probe->mutex);
    prMutexUnlock(&probe->mutex);
    probe->events.reset();
    probe->received.reset();
    prConditionReset(&probe->changed);
    prMutexReset(&probe->mutex);
}

} // namespace harness

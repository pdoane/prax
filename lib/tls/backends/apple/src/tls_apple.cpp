// TLS sessions by Network.framework (tls_apple.h).

#include "pr/tls_apple.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/thread.h"

#include <Network/Network.h>
#include <Security/Security.h>
#include <stdlib.h>

namespace pr_impl {

namespace {

// The most plaintext bytes that a session holds that arrived and were not read. A session asks Network.framework for no
// more while it holds this many.
constexpr uint32_t kReceivedMaxBytes = 256 * 1024;

// The most bytes that one receive asks for.
constexpr uint32_t kReceiveChunkBytes = 64 * 1024;

// The longest host or server name, with room for its terminator.
constexpr uint32_t kNameMaxBytes = 254;

// `mutex` guards `live`, the number of sessions whose memory remains. The source's destruction waits for it to reach
// zero, because Network.framework calls a connection's handlers until it is cancelled.
struct AppleTls : PrTls {
    dispatch_queue_t queue;
    PrMutex mutex;
    PrCondition idle;
    uint32_t live;
};

enum {
    SessionState_Handshaking,
    SessionState_Open,
    SessionState_Failed,
};
typedef uint8_t SessionState;

// A session. Network.framework's handlers run on the source's queue and post what they learn to the loop, where the
// rest of the session's state lives. `references` counts the client's reference, one for the state handler until the
// connection is cancelled, one for each receive and send that has not completed, and one for each posted delivery.
//
// `verifyFailure` is written on the queue during the handshake, and read on the loop after the delivery that follows
// it.
struct AppleSession : PrTlsSession {
    AppleTls* tls;
    nw_connection_t connection;
    Atomic<uint32_t> references;
    CFStringRef serverName;
    CFArrayRef PR_NULLABLE anchors;
    PrTlsFailure verifyFailure;
    SessionState state;
    PrNetError error;
    PrTlsFailure failure;
    PrTlsVersion version;
    PrRawString protocol;
    RawArray<uint8_t> received;        // plaintext that arrived, from `receivedStart`
    uint32_t receivedStart;
    RawArray<uint8_t> unsent;       // plaintext written before the handshake completed
    size_t sending;                 // bytes handed to Network.framework whose send has not completed
    bool receiving;                 // true while a receive has not completed
    bool ended;
    bool writableOwed;
    bool dead;
};

enum {
    DeliveryKind_Ready,
    DeliveryKind_Failed,
    DeliveryKind_Received,
    DeliveryKind_Sent,
};
typedef uint8_t DeliveryKind;

// What a handler learned, posted to the loop.
struct Delivery {
    AppleSession* session;
    DeliveryKind kind;
    PrNetError error;
    PrTlsFailure failure;
    PrTlsVersion version;
    PrRawString protocol;
    RawArray<uint8_t> bytes;
    size_t sent;
    bool complete;                  // true if the peer ended
};

void retain(AppleSession* session) {
    session->references.fetchAddRelaxed(1);
}

void release(AppleSession* session) {
    if (session->references.fetchSub(1) != 1)
        return;
    AppleTls* tls = session->tls;
    nw_release(session->connection);
    CFRelease(session->serverName);
    if (session->anchors != nullptr)
        CFRelease(session->anchors);
    prRawStringReset(&session->protocol);
    session->received.reset();
    session->unsent.reset();
    prMemoryFree(session);
    prMutexLock(&tls->mutex);
    if (--tls->live == 0)
        prConditionBroadcast(&tls->idle);
    prMutexUnlock(&tls->mutex);
}

void deliver(AppleSession* session, PrNetStreamEvent event) {
    if (!session->dead && session->stream.event != nullptr)
        session->stream.event(session->stream.context, &session->stream, event);
}

PrNetError errorOfPosix(int code) {
    switch (code) {
        case ECONNREFUSED:
            return PrNetError_Refused;
        case ENETUNREACH:
        case EHOSTUNREACH:
        case EADDRNOTAVAIL:
        case ENETDOWN:
            return PrNetError_Unreachable;
        case ETIMEDOUT:
            return PrNetError_TimedOut;
        case ECONNRESET:
        case ECONNABORTED:
        case EPIPE:
            return PrNetError_Reset;
        default:
            return PrNetError_Io;
    }
}

PrTlsFailure failureOfTrust(CFIndex code) {
    switch (code) {
        case errSecCertificateExpired:
        case errSecCertificateNotValidYet:
            return PrTlsFailure_Expired;
        case errSecHostNameMismatch:
            return PrTlsFailure_NameMismatch;
        default:
            return PrTlsFailure_Untrusted;
    }
}

// Fills `delivery` with the failure that `error` reports, during the handshake if `handshaking`.
void describeError(Delivery* delivery, nw_error_t error, bool handshaking) {
    int code = nw_error_get_error_code(error);
    switch (nw_error_get_error_domain(error)) {
        case nw_error_domain_posix:
            delivery->error = errorOfPosix(code);
            break;
        case nw_error_domain_dns:
            delivery->error = PrNetError_NameNotResolved;
            break;
        case nw_error_domain_tls:
            delivery->error = PrNetError_Protocol;
            if (delivery->session->verifyFailure != PrTlsFailure_None)
                delivery->failure = delivery->session->verifyFailure;
            else if (code == errSSLClosedNoNotify)
                delivery->failure = PrTlsFailure_Truncated;
            else
                delivery->failure = handshaking ? PrTlsFailure_Handshake : PrTlsFailure_Protocol;
            break;
        default:
            delivery->error = PrNetError_Io;
            break;
    }
}

void run(void* PR_NULLABLE context);

// Posts `delivery` to the loop, which takes over the reference to the session that the delivery holds.
void post(Delivery* delivery) {
    prNetPost(delivery->session->stream.net, run, delivery);
}

Delivery* deliveryOf(AppleSession* session, DeliveryKind kind) {
    Delivery* delivery = allocZeroed<Delivery>();
    delivery->session = session;
    delivery->kind = kind;
    return delivery;
}

void receive(AppleSession* session);

// Sends `size` bytes from `bytes`, from the loop.
void send(AppleSession* session, const uint8_t* bytes, size_t size) {
    dispatch_data_t data = dispatch_data_create(bytes, size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    session->sending += size;
    retain(session);
    nw_connection_send(session->connection, data, NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false, ^(nw_error_t error) {
        Delivery* delivery = deliveryOf(session, DeliveryKind_Sent);
        delivery->sent = size;
        if (error != nullptr) {
            delivery->kind = DeliveryKind_Failed;
            describeError(delivery, error, false);
        }
        post(delivery);
    });
    dispatch_release(data);
}

void failSession(AppleSession* session, PrNetError error, PrTlsFailure failure) {
    if (session->state == SessionState_Failed)
        return;
    session->state = SessionState_Failed;
    session->error = error;
    session->failure = failure;
    nw_connection_cancel(session->connection);
    deliver(session, PrNetStreamEvent_Fail);
}

// Runs a delivery on the loop.
void run(void* PR_NULLABLE context) {
    Delivery* delivery = static_cast<Delivery*>(context);
    AppleSession* session = delivery->session;
    if (!session->dead && session->state != SessionState_Failed) {
        switch (delivery->kind) {
            case DeliveryKind_Ready:
                session->state = SessionState_Open;
                session->version = delivery->version;
                session->protocol = delivery->protocol;
                delivery->protocol = prRawStringNull();
                if (session->unsent.count != 0) {
                    send(session, session->unsent.data, session->unsent.count);
                    session->unsent.reset();
                }
                receive(session);
                deliver(session, PrNetStreamEvent_Open);
                break;
            case DeliveryKind_Failed:
                failSession(session, delivery->error, delivery->failure);
                break;
            case DeliveryKind_Received: {
                session->receiving = false;
                bool waiting = session->received.count > session->receivedStart;
                if (delivery->bytes.count != 0) {
                    if (session->receivedStart != 0 && session->receivedStart == session->received.count) {
                        session->received.clear();
                        session->receivedStart = 0;
                    }
                    session->received.append(delivery->bytes.data, delivery->bytes.count);
                }
                if (delivery->complete)
                    session->ended = true;
                else
                    receive(session);
                if (delivery->bytes.count != 0 && !waiting)
                    deliver(session, PrNetStreamEvent_Readable);
                if (delivery->complete)
                    deliver(session, PrNetStreamEvent_End);
                break;
            }
            case DeliveryKind_Sent:
                session->sending -= delivery->sent;
                if (session->sending == 0 && session->unsent.count == 0 && session->writableOwed) {
                    session->writableOwed = false;
                    deliver(session, PrNetStreamEvent_Writable);
                }
                break;
        }
    }
    prRawStringReset(&delivery->protocol);
    delivery->bytes.reset();
    prMemoryFree(delivery);
    release(session);
}

// Asks for the next bytes, from the loop, unless one receive is waiting or the session holds as much as it may.
void receive(AppleSession* session) {
    if (session->receiving || session->ended || session->state != SessionState_Open ||
            session->received.count - session->receivedStart >= kReceivedMaxBytes)
        return;
    session->receiving = true;
    retain(session);
    nw_connection_receive(session->connection, 1, kReceiveChunkBytes,
        ^(dispatch_data_t content, nw_content_context_t message, bool complete, nw_error_t error) {
            Delivery* delivery = deliveryOf(session, DeliveryKind_Received);
            if (content != nullptr) {
                dispatch_data_apply(content, ^bool(dispatch_data_t, size_t, const void* buffer, size_t size) {
                    delivery->bytes.append(static_cast<const uint8_t*>(buffer), uint32_t(size));
                    return true;
                });
            }
            if (error != nullptr) {
                delivery->kind = DeliveryKind_Failed;
                describeError(delivery, error, false);
            } else {
                delivery->complete = complete && (message == nullptr || nw_content_context_get_is_final(message));
            }
            post(delivery);
        });
}

// Runs on the queue with each change of the connection's state.
void stateChanged(AppleSession* session, nw_connection_state_t state, nw_error_t PR_NULLABLE error) {
    switch (state) {
        case nw_connection_state_ready: {
            Delivery* delivery = deliveryOf(session, DeliveryKind_Ready);
            nw_protocol_definition_t definition = nw_protocol_copy_tls_definition();
            nw_protocol_metadata_t metadata = nw_connection_copy_protocol_metadata(session->connection, definition);
            nw_release(definition);
            if (metadata != nullptr) {
                sec_protocol_metadata_t secure = nw_tls_copy_sec_protocol_metadata(metadata);
                tls_protocol_version_t version = sec_protocol_metadata_get_negotiated_tls_protocol_version(secure);
                if (version == tls_protocol_version_TLSv13)
                    delivery->version = PrTlsVersion_Tls13;
                else if (version == tls_protocol_version_TLSv12)
                    delivery->version = PrTlsVersion_Tls12;
                else
                    delivery->version = PrTlsVersion_None;
                char* protocol = const_cast<char*>(sec_protocol_metadata_copy_negotiated_protocol(secure));
                if (protocol != nullptr) {
                    size_t size = 0;
                    while (protocol[size] != 0)
                        ++size;
                    delivery->protocol = prRawStringCopy(PrStringView{protocol, size});
                    free(protocol);
                }
                sec_release(secure);
                nw_release(metadata);
            }
            retain(session);
            post(delivery);
            break;
        }
        case nw_connection_state_waiting:
        case nw_connection_state_failed: {
            // A connection that waits would try again when the network changes. A session fails instead, as a TCP
            // connection does.
            Delivery* delivery = deliveryOf(session, DeliveryKind_Failed);
            if (error != nullptr)
                describeError(delivery, error, true);
            else
                delivery->error = PrNetError_Io;
            retain(session);
            post(delivery);
            break;
        }
        case nw_connection_state_cancelled:
            release(session);
            break;
        default:
            break;
    }
}

// Evaluates the server's chain on the queue, for the session's server name, against the system's roots and the
// session's anchors. Records why it fails.
bool verify(AppleSession* session, sec_trust_t secure) {
    SecTrustRef trust = sec_trust_copy_ref(secure);
    SecPolicyRef policy = SecPolicyCreateSSL(true, session->serverName);
    SecTrustSetPolicies(trust, policy);
    CFRelease(policy);
    if (session->anchors != nullptr) {
        SecTrustSetAnchorCertificates(trust, session->anchors);
        SecTrustSetAnchorCertificatesOnly(trust, false);
    }
    CFErrorRef error = nullptr;
    bool trusted = SecTrustEvaluateWithError(trust, &error);
    if (!trusted)
        session->verifyFailure = failureOfTrust(error != nullptr ? CFErrorGetCode(error) : 0);
    if (error != nullptr)
        CFRelease(error);
    CFRelease(trust);
    return trusted;
}

// AppleSession functions

void destroySession(PrNetStream* base) {
    AppleSession* session = static_cast<AppleSession*>(reinterpret_cast<PrTlsSession*>(base));
    PR_ASSERT(!session->dead);
    session->dead = true;
    nw_connection_cancel(session->connection);
    release(session);
}

size_t readSession(PrNetStream* base, uint8_t* output, size_t capacity) {
    AppleSession* session = static_cast<AppleSession*>(reinterpret_cast<PrTlsSession*>(base));
    uint32_t available = session->received.count - session->receivedStart;
    uint32_t taken = capacity < available ? uint32_t(capacity) : available;
    prMemCopy(output, session->received.data + session->receivedStart, taken);
    session->receivedStart += taken;
    if (session->receivedStart == session->received.count) {
        session->received.clear();
        session->receivedStart = 0;
    }
    // The receive completes on the queue, so its callback runs after this call.
    receive(session);
    return taken;
}

void writeSession(PrNetStream* base, PrByteView bytes) {
    AppleSession* session = static_cast<AppleSession*>(reinterpret_cast<PrTlsSession*>(base));
    if (session->state == SessionState_Failed)
        return;
    session->writableOwed = true;
    if (session->state == SessionState_Handshaking) {
        PR_ASSERT(bytes.size <= UINT32_MAX - session->unsent.count);
        session->unsent.append(bytes.data, uint32_t(bytes.size));
        return;
    }
    send(session, bytes.data, bytes.size);
}

size_t queuedSession(const PrNetStream* base) {
    const AppleSession* session = static_cast<const AppleSession*>(reinterpret_cast<const PrTlsSession*>(base));
    return session->unsent.count + session->sending;
}

PrNetError errorSession(const PrNetStream* base) {
    return static_cast<const AppleSession*>(reinterpret_cast<const PrTlsSession*>(base))->error;
}

PrTlsSessionInfo infoSession(const PrTlsSession* base) {
    const AppleSession* session = static_cast<const AppleSession*>(base);
    return PrTlsSessionInfo{
        .version = session->version,
        .failure = session->failure,
        .protocol = prRawStringView(&session->protocol),
    };
}

// Source functions

void destroyTls(PrTls* base) {
    AppleTls* tls = static_cast<AppleTls*>(base);
    prMutexLock(&tls->mutex);
    while (tls->live != 0)
        prConditionWait(&tls->idle, &tls->mutex);
    prMutexUnlock(&tls->mutex);
    dispatch_release(tls->queue);
    prConditionReset(&tls->idle);
    prMutexReset(&tls->mutex);
    prMemoryFree(tls);
}

bool copyName(PrStringView name, char* out) {
    if (name.size == 0 || name.size >= kNameMaxBytes)
        return false;
    for (size_t i = 0; i < name.size; ++i) {
        char c = name.data[i];
        if (c == 0 || c == '/' || c == ' ' || c == '[' || c == ']')
            return false;
    }
    prMemCopy(out, name.data, name.size);
    out[name.size] = 0;
    return true;
}

// Returns true if `name` is an IPv4 or IPv6 address rather than a domain name. A domain name always has a letter, and
// only an IPv6 address has a colon.
bool isAddress(const char* name) {
    bool letter = false;
    for (const char* c = name; *c != 0; ++c) {
        if (*c == ':')
            return true;
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z'))
            letter = true;
    }
    return !letter;
}

PrStatus connectTls(PrTls* base, const PrTlsSessionDesc* desc, PrTlsSession* PR_NULLABLE* PR_NONNULL out) {
    AppleTls* tls = static_cast<AppleTls*>(base);
    if (desc->transport != nullptr)
        return PrStatus_NotSupported;
    char host[kNameMaxBytes];
    char serverName[kNameMaxBytes];
    if (!copyName(desc->host, host) ||
            !copyName(desc->serverName.data != nullptr ? desc->serverName : desc->host, serverName))
        return PrStatus_InvalidData;
    CFMutableArrayRef anchors = nullptr;
    if (desc->anchorCount != 0) {
        anchors = CFArrayCreateMutable(nullptr, desc->anchorCount, &kCFTypeArrayCallBacks);
        for (uint32_t i = 0; i < desc->anchorCount; ++i) {
            CFDataRef der = CFDataCreate(nullptr, desc->anchors[i].data, CFIndex(desc->anchors[i].size));
            SecCertificateRef certificate = SecCertificateCreateWithData(nullptr, der);
            CFRelease(der);
            if (certificate == nullptr) {
                CFRelease(anchors);
                return PrStatus_InvalidData;
            }
            CFArrayAppendValue(anchors, certificate);
            CFRelease(certificate);
        }
    }
    char port[8];
    uint32_t digits = 0;
    char reversed[8];
    uint32_t value = desc->port;
    do {
        reversed[digits++] = char('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (uint32_t i = 0; i < digits; ++i)
        port[i] = reversed[digits - 1 - i];
    port[digits] = 0;

    AppleSession* session = allocZeroed<AppleSession>();
    session->stream.net = tls->net;
    session->stream.event = desc->event;
    session->stream.context = desc->context;
    session->stream.destroy = destroySession;
    session->stream.read = readSession;
    session->stream.write = writeSession;
    session->stream.queued = queuedSession;
    session->stream.error = errorSession;
    session->info = infoSession;
    session->tls = tls;
    session->serverName = CFStringCreateWithCString(nullptr, serverName, kCFStringEncodingUTF8);
    session->anchors = anchors;
    // The client's reference, and the state handler's until the connection is cancelled.
    session->references.store(2);
    prMutexLock(&tls->mutex);
    ++tls->live;
    prMutexUnlock(&tls->mutex);

    // The block that configures TLS runs during nw_parameters_create_secure_tcp, so it reads the descriptor and the
    // names in place.
    const char* name = isAddress(serverName) ? nullptr : serverName;
    const PrTlsSessionDesc* offered = desc;
    bool configured = false;
    bool* configuredAt = &configured;
    nw_parameters_t parameters = nw_parameters_create_secure_tcp(^(nw_protocol_options_t options) {
        *configuredAt = true;
        sec_protocol_options_t secure = nw_tls_copy_sec_protocol_options(options);
        sec_protocol_options_set_min_tls_protocol_version(secure, tls_protocol_version_TLSv12);
        if (name != nullptr)
            sec_protocol_options_set_tls_server_name(secure, name);
        for (uint32_t i = 0; i < offered->protocolCount; ++i) {
            char protocol[256];
            PrStringView offer = offered->protocols[i];
            PR_ASSERT(offer.size != 0 && offer.size < sizeof(protocol));
            prMemCopy(protocol, offer.data, offer.size);
            protocol[offer.size] = 0;
            sec_protocol_options_add_tls_application_protocol(secure, protocol);
        }
        sec_protocol_options_set_verify_block(secure, ^(sec_protocol_metadata_t, sec_trust_t trust,
            sec_protocol_verify_complete_t complete) {
                complete(verify(session, trust));
            }, tls->queue);
        sec_release(secure);
    }, NW_PARAMETERS_DEFAULT_CONFIGURATION);
    PR_ASSERT(configured);
    nw_endpoint_t endpoint = nw_endpoint_create_host(host, port);
    session->connection = nw_connection_create(endpoint, parameters);
    nw_release(endpoint);
    nw_release(parameters);
    nw_connection_set_queue(session->connection, tls->queue);
    nw_connection_set_state_changed_handler(session->connection, ^(nw_connection_state_t state, nw_error_t error) {
        stateChanged(session, state, error);
    });
    nw_connection_start(session->connection);
    *out = session;
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTls* prTlsAppleCreate(PrNet* net) {
    AppleTls* tls = allocZeroed<AppleTls>();
    tls->net = net;
    tls->destroy = destroyTls;
    tls->connect = connectTls;
    tls->queue = dispatch_queue_create("prax.tls", DISPATCH_QUEUE_SERIAL);
    prMutexInit(&tls->mutex);
    prConditionInit(&tls->idle);
    return tls;
}

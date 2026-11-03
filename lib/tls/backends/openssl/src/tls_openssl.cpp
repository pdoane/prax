// TLS sessions by OpenSSL, over streams (tls_openssl.h).

#include "pr/tls_openssl.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/raw_array.h"

#include "tls/backends/shared/src/tls_shared.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace pr_impl {

namespace {

// The bytes that one SSL_read, or one move between the transport and OpenSSL, takes at most.
constexpr uint32_t kChunkBytes = 16 * 1024;

// The longest server name, with room for its terminator.
constexpr uint32_t kNameMaxBytes = 254;

struct OpensslTls : PrTls {
    SSL_CTX* context;
};

// A session of OpenSSL's. OpenSSL reads the transport's bytes from `incoming` and writes the bytes for the transport to
// `outgoing`, both memory BIOs that the session moves to and from the transport. Its protocol points into OpenSSL's
// session, which lives as long as `ssl`.
struct Session : TlsSharedSession {
    SSL* ssl;
    BIO* incoming;
    BIO* outgoing;
};

void freeSession(Session* session) {
    SSL_free(session->ssl);
    session->received.reset();
    session->unsent.reset();
    prMemoryFree(session);
}

PrTlsFailure failureOfVerify(long result) {
    switch (result) {
        case X509_V_OK:
            return PrTlsFailure_Handshake;
        case X509_V_ERR_CERT_HAS_EXPIRED:
        case X509_V_ERR_CERT_NOT_YET_VALID:
            return PrTlsFailure_Expired;
        case X509_V_ERR_HOSTNAME_MISMATCH:
        case X509_V_ERR_IP_ADDRESS_MISMATCH:
            return PrTlsFailure_NameMismatch;
        default:
            return PrTlsFailure_Untrusted;
    }
}

// Moves the bytes that OpenSSL wrote for the transport to it.
void flushOutgoing(Session* session) {
    uint8_t chunk[kChunkBytes];
    while (session->transport != nullptr) {
        int got = BIO_read(session->outgoing, chunk, int(sizeof(chunk)));
        if (got <= 0)
            break;
        prNetStreamWrite(session->transport, PrByteView{chunk, size_t(got)});
    }
}

// Encrypts `bytes`, from a call of the client's.
void writePlain(Session* session, const uint8_t* bytes, size_t size) {
    while (size != 0) {
        int chunk = size < kChunkBytes ? int(size) : int(kChunkBytes);
        ERR_clear_error();
        int written = SSL_write(session->ssl, bytes, chunk);
        if (written <= 0) {
            tlsSharedFailLater(session, PrNetError_Protocol, PrTlsFailure_Protocol);
            return;
        }
        bytes += written;
        size -= size_t(written);
    }
}

void sendCloseNotify(Session* session) {
    session->finished = true;
    SSL_shutdown(session->ssl);
    flushOutgoing(session);
    // A transport that cannot end its sending side ends when the session is destroyed.
    if (session->transport != nullptr)
        prNetStreamFinish(session->transport);
}

void completeHandshake(Session* session) {
    session->state = TlsSharedState_Open;
    session->version = SSL_version(session->ssl) == TLS1_3_VERSION ? PrTlsVersion_Tls13 : PrTlsVersion_Tls12;
    const unsigned char* selected = nullptr;
    unsigned int selectedSize = 0;
    SSL_get0_alpn_selected(session->ssl, &selected, &selectedSize);
    if (selected != nullptr && selectedSize != 0)
        session->protocol = PrStringView{reinterpret_cast<const char*>(selected), selectedSize};
    if (session->unsent.count != 0) {
        writePlain(session, session->unsent.data, session->unsent.count);
        session->unsent.reset();
    }
    if (session->finishing && session->state == TlsSharedState_Open)
        sendCloseNotify(session);
    flushOutgoing(session);
    tlsSharedDeliver(session, PrNetStreamEvent_Open);
}

// Runs OpenSSL on what arrived: the handshake, then the records, and delivers the events that they cause.
void advance(Session* session) {
    if (session->state == TlsSharedState_Handshaking) {
        ERR_clear_error();
        int result = SSL_do_handshake(session->ssl);
        if (result == 1) {
            completeHandshake(session);
        } else {
            int code = SSL_get_error(session->ssl, result);
            flushOutgoing(session);
            if (code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE)
                tlsSharedFail(session, PrNetError_Protocol, failureOfVerify(SSL_get_verify_result(session->ssl)));
            return;
        }
    }
    if (session->dead || session->state != TlsSharedState_Open)
        return;
    bool waiting = session->received.count > session->receivedStart;
    bool arrived = false;
    for (;;) {
        if (session->ended)
            break;
        if (session->received.count - session->receivedStart >= kTlsSharedReceivedMaxBytes) {
            session->stalled = true;
            break;
        }
        if (session->receivedStart != 0 && session->receivedStart == session->received.count) {
            session->received.clear();
            session->receivedStart = 0;
        }
        uint32_t at = session->received.count;
        session->received.resize(at + kChunkBytes);
        ERR_clear_error();
        int got = SSL_read(session->ssl, session->received.data + at, int(kChunkBytes));
        session->received.resize(got > 0 ? at + uint32_t(got) : at);
        if (got > 0) {
            arrived = true;
            continue;
        }
        int code = SSL_get_error(session->ssl, got);
        if (code == SSL_ERROR_ZERO_RETURN) {
            session->ended = true;
        } else if (code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE) {
            flushOutgoing(session);
            tlsSharedFail(session, PrNetError_Protocol, PrTlsFailure_Protocol);
            return;
        }
        break;
    }
    // Reading may answer the peer, as a key update does.
    flushOutgoing(session);
    if (arrived && !waiting)
        tlsSharedDeliver(session, PrNetStreamEvent_Readable);
    if (session->ended && !session->dead)
        tlsSharedDeliver(session, PrNetStreamEvent_End);
}

// Moves what the transport received into OpenSSL until the session holds as much plaintext as it may. Then, if the
// transport ended without the peer's close_notify, fails the session.
void drainTransport(Session* session) {
    uint8_t chunk[kChunkBytes];
    while (session->transport != nullptr && !session->stalled) {
        size_t got = prNetStreamRead(session->transport, chunk, sizeof(chunk));
        if (got == 0)
            break;
        BIO_write(session->incoming, chunk, int(got));
        advance(session);
        if (session->dead || session->state == TlsSharedState_Failed)
            return;
    }
    if (session->transportEnded && !session->stalled && !session->ended) {
        bool handshaking = session->state == TlsSharedState_Handshaking;
        tlsSharedFail(session, PrNetError_Protocol, handshaking ? PrTlsFailure_Handshake : PrTlsFailure_Truncated);
    }
}

// Session functions

void destroySession(PrNetStream* base) {
    Session* session = static_cast<Session*>(reinterpret_cast<PrTlsSession*>(base));
    PR_ASSERT(!session->dead);
    session->dead = true;
    prNetStreamDestroy(session->transport);
    session->transport = nullptr;
    if (session->holds == 0)
        freeSession(session);
}

void writeSession(PrNetStream* base, PrByteView bytes) {
    Session* session = static_cast<Session*>(reinterpret_cast<PrTlsSession*>(base));
    if (session->state == TlsSharedState_Failed || session->finishing)
        return;
    session->writableOwed = true;
    if (session->state == TlsSharedState_Handshaking) {
        PR_ASSERT(bytes.size <= UINT32_MAX - session->unsent.count);
        session->unsent.append(bytes.data, uint32_t(bytes.size));
        return;
    }
    writePlain(session, bytes.data, bytes.size);
    flushOutgoing(session);
}

// Source functions

void destroyTls(PrTls* base) {
    OpensslTls* tls = static_cast<OpensslTls*>(base);
    SSL_CTX_free(tls->context);
    prMemoryFree(tls);
}

bool copyName(PrStringView name, char* out) {
    if (name.size == 0 || name.size >= kNameMaxBytes)
        return false;
    for (size_t i = 0; i < name.size; ++i) {
        if (name.data[i] == 0)
            return false;
    }
    prMemCopy(out, name.data, name.size);
    out[name.size] = 0;
    return true;
}

// Configures what `ssl` offers and verifies. Returns InvalidData for a name or an anchor that OpenSSL rejects.
PrStatus configure(SSL* ssl, const PrTlsSessionDesc* desc) {
    char name[kNameMaxBytes];
    if (!copyName(desc->serverName.data != nullptr ? desc->serverName : desc->host, name))
        return PrStatus_InvalidData;
    ASN1_OCTET_STRING* address = a2i_IPADDRESS(name);
    if (address != nullptr) {
        ASN1_OCTET_STRING_free(address);
        if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), name) != 1)
            return PrStatus_InvalidData;
    } else if (SSL_set1_host(ssl, name) != 1 || SSL_set_tlsext_host_name(ssl, name) != 1) {
        return PrStatus_InvalidData;
    }
    if (desc->protocolCount != 0) {
        RawArray<uint8_t> wire{};
        for (uint32_t i = 0; i < desc->protocolCount; ++i) {
            PrStringView protocol = desc->protocols[i];
            PR_ASSERT(protocol.size != 0 && protocol.size <= 255);
            wire.push(uint8_t(protocol.size));
            wire.append(reinterpret_cast<const uint8_t*>(protocol.data), uint32_t(protocol.size));
        }
        int status = SSL_set_alpn_protos(ssl, wire.data, wire.count);
        wire.reset();
        // SSL_set_alpn_protos returns zero on success.
        if (status != 0)
            return PrStatus_InvalidData;
    }
    if (desc->anchorCount != 0) {
        X509_STORE* store = X509_STORE_new();
        X509_STORE_set_default_paths(store);
        for (uint32_t i = 0; i < desc->anchorCount; ++i) {
            const unsigned char* der = desc->anchors[i].data;
            X509* certificate = d2i_X509(nullptr, &der, long(desc->anchors[i].size));
            if (certificate == nullptr) {
                X509_STORE_free(store);
                return PrStatus_InvalidData;
            }
            X509_STORE_add_cert(store, certificate);
            X509_free(certificate);
        }
        SSL_set1_verify_cert_store(ssl, store);
        X509_STORE_free(store);
    }
    return PrStatus_Ok;
}

// OpenSSL's steps, which the shared calls take.
constexpr TlsSharedEngine kEngine = {
    .advance = [](TlsSharedSession* session) { advance(static_cast<Session*>(session)); },
    .drainTransport = [](TlsSharedSession* session) { drainTransport(static_cast<Session*>(session)); },
    .sendCloseNotify = [](TlsSharedSession* session) { sendCloseNotify(static_cast<Session*>(session)); },
    .free = [](TlsSharedSession* session) { freeSession(static_cast<Session*>(session)); },
};

PrStatus connectTls(PrTls* base, const PrTlsSessionDesc* desc, PrTlsSession* PR_NULLABLE* PR_NONNULL out) {
    OpensslTls* tls = static_cast<OpensslTls*>(base);
    SSL* ssl = SSL_new(tls->context);
    PrStatus status = configure(ssl, desc);
    if (status != PrStatus_Ok) {
        SSL_free(ssl);
        return status;
    }
    Session* session = allocZeroed<Session>();
    session->engine = &kEngine;
    session->stream.net = tls->net;
    session->stream.event = desc->event;
    session->stream.context = desc->context;
    session->stream.destroy = destroySession;
    session->stream.read = tlsSharedRead;
    session->stream.write = writeSession;
    session->stream.finish = tlsSharedFinish;
    session->stream.queued = tlsSharedQueued;
    session->stream.error = tlsSharedError;
    session->info = tlsSharedInfo;
    session->ssl = ssl;
    session->incoming = BIO_new(BIO_s_mem());
    session->outgoing = BIO_new(BIO_s_mem());
    // An empty incoming BIO means that more bytes may arrive, not that the transport ended.
    BIO_set_mem_eof_return(session->incoming, -1);
    SSL_set_bio(ssl, session->incoming, session->outgoing);
    SSL_set_connect_state(ssl);
    if (desc->transport != nullptr) {
        session->transport = desc->transport;
        prNetStreamSetEvent(desc->transport, tlsSharedTransportEvent, session);
    } else {
        PrNetConnectDesc connect{
            .host = desc->host,
            .port = desc->port,
            .event = tlsSharedTransportEvent,
            .context = session,
        };
        status = prNetConnect(tls->net, &connect, &session->transport);
        if (status != PrStatus_Ok) {
            freeSession(session);
            return status;
        }
    }
    // The client's first flight is written now, and the transport queues it until it opens. A failure here recurs when
    // the server's flight arrives, which reports it.
    ERR_clear_error();
    SSL_do_handshake(ssl);
    flushOutgoing(session);
    *out = session;
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTls* prTlsOpensslCreate(PrNet* net) {
    OpensslTls* tls = allocZeroed<OpensslTls>();
    tls->net = net;
    tls->destroy = destroyTls;
    tls->connect = connectTls;
    tls->context = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(tls->context, TLS1_2_VERSION);
    SSL_CTX_set_default_verify_paths(tls->context);
    SSL_CTX_set_verify(tls->context, SSL_VERIFY_PEER, nullptr);
    return tls;
}

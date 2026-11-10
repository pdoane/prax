// TLS sessions by SChannel, over streams (tls_schannel.h).

#include "pr/tls_schannel.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/core_windows_string.h"
#include "pr/count_of.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/thread.h"

#include "tls/backends/shared/src/tls_shared.h"

// Each header needs the declarations of the runs before it. security.h declares SSPI for user mode with SECURITY_WIN32,
// and schannel.h declares SCH_CREDENTIALS with SCHANNEL_USE_BLACKLISTS.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define SECURITY_WIN32
#include <security.h>
#include <subauth.h>
#include <wincrypt.h>
#define SCHANNEL_USE_BLACKLISTS
#include <schannel.h>

namespace pr_impl {

namespace {

// The bytes that one read from the transport takes at most.
constexpr uint32_t kChunkBytes = 16 * 1024;

// The longest server name, with room for its terminator.
constexpr uint32_t kNameMaxBytes = 254;

// The longest application protocol (rfc 7301).
constexpr uint32_t kProtocolMaxBytes = 255;

// What every call to InitializeSecurityContextW asks for. The session verifies the server's certificate itself.
constexpr ULONG kContextRequest = (ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
    ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR | ISC_REQ_MANUAL_CRED_VALIDATION);

struct Session;

// A certificate to verify on the verifier's thread, which writes `failure`. `session` is read and written only on the
// loop: it is cleared if the session is destroyed before the result arrives.
struct VerifyJob {
    PrNet* net;
    Session* PR_NULLABLE session;
    PCCERT_CONTEXT certificate;     // the certificate that the server presented, with the intermediates it sent
    HCERTSTORE PR_NULLABLE anchors;
    wchar_t name[kNameMaxBytes];
    PrTlsFailure failure;
};

// `mutex` guards the fields after it, which the verifier's thread reaches.
struct SchannelTls : PrTls {
    PrNet* net;                 // the network that the sessions are streams of
    CredHandle credentials;
    PrThread verifier;
    PrMutex mutex;
    PrCondition jobsChanged;
    RawArray<VerifyJob*> jobs;
    bool stopping;
};

// A session of SChannel's. `incoming` from `incomingStart` holds the transport's bytes that SChannel has not consumed.
// Its protocol points into `protocolBytes`.
struct Session : TlsSharedSession {
    SchannelTls* tls;
    CtxtHandle context;
    bool hasContext;
    char protocolBytes[kProtocolMaxBytes];
    wchar_t name[kNameMaxBytes];    // the name that the certificate must name
    HCERTSTORE PR_NULLABLE anchors;
    PCCERT_CONTEXT PR_NULLABLE certificate; // the certificate that the server presented, once it is verified
    VerifyJob* PR_NULLABLE job;
    SecPkgContext_StreamSizes sizes;
    RawArray<uint8_t> incoming;
    uint32_t incomingStart;
    RawArray<uint8_t> sealed;       // the records of one write, as EncryptMessage seals them
    bool negotiating;               // true while an open session hands a message after the handshake to SChannel
};

void freeSession(Session* session) {
    if (session->hasContext)
        DeleteSecurityContext(&session->context);
    if (session->certificate != nullptr)
        CertFreeCertificateContext(session->certificate);
    if (session->anchors != nullptr)
        CertCloseStore(session->anchors, 0);
    session->incoming.reset();
    session->received.reset();
    session->unsent.reset();
    session->sealed.reset();
    prMemoryFree(session);
}

// Writes a token that SChannel made to the transport, and frees it.
void sendToken(Session* session, SecBuffer* token) {
    if (token->pvBuffer == nullptr)
        return;
    if (session->transport != nullptr && token->cbBuffer != 0)
        prNetStreamWrite(session->transport, PrByteView{static_cast<const uint8_t*>(token->pvBuffer), token->cbBuffer});
    FreeContextBuffer(token->pvBuffer);
    token->pvBuffer = nullptr;
}

// Marks the incoming bytes consumed, except the last `extra`, which SChannel did not read.
void consume(Session* session, uint32_t extra) {
    PR_ASSERT(extra <= session->incoming.count - session->incomingStart);
    session->incomingStart = session->incoming.count - extra;
    if (session->incomingStart == session->incoming.count) {
        session->incoming.clear();
        session->incomingStart = 0;
    }
}

// The bytes that the buffer of type `type` holds among `buffers`, or zero.
uint32_t bufferBytes(const SecBuffer* buffers, uint32_t count, unsigned long type) {
    for (uint32_t i = 0; i < count; ++i) {
        if (buffers[i].BufferType == type)
            return buffers[i].cbBuffer;
    }
    return 0;
}

// Calls InitializeSecurityContextW on the session's context, with `input`, and writes the token that it makes.
SECURITY_STATUS stepContext(Session* session, SecBufferDesc* PR_NULLABLE input) {
    SecBuffer output{0, SECBUFFER_TOKEN, nullptr};
    SecBufferDesc outputDesc{SECBUFFER_VERSION, 1, &output};
    ULONG attributes = 0;
    SECURITY_STATUS status = InitializeSecurityContextW(&session->tls->credentials, &session->context,
        session->name, kContextRequest, 0, 0, input, 0, nullptr, &outputDesc, &attributes, nullptr);
    sendToken(session, &output);
    return status;
}

// Encrypts `bytes`, from a call of the client's.
void writePlain(Session* session, const uint8_t* bytes, size_t size) {
    const SecPkgContext_StreamSizes& sizes = session->sizes;
    while (size != 0) {
        uint32_t chunk = size < sizes.cbMaximumMessage ? uint32_t(size) : uint32_t(sizes.cbMaximumMessage);
        session->sealed.resize(sizes.cbHeader + chunk + sizes.cbTrailer);
        uint8_t* record = session->sealed.data;
        prMemCopy(record + sizes.cbHeader, bytes, chunk);
        SecBuffer buffers[4] = {
            {sizes.cbHeader, SECBUFFER_STREAM_HEADER, record},
            {chunk, SECBUFFER_DATA, record + sizes.cbHeader},
            {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, record + sizes.cbHeader + chunk},
            {0, SECBUFFER_EMPTY, nullptr},
        };
        SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
        if (EncryptMessage(&session->context, 0, &desc, 0) != SEC_E_OK) {
            tlsSharedFailLater(session, PrNetError_Protocol, PrTlsFailure_Protocol);
            return;
        }
        // The trailer may be shorter than its room, and follows the data, so the record is contiguous.
        uint32_t sealed = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
        if (session->transport != nullptr)
            prNetStreamWrite(session->transport, PrByteView{record, sealed});
        bytes += chunk;
        size -= chunk;
    }
}

void sendCloseNotify(Session* session) {
    session->finished = true;
    DWORD shutdown = SCHANNEL_SHUTDOWN;
    SecBuffer control{sizeof(shutdown), SECBUFFER_TOKEN, &shutdown};
    SecBufferDesc controlDesc{SECBUFFER_VERSION, 1, &control};
    if (ApplyControlToken(&session->context, &controlDesc) == SEC_E_OK)
        stepContext(session, nullptr);
    // A transport that cannot end its sending side ends when the session is destroyed.
    if (session->transport != nullptr)
        prNetStreamFinish(session->transport);
}

// Returns true if `certificate` is the one that the session verified.
bool isVerifiedCertificate(const Session* session, PCCERT_CONTEXT certificate) {
    return session->certificate != nullptr &&
        CertCompareCertificate(X509_ASN_ENCODING, session->certificate->pCertInfo, certificate->pCertInfo);
}

// Reads what the handshake negotiated, and hands the server's certificate to the verifier.
void completeHandshake(Session* session) {
    SecPkgContext_ConnectionInfo connection{};
    QueryContextAttributesW(&session->context, SECPKG_ATTR_CONNECTION_INFO, &connection);
    session->version = (connection.dwProtocol & SP_PROT_TLS1_3_CLIENT) != 0 ? PrTlsVersion_Tls13 : PrTlsVersion_Tls12;
    SecPkgContext_ApplicationProtocol protocol{};
    if (QueryContextAttributesW(&session->context, SECPKG_ATTR_APPLICATION_PROTOCOL, &protocol) == SEC_E_OK &&
            protocol.ProtoNegoStatus == SecApplicationProtocolNegotiationStatus_Success &&
            protocol.ProtoNegoExt == SecApplicationProtocolNegotiationExt_ALPN && protocol.ProtocolIdSize != 0) {
        prMemCopy(session->protocolBytes, protocol.ProtocolId, protocol.ProtocolIdSize);
        session->protocol = PrStringView{session->protocolBytes, protocol.ProtocolIdSize};
    }
    PCCERT_CONTEXT certificate = nullptr;
    if (QueryContextAttributesW(&session->context, SECPKG_ATTR_STREAM_SIZES, &session->sizes) != SEC_E_OK ||
            QueryContextAttributesW(&session->context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &certificate) != SEC_E_OK ||
            certificate == nullptr) {
        tlsSharedFail(session, PrNetError_Protocol, PrTlsFailure_Handshake);
        return;
    }
    session->state = TlsSharedState_Verifying;
    VerifyJob* job = allocZeroed<VerifyJob>();
    job->net = session->net;
    job->session = session;
    job->certificate = certificate;
    job->anchors = session->anchors != nullptr ? CertDuplicateStore(session->anchors) : nullptr;
    prMemCopy(job->name, session->name, sizeof(job->name));
    session->job = job;
    SchannelTls* tls = session->tls;
    prMutexLock(&tls->mutex);
    tls->jobs.push(job);
    prConditionSignal(&tls->jobsChanged);
    prMutexUnlock(&tls->mutex);
}

// Checks the certificate that a renegotiation presented, which must be the one that the session verified.
void completeRenegotiation(Session* session) {
    PCCERT_CONTEXT certificate = nullptr;
    if (QueryContextAttributesW(&session->context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &certificate) == SEC_E_OK &&
            certificate != nullptr) {
        bool same = isVerifiedCertificate(session, certificate);
        CertFreeCertificateContext(certificate);
        if (!same) {
            tlsSharedFail(session, PrNetError_Protocol, PrTlsFailure_Protocol);
            return;
        }
    }
    QueryContextAttributesW(&session->context, SECPKG_ATTR_STREAM_SIZES, &session->sizes);
}

// Hands what arrived to InitializeSecurityContextW while the session negotiates: the handshake, or a message after it
// that DecryptMessage handed back, such as a TLS 1.3 session ticket.
void negotiate(Session* session) {
    bool open = session->state == TlsSharedState_Open;
    bool retried = false;
    while (session->incomingStart < session->incoming.count) {
        SecBuffer input[2] = {
            {
                session->incoming.count - session->incomingStart, SECBUFFER_TOKEN,
                session->incoming.data + session->incomingStart,
            },
            {0, SECBUFFER_EMPTY, nullptr},
        };
        SecBufferDesc inputDesc{SECBUFFER_VERSION, 2, input};
        SECURITY_STATUS status = stepContext(session, &inputDesc);
        if (session->dead)
            return;
        if (status == SEC_E_INCOMPLETE_MESSAGE)
            return;
        // A server that asks for a certificate is answered without one, from the same bytes.
        if (status == SEC_I_INCOMPLETE_CREDENTIALS && !retried) {
            retried = true;
            continue;
        }
        if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) {
            tlsSharedFail(session, PrNetError_Protocol, open ? PrTlsFailure_Protocol : PrTlsFailure_Handshake);
            return;
        }
        consume(session, input[1].BufferType == SECBUFFER_EXTRA ? input[1].cbBuffer : 0);
        if (status == SEC_E_OK) {
            session->negotiating = false;
            if (open)
                completeRenegotiation(session);
            else
                completeHandshake(session);
            return;
        }
    }
}

// Decrypts the records that arrived, and delivers the events that they cause.
void decrypt(Session* session) {
    bool waiting = session->received.count > session->receivedStart;
    bool arrived = false;
    while (!session->ended && session->incomingStart < session->incoming.count) {
        if (session->received.count - session->receivedStart >= kTlsSharedReceivedMaxBytes) {
            session->stalled = true;
            break;
        }
        if (session->receivedStart != 0 && session->receivedStart == session->received.count) {
            session->received.clear();
            session->receivedStart = 0;
        }
        SecBuffer buffers[4] = {
            {
                session->incoming.count - session->incomingStart, SECBUFFER_DATA,
                session->incoming.data + session->incomingStart,
            },
            {0, SECBUFFER_EMPTY, nullptr},
            {0, SECBUFFER_EMPTY, nullptr},
            {0, SECBUFFER_EMPTY, nullptr},
        };
        SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
        SECURITY_STATUS status = DecryptMessage(&session->context, &desc, 0, nullptr);
        if (status == SEC_E_INCOMPLETE_MESSAGE)
            break;
        if (status == SEC_I_CONTEXT_EXPIRED) {
            session->ended = true;
            consume(session, 0);
            break;
        }
        if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) {
            tlsSharedFail(session, PrNetError_Protocol, PrTlsFailure_Protocol);
            return;
        }
        for (uint32_t i = 0; i < countOf(buffers); ++i) {
            if (buffers[i].BufferType == SECBUFFER_DATA && buffers[i].cbBuffer != 0) {
                session->received.append(static_cast<const uint8_t*>(buffers[i].pvBuffer), buffers[i].cbBuffer);
                arrived = true;
            }
        }
        consume(session, bufferBytes(buffers, uint32_t(countOf(buffers)), SECBUFFER_EXTRA));
        if (status == SEC_I_RENEGOTIATE) {
            session->negotiating = true;
            negotiate(session);
            if (session->dead || session->state != TlsSharedState_Open || session->negotiating)
                break;
        }
    }
    if (session->dead || session->state != TlsSharedState_Open)
        return;
    if (arrived && !waiting)
        tlsSharedDeliver(session, PrNetStreamEvent_Readable);
    if (session->ended && !session->dead)
        tlsSharedDeliver(session, PrNetStreamEvent_End);
}

// Runs SChannel on what arrived, as the session's state calls for.
void advance(Session* session) {
    if (session->state == TlsSharedState_Handshaking || session->negotiating)
        negotiate(session);
    if (!session->dead && session->state == TlsSharedState_Open && !session->negotiating)
        decrypt(session);
}

// Returns true if the session takes more of the transport's bytes: not while its certificate is verified, nor while it
// holds as much plaintext as it may.
bool takesBytes(const Session* session) {
    return session->state == TlsSharedState_Handshaking || (session->state == TlsSharedState_Open && !session->stalled);
}

// Moves what the transport received into the session while it takes bytes. Then, if the transport ended without the
// peer's close_notify, fails the session.
void drainTransport(Session* session) {
    uint8_t chunk[kChunkBytes];
    while (session->transport != nullptr && takesBytes(session)) {
        size_t got = prNetStreamRead(session->transport, chunk, sizeof(chunk));
        if (got == 0)
            break;
        session->incoming.erase(0, session->incomingStart);
        session->incomingStart = 0;
        session->incoming.append(chunk, uint32_t(got));
        advance(session);
        if (session->dead || session->state == TlsSharedState_Failed)
            return;
    }
    if (session->transportEnded && takesBytes(session) && !session->ended) {
        bool handshaking = session->state == TlsSharedState_Handshaking;
        tlsSharedFail(session, PrNetError_Protocol, handshaking ? PrTlsFailure_Handshake : PrTlsFailure_Truncated);
    }
}

// Opens the session once its certificate is verified: sends what the client wrote before, delivers Open, and then reads
// the records that arrived.
void open(Session* session) {
    session->state = TlsSharedState_Open;
    if (session->unsent.count != 0) {
        writePlain(session, session->unsent.data, session->unsent.count);
        session->unsent.reset();
    }
    if (session->finishing && session->state == TlsSharedState_Open)
        sendCloseNotify(session);
    tlsSharedDeliver(session, PrNetStreamEvent_Open);
    if (!session->dead && session->state == TlsSharedState_Open) {
        advance(session);
        if (!session->dead && session->state == TlsSharedState_Open)
            drainTransport(session);
    }
}

// Delivers the result of a verification on the loop, and frees the job.
void verified(void* PR_NULLABLE context) {
    VerifyJob* job = static_cast<VerifyJob*>(context);
    Session* session = job->session;
    if (session != nullptr) {
        session->job = nullptr;
        tlsSharedHold(session);
        if (job->failure != PrTlsFailure_None) {
            tlsSharedFail(session, PrNetError_Protocol, job->failure);
        } else {
            session->certificate = CertDuplicateCertificateContext(job->certificate);
            open(session);
        }
        tlsSharedRelease(session);
    }
    CertFreeCertificateContext(job->certificate);
    if (job->anchors != nullptr)
        CertCloseStore(job->anchors, 0);
    prMemoryFree(job);
}

// Returns true if `certificate` is one of `anchors`.
bool isAnchor(HCERTSTORE anchors, PCCERT_CONTEXT certificate) {
    PCCERT_CONTEXT found = CertFindCertificateInStore(anchors, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, certificate, nullptr);
    if (found == nullptr)
        return false;
    CertFreeCertificateContext(found);
    return true;
}

// Builds the chain of the job's certificate and checks it with the SSL policy. A chain that ends in an anchor is
// trusted, though the system does not trust its root.
PrTlsFailure verify(const VerifyJob* job) {
    HCERTSTORE issuers = CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, nullptr);
    if (issuers == nullptr)
        return PrTlsFailure_Untrusted;
    CertAddStoreToCollection(issuers, job->certificate->hCertStore, 0, 0);
    if (job->anchors != nullptr)
        CertAddStoreToCollection(issuers, job->anchors, 0, 0);
    char serverAuth[] = szOID_PKIX_KP_SERVER_AUTH;
    LPSTR usages[] = {serverAuth};
    CERT_CHAIN_PARA chainPara{};
    chainPara.cbSize = sizeof(chainPara);
    chainPara.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    chainPara.RequestedUsage.Usage.cUsageIdentifier = 1;
    chainPara.RequestedUsage.Usage.rgpszUsageIdentifier = usages;
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    BOOL built = CertGetCertificateChain(nullptr, job->certificate, nullptr, issuers, &chainPara, 0, nullptr, &chain);
    CertCloseStore(issuers, 0);
    if (!built || chain == nullptr)
        return PrTlsFailure_Untrusted;
    DWORD allowed = 0;
    if (job->anchors != nullptr && (chain->TrustStatus.dwErrorStatus & CERT_TRUST_IS_UNTRUSTED_ROOT) != 0) {
        const CERT_SIMPLE_CHAIN* last = chain->rgpChain[chain->cChain - 1];
        if (isAnchor(job->anchors, last->rgpElement[last->cElement - 1]->pCertContext))
            allowed = CERT_CHAIN_POLICY_ALLOW_UNKNOWN_CA_FLAG;
    }
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
    ssl.cbSize = sizeof(ssl);
    ssl.dwAuthType = AUTHTYPE_SERVER;
    ssl.pwszServerName = const_cast<wchar_t*>(job->name);
    CERT_CHAIN_POLICY_PARA policyPara{};
    policyPara.cbSize = sizeof(policyPara);
    policyPara.dwFlags = allowed;
    policyPara.pvExtraPolicyPara = &ssl;
    CERT_CHAIN_POLICY_STATUS policy{};
    policy.cbSize = sizeof(policy);
    BOOL checked = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policyPara, &policy);
    CertFreeCertificateChain(chain);
    if (!checked)
        return PrTlsFailure_Untrusted;
    switch (policy.dwError) {
        case ERROR_SUCCESS:
            return PrTlsFailure_None;
        case static_cast<DWORD>(CERT_E_EXPIRED):
        case static_cast<DWORD>(CERT_E_VALIDITYPERIODNESTING):
            return PrTlsFailure_Expired;
        case static_cast<DWORD>(CERT_E_CN_NO_MATCH):
            return PrTlsFailure_NameMismatch;
        default:
            return PrTlsFailure_Untrusted;
    }
}

void serveVerifier(void* PR_NULLABLE argument) {
    SchannelTls* tls = static_cast<SchannelTls*>(argument);
    prMutexLock(&tls->mutex);
    for (;;) {
        while (tls->jobs.count == 0 && !tls->stopping)
            prConditionWait(&tls->jobsChanged, &tls->mutex);
        if (tls->jobs.count == 0)
            break;
        VerifyJob* job = tls->jobs.data[0];
        tls->jobs.erase(0);
        prMutexUnlock(&tls->mutex);
        job->failure = verify(job);
        prNetPost(job->net, verified, job);
        prMutexLock(&tls->mutex);
    }
    prMutexUnlock(&tls->mutex);
}

// Session functions

void destroySession(PrNetStream* base) {
    Session* session = static_cast<Session*>(reinterpret_cast<PrTlsSession*>(base));
    PR_ASSERT(!session->dead);
    session->dead = true;
    if (session->job != nullptr)
        session->job->session = nullptr;
    session->job = nullptr;
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
    if (session->state != TlsSharedState_Open) {
        PR_ASSERT(bytes.size <= UINT32_MAX - session->unsent.count);
        session->unsent.append(bytes.data, uint32_t(bytes.size));
        return;
    }
    writePlain(session, bytes.data, bytes.size);
}

// Source functions

void destroyTls(PrTls* base) {
    SchannelTls* tls = static_cast<SchannelTls*>(base);
    prMutexLock(&tls->mutex);
    tls->stopping = true;
    prConditionBroadcast(&tls->jobsChanged);
    prMutexUnlock(&tls->mutex);
    prThreadJoin(&tls->verifier);
    tls->jobs.reset();
    FreeCredentialsHandle(&tls->credentials);
    prConditionReset(&tls->jobsChanged);
    prMutexReset(&tls->mutex);
    prMemoryFree(tls);
}

// Writes `name` to `out` as UTF-16 with a terminator. Returns false for a name that is empty, too long, or holds a NUL
// or a byte that is not UTF-8.
bool copyName(PrStringView name, wchar_t* out) {
    if (name.size == 0 || name.size >= kNameMaxBytes)
        return false;
    for (size_t i = 0; i < name.size; ++i) {
        if (name.data[i] == 0)
            return false;
    }
    uint32_t written = prCoreWindowsWiden(name, out, uint32_t(kNameMaxBytes - 1), PrCoreWindowsString_Exact);
    if (written == 0 || written > kNameMaxBytes - 1)
        return false;
    out[written] = 0;
    return true;
}

// Returns a memory store of the session's anchors, or null if it has none. Returns InvalidData for an anchor that is
// not a DER certificate.
PrStatus openAnchors(const PrTlsSessionDesc* desc, HCERTSTORE PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (desc->anchorCount == 0)
        return PrStatus_Ok;
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
    if (store == nullptr)
        PR_FATAL("tls_schannel: no memory store for the anchors");
    for (uint32_t i = 0; i < desc->anchorCount; ++i) {
        PrByteView anchor = desc->anchors[i];
        if (anchor.size > UINT32_MAX ||
                !CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, anchor.data, DWORD(anchor.size),
                    CERT_STORE_ADD_ALWAYS, nullptr)) {
            CertCloseStore(store, 0);
            return PrStatus_InvalidData;
        }
    }
    *out = store;
    return PrStatus_Ok;
}

// Writes the session's protocols to `out` as the SEC_APPLICATION_PROTOCOLS that SChannel reads, offered with ALPN.
void protocolsOf(const PrTlsSessionDesc* desc, RawArray<uint8_t>* out) {
    RawArray<uint8_t> wire{};
    for (uint32_t i = 0; i < desc->protocolCount; ++i) {
        PrStringView protocol = desc->protocols[i];
        PR_ASSERT(protocol.size != 0 && protocol.size <= kProtocolMaxBytes);
        wire.push(uint8_t(protocol.size));
        wire.append(reinterpret_cast<const uint8_t*>(protocol.data), uint32_t(protocol.size));
    }
    PR_ASSERT(wire.count <= UINT16_MAX);
    uint32_t listAt = uint32_t(offsetof(SEC_APPLICATION_PROTOCOLS, ProtocolLists));
    uint32_t wireAt = uint32_t(offsetof(SEC_APPLICATION_PROTOCOL_LIST, ProtocolList));
    out->resize(listAt + wireAt + wire.count);
    prMemZero(out->data, out->count);
    SEC_APPLICATION_PROTOCOLS* protocols = reinterpret_cast<SEC_APPLICATION_PROTOCOLS*>(out->data);
    protocols->ProtocolListsSize = wireAt + wire.count;
    SEC_APPLICATION_PROTOCOL_LIST* list = protocols->ProtocolLists;
    list->ProtoNegoExt = SecApplicationProtocolNegotiationExt_ALPN;
    list->ProtocolListSize = uint16_t(wire.count);
    prMemCopy(out->data + listAt + wireAt, wire.data, wire.count);
    wire.reset();
}

// Starts the handshake: makes the context, with the client's first flight, which the transport queues until it opens.
void startHandshake(Session* session, const PrTlsSessionDesc* desc) {
    RawArray<uint8_t> protocols{};
    SecBuffer input{0, SECBUFFER_APPLICATION_PROTOCOLS, nullptr};
    SecBufferDesc inputDesc{SECBUFFER_VERSION, 1, &input};
    if (desc->protocolCount != 0) {
        protocolsOf(desc, &protocols);
        input.cbBuffer = protocols.count;
        input.pvBuffer = protocols.data;
    }
    SecBuffer output{0, SECBUFFER_TOKEN, nullptr};
    SecBufferDesc outputDesc{SECBUFFER_VERSION, 1, &output};
    ULONG attributes = 0;
    SECURITY_STATUS status = InitializeSecurityContextW(&session->tls->credentials, nullptr, session->name,
        kContextRequest, 0, 0, desc->protocolCount != 0 ? &inputDesc : nullptr, 0,
        &session->context, &outputDesc, &attributes, nullptr);
    protocols.reset();
    if (status == SEC_I_CONTINUE_NEEDED) {
        session->hasContext = true;
        sendToken(session, &output);
        return;
    }
    if (output.pvBuffer != nullptr)
        FreeContextBuffer(output.pvBuffer);
    tlsSharedFailLater(session, PrNetError_Protocol, PrTlsFailure_Handshake);
}

// SChannel's steps, which the shared calls take.
constexpr TlsSharedEngine kEngine = {
    .advance = [](TlsSharedSession* session) { advance(static_cast<Session*>(session)); },
    .drainTransport = [](TlsSharedSession* session) { drainTransport(static_cast<Session*>(session)); },
    .sendCloseNotify = [](TlsSharedSession* session) { sendCloseNotify(static_cast<Session*>(session)); },
    .free = [](TlsSharedSession* session) { freeSession(static_cast<Session*>(session)); },
};

PrStatus connectTls(PrTls* base, const PrTlsSessionDesc* desc, PrTlsSession* PR_NULLABLE* PR_NONNULL out) {
    SchannelTls* tls = static_cast<SchannelTls*>(base);
    wchar_t name[kNameMaxBytes];
    if (!copyName(desc->serverName.data != nullptr ? desc->serverName : desc->host, name))
        return PrStatus_InvalidData;
    HCERTSTORE anchors = nullptr;
    PrStatus status = openAnchors(desc, &anchors);
    if (status != PrStatus_Ok)
        return status;
    Session* session = allocZeroed<Session>();
    session->engine = &kEngine;
    session->net = tls->net;
    session->event = desc->event;
    session->context = desc->context;
    session->stream.destroyFn = destroySession;
    session->stream.setEventFn = tlsSharedSetEvent;
    session->stream.readFn = tlsSharedRead;
    session->stream.writeFn = writeSession;
    session->stream.finishFn = tlsSharedFinish;
    session->stream.queuedFn = tlsSharedQueued;
    session->stream.errorFn = tlsSharedError;
    session->infoFn = tlsSharedInfo;
    session->tls = tls;
    session->anchors = anchors;
    prMemCopy(session->name, name, sizeof(name));
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
    startHandshake(session, desc);
    *out = session;
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTls* prTlsSchannelCreate(PrNet* net) {
    SchannelTls* tls = allocZeroed<SchannelTls>();
    tls->net = net;
    tls->destroyFn = destroyTls;
    tls->connectFn = connectTls;
    TLS_PARAMETERS parameters{};
    parameters.grbitDisabledProtocols = ~DWORD(SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT);
    SCH_CREDENTIALS credentials{};
    credentials.dwVersion = SCH_CREDENTIALS_VERSION;
    credentials.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    credentials.cTlsParameters = 1;
    credentials.pTlsParameters = &parameters;
    SECURITY_STATUS status = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W),
        SECPKG_CRED_OUTBOUND, nullptr, &credentials, nullptr, nullptr, &tls->credentials, nullptr);
    if (status != SEC_E_OK)
        PR_FATAL("tls_schannel: no SChannel credentials for TLS 1.2 and 1.3");
    prMutexInit(&tls->mutex);
    prConditionInit(&tls->jobsChanged);
    prThreadStart(&tls->verifier, serveVerifier, tls);
    return tls;
}

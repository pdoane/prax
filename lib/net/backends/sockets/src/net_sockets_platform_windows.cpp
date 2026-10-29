// The system's sockets on Windows (net_sockets_platform.h): Winsock, waited on with WSAPoll, and woken by a datagram
// that a loopback socket sends to itself.

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/mem.h"

#include "net/backends/sockets/src/net_sockets_platform.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
// mstcpip.h, which declares the TCP ioctls, needs Winsock's declarations before it, so a directive ends the sorted run
// of includes above.
#if !defined(SIO_TCP_INITIAL_RTO)
#include <mstcpip.h>
#endif

namespace pr_impl {

namespace {

// The longest domain name (rfc 1035), with room for its terminator.
constexpr uint32_t kHostMaxBytes = 254;

static_assert(sizeof(SocketsAddress::storage) >= sizeof(sockaddr_storage), "an address holds any socket address");
static_assert(SocketsHandle(INVALID_SOCKET) == kSocketsHandleNone, "no socket is INVALID_SOCKET");

void setNonBlocking(SOCKET socket) {
    u_long on = 1;
    ioctlsocket(socket, FIONBIO, &on);
}

// Opens a socket that child processes do not inherit.
SOCKET openSocket(int family, int type, int protocol) {
    return WSASocketW(family, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

PrNetError errorOfWsa(int code) {
    switch (code) {
        case WSAECONNREFUSED:
            return PrNetError_Refused;
        case WSAENETUNREACH:
        case WSAEHOSTUNREACH:
        case WSAEADDRNOTAVAIL:
        case WSAENETDOWN:
            return PrNetError_Unreachable;
        case WSAETIMEDOUT:
            return PrNetError_TimedOut;
        case WSAECONNRESET:
        case WSAECONNABORTED:
        case WSAENETRESET:
        case WSAESHUTDOWN:
            return PrNetError_Reset;
        default:
            return PrNetError_Io;
    }
}

// The error that the socket holds, or zero.
int pendingError(SOCKET socket) {
    int code = 0;
    int length = sizeof(code);
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length) != 0)
        code = WSAGetLastError();
    return code;
}

// Returns true if the address is on the loopback: in 127.0.0.0/8, or ::1.
bool isLoopback(const sockaddr_storage& storage) {
    if (storage.ss_family == AF_INET) {
        const sockaddr_in* v4 = reinterpret_cast<const sockaddr_in*>(&storage);
        return ntohl(v4->sin_addr.s_addr) >> 24 == 127;
    }
    if (storage.ss_family == AF_INET6)
        return IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_addr);
    return false;
}

// Opens the wake socket: a UDP socket bound to a port of the loopback, and connected to that port.
SOCKET openWake() {
    SOCKET wake = openSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (wake == INVALID_SOCKET)
        return INVALID_SOCKET;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    if (bind(wake, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
        || getsockname(wake, reinterpret_cast<sockaddr*>(&address), &length) != 0
        || ::connect(wake, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(wake);
        return INVALID_SOCKET;
    }
    setNonBlocking(wake);
    return wake;
}

} // namespace

// A UDP socket on the loopback, connected to itself, and the poll's entries in the system's form, that socket first.
// WSAPoll waits only on sockets, so a datagram that the socket sends to itself wakes it.
struct SocketsSystem {
    SOCKET wake;
    Array<WSAPOLLFD> polls;
};

SocketsSystem* socketsSystemCreate() {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        PR_FATAL("net_sockets: Winsock 2.2 is unavailable");
    SocketsSystem* system = allocZeroed<SocketsSystem>();
    system->wake = openWake();
    if (system->wake == INVALID_SOCKET)
        PR_FATAL("net_sockets: no loopback socket for the loop");
    return system;
}

void socketsSystemDestroy(SocketsSystem* system) {
    closesocket(system->wake);
    system->polls.reset();
    prMemoryFree(system);
    WSACleanup();
}

void socketsSystemWake(SocketsSystem* system) {
    char byte = 1;
    // A full buffer already wakes the poll.
    send(system->wake, &byte, 1, 0);
}

bool socketsSystemPoll(SocketsSystem* system, SocketsPollEntry* entries, uint32_t count, bool block) {
    system->polls.clear();
    system->polls.push(WSAPOLLFD{.fd = system->wake, .events = POLLIN});
    for (uint32_t i = 0; i < count; ++i) {
        SHORT events = 0;
        if ((entries[i].events & SocketsEventFlag_Read) != 0)
            events |= POLLIN;
        if ((entries[i].events & SocketsEventFlag_Write) != 0)
            events |= POLLOUT;
        system->polls.push(WSAPOLLFD{.fd = SOCKET(entries[i].handle), .events = events});
        entries[i].ready = 0;
    }
    if (WSAPoll(system->polls.data, ULONG(system->polls.count), block ? -1 : 0) < 0)
        return false;
    if ((system->polls.data[0].revents & POLLIN) != 0) {
        char drained[64];
        while (recv(system->wake, drained, sizeof(drained), 0) > 0) {
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        SHORT revents = system->polls.data[i + 1].revents;
        SocketsEventFlags ready = 0;
        if ((revents & POLLIN) != 0)
            ready |= SocketsEventFlag_Read;
        if ((revents & POLLOUT) != 0)
            ready |= SocketsEventFlag_Write;
        if ((revents & POLLERR) != 0)
            ready |= SocketsEventFlag_Error;
        if ((revents & POLLHUP) != 0)
            ready |= SocketsEventFlag_HangUp;
        entries[i].ready = ready;
    }
    return true;
}

SocketsHandle socketsConnect(const SocketsAddress& address, PrNetError* error) {
    const sockaddr_storage* storage = reinterpret_cast<const sockaddr_storage*>(address.storage);
    SOCKET socket = openSocket(storage->ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        *error = errorOfWsa(WSAGetLastError());
        return kSocketsHandleNone;
    }
    setNonBlocking(socket);
    BOOL on = TRUE;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
    // Windows answers a reset to its SYN with more SYNs, for about a second, before it reports the refusal. The
    // loopback loses no SYN, so a connection there sends one, and a refused one fails at once.
    if (isLoopback(*storage)) {
        TCP_INITIAL_RTO_PARAMETERS rto{TCP_INITIAL_RTO_UNSPECIFIED_RTT, TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS};
        DWORD returned = 0;
        WSAIoctl(socket, SIO_TCP_INITIAL_RTO, &rto, sizeof(rto), nullptr, 0, &returned, nullptr, nullptr);
    }
    if (::connect(socket, reinterpret_cast<const sockaddr*>(storage), int(address.length)) == 0
        || WSAGetLastError() == WSAEWOULDBLOCK)
        return SocketsHandle(socket);
    *error = errorOfWsa(WSAGetLastError());
    closesocket(socket);
    return kSocketsHandleNone;
}

PrNetError socketsConnectResult(SocketsHandle socket, SocketsEventFlags ready) {
    int code = pendingError(SOCKET(socket));
    // WSAPoll reports a failed connection with POLLERR or POLLHUP, whether or not SO_ERROR says why.
    if (code != 0)
        return errorOfWsa(code);
    return (ready & (SocketsEventFlag_Error | SocketsEventFlag_HangUp)) != 0 ? PrNetError_Io : PrNetError_None;
}

PrNetError socketsError(SocketsHandle socket) {
    int code = pendingError(SOCKET(socket));
    return code != 0 ? errorOfWsa(code) : PrNetError_Io;
}

void socketsClose(SocketsHandle socket) {
    closesocket(SOCKET(socket));
}

int64_t socketsReceive(SocketsHandle socket, uint8_t* out, uint32_t capacity, PrNetError* error) {
    int got = recv(SOCKET(socket), reinterpret_cast<char*>(out), capacity < INT32_MAX ? int(capacity) : INT32_MAX, 0);
    if (got >= 0)
        return got;
    int code = WSAGetLastError();
    *error = code == WSAEWOULDBLOCK ? PrNetError_None : errorOfWsa(code);
    return -1;
}

int64_t socketsSend(SocketsHandle socket, const uint8_t* data, uint32_t size, PrNetError* error) {
    int sent = send(SOCKET(socket), reinterpret_cast<const char*>(data), size < INT32_MAX ? int(size) : INT32_MAX, 0);
    if (sent >= 0)
        return sent;
    int code = WSAGetLastError();
    *error = code == WSAEWOULDBLOCK ? PrNetError_None : errorOfWsa(code);
    return -1;
}

void socketsShutdownSend(SocketsHandle socket) {
    shutdown(SOCKET(socket), SD_SEND);
}

PrNetError socketsResolve(const char* host, uint16_t port, Array<SocketsAddress>* out) {
    wchar_t wide[kHostMaxBytes + 1];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host, -1, wide, int(countOf(wide))) == 0)
        return PrNetError_NameNotResolved;
    wchar_t service[8];
    uint32_t digits = 0;
    wchar_t reversed[8];
    uint32_t value = port;
    do {
        reversed[digits++] = wchar_t(L'0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (uint32_t i = 0; i < digits; ++i)
        service[i] = reversed[digits - 1 - i];
    service[digits] = 0;
    // AI_ADDRCONFIG is left out because Windows does not count the loopback as configured, so a machine with no other
    // address could not resolve localhost. The system orders the addresses with the families that it reaches first.
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW* found = nullptr;
    int status = GetAddrInfoW(wide, service, &hints, &found);
    if (status != 0)
        return status == WSA_NOT_ENOUGH_MEMORY || status == WSANOTINITIALISED ? PrNetError_Io
                                                                              : PrNetError_NameNotResolved;
    uint32_t before = out->count;
    for (ADDRINFOW* entry = found; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_addrlen > sizeof(sockaddr_storage))
            continue;
        SocketsAddress address{};
        memCopy(address.storage, entry->ai_addr, entry->ai_addrlen);
        address.length = uint32_t(entry->ai_addrlen);
        out->push(address);
    }
    FreeAddrInfoW(found);
    return out->count != before ? PrNetError_None : PrNetError_NameNotResolved;
}

} // namespace pr_impl

// The system's sockets on POSIX (net_sockets_platform.h): BSD sockets, waited on with poll, and woken by a pipe.

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"

#include "net/backends/sockets/src/net_sockets_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pr_impl {

namespace {

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

static_assert(sizeof(SocketsAddress::storage) >= sizeof(sockaddr_storage), "an address holds any socket address");

void setNonBlocking(int descriptor) {
    fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) | O_NONBLOCK);
    fcntl(descriptor, F_SETFD, FD_CLOEXEC);
}

PrNetError errorOfErrno(int code) {
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

// The error that the socket holds, or zero.
int pendingError(SocketsHandle socket) {
    int code = 0;
    socklen_t length = sizeof(code);
    if (getsockopt(int(socket), SOL_SOCKET, SO_ERROR, &code, &length) != 0)
        code = errno;
    return code;
}

bool wouldBlock(int code) {
    return code == EAGAIN || code == EWOULDBLOCK || code == EINTR;
}

} // namespace

// A pipe whose read end wakes the poll, and the poll's entries in the system's form, the pipe first.
struct SocketsSystem {
    int wake[2];
    Array<pollfd> polls;
};

SocketsSystem* socketsSystemCreate() {
    SocketsSystem* system = allocZeroed<SocketsSystem>();
    if (pipe(system->wake) != 0)
        PR_FATAL("net_sockets: no pipe for the loop");
    setNonBlocking(system->wake[0]);
    setNonBlocking(system->wake[1]);
    return system;
}

void socketsSystemDestroy(SocketsSystem* system) {
    close(system->wake[0]);
    close(system->wake[1]);
    system->polls.reset();
    prMemoryFree(system);
}

void socketsSystemWake(SocketsSystem* system) {
    uint8_t byte = 1;
    // A full pipe already wakes the poll.
    (void)!write(system->wake[1], &byte, 1);
}

bool socketsSystemPoll(SocketsSystem* system, SocketsPollEntry* entries, uint32_t count, bool block) {
    system->polls.clear();
    system->polls.push(pollfd{system->wake[0], POLLIN, 0});
    for (uint32_t i = 0; i < count; ++i) {
        short events = 0;
        if ((entries[i].events & SocketsEventFlag_Read) != 0)
            events |= POLLIN;
        if ((entries[i].events & SocketsEventFlag_Write) != 0)
            events |= POLLOUT;
        system->polls.push(pollfd{int(entries[i].handle), events, 0});
        entries[i].ready = 0;
    }
    if (poll(system->polls.data, nfds_t(system->polls.count), block ? -1 : 0) < 0)
        return false;
    if ((system->polls.data[0].revents & POLLIN) != 0) {
        uint8_t drained[64];
        while (read(system->wake[0], drained, sizeof(drained)) > 0) {
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        short revents = system->polls.data[i + 1].revents;
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
    int descriptor = ::socket(storage->ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (descriptor < 0) {
        *error = errorOfErrno(errno);
        return kSocketsHandleNone;
    }
    setNonBlocking(descriptor);
    int on = 1;
    setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
#if defined(SO_NOSIGPIPE)
    setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(storage), socklen_t(address.length)) == 0
        || errno == EINPROGRESS)
        return descriptor;
    *error = errorOfErrno(errno);
    close(descriptor);
    return kSocketsHandleNone;
}

PrNetError socketsConnectResult(SocketsHandle socket, SocketsEventFlags) {
    int code = pendingError(socket);
    return code != 0 ? errorOfErrno(code) : PrNetError_None;
}

PrNetError socketsError(SocketsHandle socket) {
    int code = pendingError(socket);
    return code != 0 ? errorOfErrno(code) : PrNetError_Io;
}

void socketsClose(SocketsHandle socket) {
    close(int(socket));
}

int64_t socketsReceive(SocketsHandle socket, uint8_t* out, uint32_t capacity, PrNetError* error) {
    ssize_t got = recv(int(socket), out, capacity, 0);
    if (got >= 0)
        return got;
    *error = wouldBlock(errno) ? PrNetError_None : errorOfErrno(errno);
    return -1;
}

int64_t socketsSend(SocketsHandle socket, const uint8_t* data, uint32_t size, PrNetError* error) {
    ssize_t sent = send(int(socket), data, size, kSendFlags);
    if (sent >= 0)
        return sent;
    *error = wouldBlock(errno) ? PrNetError_None : errorOfErrno(errno);
    return -1;
}

void socketsShutdownSend(SocketsHandle socket) {
    shutdown(int(socket), SHUT_WR);
}

PrNetError socketsResolve(const char* host, uint16_t port, Array<SocketsAddress>* out) {
    char service[8];
    uint32_t digits = 0;
    char reversed[8];
    uint32_t value = port;
    do {
        reversed[digits++] = char('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (uint32_t i = 0; i < digits; ++i)
        service[i] = reversed[digits - 1 - i];
    service[digits] = 0;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_ADDRCONFIG;
    addrinfo* found = nullptr;
    int status = getaddrinfo(host, service, &hints, &found);
    if (status != 0)
        return status == EAI_SYSTEM ? errorOfErrno(errno) : PrNetError_NameNotResolved;
    uint32_t before = out->count;
    for (addrinfo* entry = found; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_addrlen > sizeof(sockaddr_storage))
            continue;
        SocketsAddress address{};
        memCopy(address.storage, entry->ai_addr, entry->ai_addrlen);
        address.length = uint32_t(entry->ai_addrlen);
        out->push(address);
    }
    freeaddrinfo(found);
    return out->count != before ? PrNetError_None : PrNetError_NameNotResolved;
}

} // namespace pr_impl

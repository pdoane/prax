#include "tools/shell/server_control.h"

#include <cstddef>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(_WIN32)

ServerControl::~ServerControl() {}
bool ServerControl::connect(uint16_t) { return false; }
bool ServerControl::connected() const { return false; }
bool ServerControl::ask(ServerRequests*) { return false; }
bool ServerControl::wake(uint64_t) { return false; }
void ServerControl::close() {}
bool ServerControl::send(const char*, size_t) { return false; }
bool ServerControl::readLine(std::vector<char>*) { return false; }

#else

namespace {

// Returns true if `address` is on the loopback.
bool loopback(const sockaddr_storage& address) {
    if (address.ss_family == AF_INET)
        return (ntohl(reinterpret_cast<const sockaddr_in&>(address).sin_addr.s_addr) >> 24) == 127;
    if (address.ss_family == AF_INET6) {
        // A copy, since some C libraries' macros cast what they test to a pointer that is not const.
        in6_addr a = reinterpret_cast<const sockaddr_in6&>(address).sin6_addr;
        return IN6_IS_ADDR_LOOPBACK(&a) || (IN6_IS_ADDR_V4MAPPED(&a) && a.s6_addr[12] == 127);
    }
    return false;
}

uint16_t portOf(const sockaddr_storage& address) {
    if (address.ss_family == AF_INET)
        return ntohs(reinterpret_cast<const sockaddr_in&>(address).sin_port);
    return ntohs(reinterpret_cast<const sockaddr_in6&>(address).sin6_port);
}

// Appends the local ports of the process's TCP connections to the loopback, apart from `except`, as the process's
// descriptors list them.
bool loopbackPorts(int except, std::string* out) {
    DIR* directory = opendir("/dev/fd");
    if (directory == nullptr)
        return false;
    std::vector<int> descriptors;
    while (dirent* entry = readdir(directory)) {
        char* end = nullptr;
        long fd = strtol(entry->d_name, &end, 10);
        if (end != entry->d_name && *end == '\0' && fd != except && fd != dirfd(directory))
            descriptors.push_back(int(fd));
    }
    closedir(directory);
    for (int fd : descriptors) {
        int type = 0;
        socklen_t size = sizeof type;
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_STREAM)
            continue;
        sockaddr_storage local{};
        sockaddr_storage peer{};
        socklen_t localSize = sizeof local;
        socklen_t peerSize = sizeof peer;
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localSize) != 0 ||
                getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peerSize) != 0 || !loopback(peer))
            continue;
        *out += " " + std::to_string(portOf(local));
    }
    return true;
}

} // namespace

ServerControl::~ServerControl() {
    close();
}

bool ServerControl::connect(uint16_t port) {
    close();
    std::string ports;
    if (!loopbackPorts(-1, &ports))
        return false;
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0)
        return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int one = 1;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (::connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
        close();
        return false;
    }
    return true;
}

bool ServerControl::connected() const {
    return socket_ >= 0;
}

void ServerControl::close() {
    if (socket_ >= 0)
        ::close(socket_);
    socket_ = -1;
    buffered_.clear();
}

bool ServerControl::send(const char* text, size_t size) {
    while (size != 0) {
        ssize_t sent = ::send(socket_, text, size, 0);
        if (sent <= 0) {
            close();
            return false;
        }
        text += sent;
        size -= size_t(sent);
    }
    return true;
}

bool ServerControl::readLine(std::vector<char>* line) {
    for (;;) {
        for (size_t i = 0; i < buffered_.size(); ++i) {
            if (buffered_[i] == '\n') {
                line->assign(buffered_.begin(), buffered_.begin() + std::ptrdiff_t(i));
                line->push_back('\0');
                buffered_.erase(buffered_.begin(), buffered_.begin() + std::ptrdiff_t(i + 1));
                return true;
            }
        }
        char chunk[4096];
        ssize_t got = ::recv(socket_, chunk, sizeof chunk, 0);
        if (got <= 0) {
            close();
            return false;
        }
        buffered_.insert(buffered_.end(), chunk, chunk + got);
    }
}

bool ServerControl::ask(ServerRequests* out) {
    *out = {};
    if (socket_ < 0)
        return false;
    std::string request = "ask";
    if (!loopbackPorts(socket_, &request)) {
        close();
        return false;
    }
    request += "\n";
    std::vector<char> line;
    if (!send(request.data(), request.size()) || !readLine(&line))
        return false;
    unsigned long count = strtoul(line.data(), nullptr, 10);
    for (unsigned long i = 0; i < count; ++i) {
        if (!readLine(&line))
            return false;
        unsigned port = 0;
        char state[16] = {};
        int used = 0;
        if (sscanf(line.data(), "%u %15s %n", &port, state, &used) < 2)
            continue;
        const char* rest = line.data() + used;
        if (strcmp(state, "sleep") == 0) {
            unsigned long long id = 0;
            double seconds = 0;
            int target = 0;
            if (sscanf(rest, "%llu %lf %n", &id, &seconds, &target) >= 2)
                out->sleeps.push_back({.id = id, .milliseconds = seconds * 1000, .target = rest + target});
        } else {
            ++out->busy;
            out->busyTargets.push_back(rest);
        }
    }
    return true;
}

bool ServerControl::wake(uint64_t id) {
    if (socket_ < 0)
        return false;
    std::string line = "wake " + std::to_string(id) + "\n";
    return send(line.data(), line.size());
}

#endif

#pragma once

#include "pr/base.h"

#include <stdint.h>
#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

// A connection to a server's control (tools/suite/wpt/wpt_serve.py, Control), which says what the server does with the
// requests of this process's connections to the loopback, so that a virtual clock moves for a request only while it
// sleeps on purpose (PageHost::setServerControl).

// A request that sleeps on purpose: its sleep, as the server numbers them, and how long it asked to sleep.
struct ServerSleep {
    uint64_t id;
    double milliseconds;
    std::string target;              // the request's target, as its request line gives it
};

// What the server does with the requests of the process's connections.
struct ServerRequests {
    uint32_t busy = 0;                    // the requests that the server handles and that do not sleep
    std::vector<std::string> busyTargets; // their targets, as their request lines give them
    std::vector<ServerSleep> sleeps;
};

// The connection (connect), which the process keeps until it closes, or until an ask or a wake fails.
class ServerControl {
public:
    ServerControl() = default;
    ServerControl(const ServerControl&) = delete;
    ServerControl& operator=(const ServerControl&) = delete;
    ~ServerControl();

    // Connects to the control at `port` of the loopback. Returns false if it cannot, or if the platform cannot list the
    // process's connections.
    bool connect(uint16_t port);
    bool connected() const;

    // Asks what the server does with the requests of the process's connections to the loopback, apart from this one.
    // Returns false, and closes the connection, if it fails.
    bool ask(ServerRequests* out);

    // Ends the sleep `id` now. Returns false, and closes the connection, if it fails.
    bool wake(uint64_t id);

private:
    void close();
    bool send(const char* text, size_t size);
    bool readLine(std::vector<char>* line);

    int socket_ = -1;
    std::vector<char> buffered_;     // what the control sent past the last line read
};

PR_ASSUME_NONNULL_END

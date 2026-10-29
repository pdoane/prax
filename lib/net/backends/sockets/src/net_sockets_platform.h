// The system's sockets, behind the calls that the network's loop makes: opening, connecting, polling, reading, writing
// and resolving. Each platform implements them in a source of its own, and the loop includes no system header.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/net.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A socket of the system: a descriptor, or a SOCKET on Windows.
typedef intptr_t SocketsHandle;

// The handle of no socket, which is -1 as a descriptor and INVALID_SOCKET as a SOCKET.
constexpr SocketsHandle kSocketsHandleNone = -1;

// An address that a name resolved to, as the system stores one (sockaddr_storage).
struct SocketsAddress {
    alignas(8) uint8_t storage[128];
    uint32_t length;
};

// What a poll waits for on a socket, and what it found.
enum {
    SocketsEventFlag_Read = 1u << 0,
    SocketsEventFlag_Write = 1u << 1,
    SocketsEventFlag_Error = 1u << 2,
    SocketsEventFlag_HangUp = 1u << 3,
};
typedef uint8_t SocketsEventFlags;

// A socket that a poll waits on: what it waits for, and what was ready when the poll returned.
struct SocketsPollEntry {
    SocketsHandle handle;
    SocketsEventFlags events;
    SocketsEventFlags ready;
};

// The system's sockets as a network uses them: whatever the platform starts, the socket that wakes a poll, and the
// storage of a poll's entries in the system's own form.
struct SocketsSystem;

// Creates the system's sockets, or ends the process if they cannot wake a poll. Destroying them releases what the
// platform started.
SocketsSystem* socketsSystemCreate();
void socketsSystemDestroy(SocketsSystem* system);

// Wakes the poll that waits on `system`, from any thread. A poll already woken stays woken.
void socketsSystemWake(SocketsSystem* system);

// Waits until a socket of `entries` or the system's wake is ready, or returns at once if `block` is false, and sets
// each entry's `ready`. A wake is consumed. Returns false if the poll failed, which leaves every `ready` empty.
bool socketsSystemPoll(SocketsSystem* system, SocketsPollEntry* entries, uint32_t count, bool block);

// Opens a socket for TCP, which neither blocks nor passes to child processes, and starts connecting it to `address`.
// Returns the socket, or kSocketsHandleNone and writes why to `error` if the connection failed at once.
SocketsHandle socketsConnect(const SocketsAddress& address, PrNetError* error);

// Returns why the connection that a socket started failed, or PrNetError_None if it is open, once a poll found it
// `ready`.
PrNetError socketsConnectResult(SocketsHandle socket, SocketsEventFlags ready);

// Returns the error that the socket holds, or PrNetError_Io if it holds none.
PrNetError socketsError(SocketsHandle socket);

// Closes the socket.
void socketsClose(SocketsHandle socket);

// Reads up to `capacity` bytes. Returns the count read, zero once the peer ended, or -1 if none waits, with `error` set
// to PrNetError_None, or if the read failed, with `error` set to why.
int64_t socketsReceive(SocketsHandle socket, uint8_t* out, uint32_t capacity, PrNetError* error);

// Sends up to `size` bytes. Returns the count sent, or -1 if the socket takes none now, with `error` set to
// PrNetError_None, or if the send failed, with `error` set to why.
int64_t socketsSend(SocketsHandle socket, const uint8_t* data, uint32_t size, PrNetError* error);

// Shuts down the sending side of the socket.
void socketsShutdownSend(SocketsHandle socket);

// Resolves the NUL-terminated `host` with the system's resolver, and appends its addresses for TCP to `out`, in the
// order that the system prefers. Returns PrNetError_None, or why it resolved to no address.
PrNetError socketsResolve(const char* host, uint16_t port, Array<SocketsAddress>* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END

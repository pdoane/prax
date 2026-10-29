// The network over the system's sockets: BSD sockets, or Winsock on Windows.

#pragma once

#include "pr/base.h"
#include "pr/net.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a network over the system's sockets. Its loop is a thread of its own, which waits on the sockets with poll,
// or with WSAPoll on Windows, which needs Windows 10 version 2004 or later, whose WSAPoll reports a connection that
// fails. Names resolve on a second thread, with the system's resolver. On Windows, the network initializes Winsock, and
// releases it when it is destroyed.
PrNet* prNetSocketsCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END

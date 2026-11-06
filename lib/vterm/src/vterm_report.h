// The terminal's answers to a program's requests for its settings and its capabilities, and its reports of its color
// scheme.

#pragma once

#include "pr/base.h"

#include "vterm/src/vterm_parser.h"
#include "vterm/src/vterm_screen.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Answers a device control string that requests the setting of a control function (DECRQSS) or terminfo capabilities
// (XTGETTCAP). The terminal ignores other device control strings.
void vtermDeviceControl(Vterm* vterm, const VtermAction* action);

// Queues a report of the color scheme. The report answers a query, and also announces a change of the scheme.
void vtermColorSchemeReport(Vterm* vterm);

} // namespace pr_impl

PR_ASSUME_NONNULL_END

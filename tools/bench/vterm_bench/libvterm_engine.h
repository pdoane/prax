// The vterm benchmark's adapter for libvterm, by Paul Evans (vterm_engine.h). It uses libvterm's screen layer.
//
// libvterm's screen layer keeps the screen's cells and hands each line that scrolls off the top to its client. The
// adapter is that client. It keeps those lines in its own scrollback, up to the limit that the terminal was created
// with, and gives them back when the screen grows taller, so that the terminal keeps what a PrVterm keeps. The terminal
// has an alternate screen, and rewraps the lines of its screen when the width changes. libvterm does not rewrap the
// scrollback, because its client holds it.

#pragma once

#include "tools/bench/vterm_bench/vterm_engine.h"

#include <memory>

PR_ASSUME_NONNULL_BEGIN

namespace vterm_bench {

// Returns the adapter for libvterm.
std::unique_ptr<Engine> makeLibvterm();

} // namespace vterm_bench

PR_ASSUME_NONNULL_END

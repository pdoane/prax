"""Compares the Prax terminal app (apps/terminal) end to end with other terminal applications on macOS: Ghostty, kitty,
WezTerm, Alacritty, iTerm2 and Terminal.app, whichever are installed.

Each terminal is launched once, with terminal_feed (tools/bench/terminal_feed.cpp) as its program. It is asked for
120x40 cells, a font of 13 points and SCROLLBACK lines of scrollback, if the terminal accepts these without the user's
configuration. The feed runs the workloads one at a time when the runner asks over a FIFO, and the runner reads the
counters of the terminal's processes between runs. The terminals run one after another, so that no terminal's window
covers another's, because a covered window may skip drawing. A terminal launched by its executable need not come to
the front, so the runner activates it once its window is open. The runner activates it again before a run if another
window covers it (tools/bench/windows.py). Activation takes the keyboard focus from the application that the user is
in.

A run is invalid, and is left out of the tables' values and the geometric means, in three cases. Its window was covered
or off screen at any sample during the run. Its terminal has a Metal client that spent no GPU time on it. Or its CPU
time is a small share of that of the terminal's other runs of the workload. Each is a sign that the terminal did not
draw what it was sent. A run whose window was covered is measured again, a few times at most, because the user may
have clicked another application and brought it to the front. A workload whose runs are all invalid shows ?, and the
notes say why.

- Wall time runs from the start of a run to the terminal's answer to a cursor position report (DSR 6) that follows the
  workload. A terminal answers DSR when it parses it, after everything before it, so the answer marks the end of the
  terminal's parsing. The end of the feed's writes does not, because the writes return once the pty has buffered the
  bytes.
- CPU time (user and system), the instructions retired, and the CPU's energy (proc_pid_rusage, rusage_info_v6:
  ri_energy_nj) are summed over the terminal's processes. These are the processes whose executable is in the
  terminal's application bundle or is one of its helpers. The measures run from the start until the terminal goes idle
  after its answer, so the frames that it draws after parsing count. The energy is that of the cores alone. It leaves
  out the GPU, the memory and the display.
- GPU time is the accumulated time of the Metal clients of the terminal's processes, over the same span. The GPU
  driver publishes it in the I/O Registry (AGXDeviceUserClient's AppUsage, which Activity Monitor's GPU column reads).
  The compositor's work for the windows belongs to WindowServer and is not counted.
- Memory is the physical footprint of the terminal's processes (ri_phys_footprint), as Activity Monitor's Memory
  column shows it. It is read at idle after the launch and after the scrollback fill, each time as the least over
  MEMORY_WINDOW. Its peak is read during each workload (ri_interval_max_phys_footprint, whose interval the runner
  restarts before each run). The peak is the sum of the peaks of the processes, which they need not have reached at
  the same time. The idle footprint is also broken down by footprint(1)'s categories of memory, such as the drawables
  of a window (IOSurface), the GPU's memory, malloc's zones, and mimalloc's memory (App-Specific Tag 1).

A run starts once the terminal is idle, with an empty screen and scrollback. The scrollback fill runs first, and once.
Every other workload runs once on part of its bytes or frames to warm up, and then --runs times. A throughput workload
reports MiB per second, and a paced one reports its costs per second of frames. A run that takes longer than its
timeout is stopped, and the workload is reported as taking longer than the timeout. Each workload's row is printed when
the workload finishes. The comparison of the terminals is printed once all have run, or once the run is stopped.
Nothing is recorded.

Terminal.app and iTerm2 are driven by AppleScript. macOS allows that only once the user lets this process control them
(System Settings > Privacy & Security > Automation). Each runs all of its windows in one process, so the benchmark
measures one only if it is not already running. The benchmark launches the application, keeps only the benchmark's
window, and quits the application at the end.
"""

import ctypes
import errno
import json
import math
import os
import plistlib
import re
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from tools.bench import common
from tools.bench import windows

# The grid and the font that every terminal is asked for, and the lines of scrollback that it keeps.
COLUMNS = 120
ROWS = 40
FONT_SIZE = 13
SCROLLBACK = 100000

# The measured runs of each workload after its warm-up, and the share of a workload that the warm-up writes.
RUNS = 2
WARM_SHARE = 0.2

# How long a terminal may take to start the feed, and how long a run may take past its frames, in seconds. The feed
# stops writing at the timeout and waits for the answer to DSR for half a minute more (terminal_feed.cpp).
LAUNCH_TIMEOUT = 30
RUN_TIMEOUT = 10
ANSWER_TIMEOUT = 45

# A terminal is idle once its processes spend less than IDLE_SHARE of a core over IDLE_WINDOW seconds. The runner waits
# for that up to SETTLE_LIMIT seconds after a launch, and up to TAIL_LIMIT seconds after a run.
IDLE_WINDOW = 0.2
IDLE_SHARE = 0.03
SETTLE_LIMIT = 8.0
TAIL_LIMIT = 3.0
SAMPLE = 0.025

# The seconds over which an idle footprint is read, as its least: a terminal may release memory, such as a renderer's
# buffers, a while after it goes idle.
MEMORY_WINDOW = 2.0

# How long the runner tries to bring a terminal's window to the front, in seconds. A run is invalid if more than
# COVER_LIMIT of the window was covered at any time during it, and is measured again up to COVER_RETRIES times. A run
# is invalid too if its CPU time is below SUSPECT_CPU_SHARE of the most that another run of the workload took.
FRONT_TIMEOUT = 3.0
COVER_LIMIT = 0.02
COVER_RETRIES = 2
SUSPECT_CPU_SHARE = 1 / 3

APPLICATIONS = "/Applications"
PROFILE = "Prax terminals bench"


# ----------------------------------------------------------------------------------------------------------------------
# Counters


class Usage(ctypes.Structure):
    """rusage_info_v6, as <sys/resource.h> declares it."""
    _fields_ = [("uuid", ctypes.c_uint8 * 16)] + [(name, ctypes.c_uint64) for name in (
        "user_time", "system_time", "pkg_idle_wkups", "interrupt_wkups", "pageins", "wired_size", "resident_size",
        "phys_footprint", "proc_start_abstime", "proc_exit_abstime", "child_user_time", "child_system_time",
        "child_pkg_idle_wkups", "child_interrupt_wkups", "child_pageins", "child_elapsed_abstime", "diskio_bytesread",
        "diskio_byteswritten", "cpu_time_qos_default", "cpu_time_qos_maintenance", "cpu_time_qos_background",
        "cpu_time_qos_utility", "cpu_time_qos_legacy", "cpu_time_qos_user_initiated",
        "cpu_time_qos_user_interactive", "billed_system_time", "serviced_system_time", "logical_writes",
        "lifetime_max_phys_footprint", "instructions", "cycles", "billed_energy", "serviced_energy",
        "interval_max_phys_footprint", "runnable_time", "flags", "user_ptime", "system_ptime", "pinstructions",
        "pcycles", "energy_nj", "penergy_nj", "secure_time_in_system", "secure_ptime_in_system", "neural_footprint",
        "lifetime_max_neural_footprint", "interval_max_neural_footprint", "conclave_footprint", "page_wait_time_mach",
        "page_cache_hits")] + [("reserved", ctypes.c_uint64 * 6)]


RUSAGE_INFO_V6 = 6
LIBPROC = ctypes.CDLL("/usr/lib/libproc.dylib") if sys.platform == "darwin" else None


class Timebase(ctypes.Structure):
    _fields_ = [("numer", ctypes.c_uint32), ("denom", ctypes.c_uint32)]


def ticks_to_ms():
    """Returns the milliseconds in one unit of the absolute clock, which rusage's times count."""
    timebase = Timebase()
    ctypes.CDLL(None).mach_timebase_info(ctypes.byref(timebase))
    return timebase.numer / timebase.denom / 1e6


def all_pids():
    count = LIBPROC.proc_listallpids(None, 0)
    pids = (ctypes.c_int * (count + 64))()
    count = LIBPROC.proc_listallpids(pids, ctypes.sizeof(pids))
    return [pids[i] for i in range(max(0, count)) if pids[i] > 0]


def executable(pid):
    """Returns the path of the executable of process `pid`, or "" if it cannot be read."""
    path = ctypes.create_string_buffer(4096)
    size = LIBPROC.proc_pidpath(pid, path, ctypes.sizeof(path))
    return path.value[:size].decode("utf-8", "replace") if size > 0 else ""


def processes_under(roots):
    """Returns the processes whose executable is one of `roots` or lies in one of them, such as a bundle."""
    prefixes = tuple(root.rstrip("/") + "/" for root in roots)
    out = set()
    for pid in all_pids():
        path = executable(pid)
        if path in roots or path.startswith(prefixes):
            out.add(pid)
    return out


def gpu_times(pids):
    """Returns the nanoseconds that the GPU spent on each of `pids`' Metal clients, summed by process, as the driver
    accumulates them. A process without a client is left out."""
    listing = subprocess.run(["ioreg", "-r", "-c", "AGXDeviceUserClient", "-a", "-d", "1"], capture_output=True)
    if listing.returncode != 0 or not listing.stdout:
        return {}
    out = {}
    for client in plistlib.loads(listing.stdout):
        creator = re.match(r"pid (\d+),", client.get("IOUserClientCreator", ""))
        if creator and int(creator.group(1)) in pids:
            pid = int(creator.group(1))
            out[pid] = out.get(pid, 0) + sum(usage.get("accumulatedGPUTime", 0) for usage in client.get("AppUsage", []))
    return out


def reset_peaks(pids):
    """Restarts the interval over which each process's peak footprint is kept."""
    for pid in pids:
        LIBPROC.proc_reset_footprint_interval(pid)


class Counters:
    """What a terminal's processes had spent at one moment, by process: CPU time in milliseconds, instructions, the
    CPU's energy in millijoules, and GPU time in milliseconds. It also holds each process's physical footprint and its
    peak since the interval's restart, in bytes."""

    def __init__(self, pids, gpu=True):
        self.cpu = {}
        self.instructions = {}
        self.energy = {}
        self.footprint = {}
        self.peak = {}
        scale = ticks_to_ms()
        for pid in pids:
            usage = Usage()
            if LIBPROC.proc_pid_rusage(pid, RUSAGE_INFO_V6, ctypes.byref(usage)) == 0:
                self.cpu[pid] = (usage.user_time + usage.system_time) * scale
                self.instructions[pid] = usage.instructions
                self.energy[pid] = usage.energy_nj / 1e6
                self.footprint[pid] = usage.phys_footprint
                self.peak[pid] = usage.interval_max_phys_footprint
        self.gpu = {pid: value / 1e6 for pid, value in gpu_times(set(pids)).items()} if gpu else {}

    def since(self, base, field):
        """Returns what the processes spent of `field` since `base`, over the processes that both read."""
        mine, theirs = getattr(self, field), getattr(base, field)
        return sum(mine[pid] - theirs.get(pid, 0) for pid in mine if pid in theirs or field == "gpu")

    def cpu_total(self):
        return sum(self.cpu.values())


def wait_idle(pids, limit):
    """Waits until the processes go idle, for at most `limit` seconds. Returns the seconds that it waited, and whether
    they went idle."""
    begun = time.monotonic()
    samples = [(begun, Counters(pids, gpu=False).cpu_total())]
    while samples[-1][0] - begun < limit:
        time.sleep(SAMPLE)
        samples.append((time.monotonic(), Counters(pids, gpu=False).cpu_total()))
        now, cpu = samples[-1]
        older = [sample for sample in samples if sample[0] <= now - IDLE_WINDOW]
        if older and (cpu - older[-1][1]) / ((now - older[-1][0]) * 1000) < IDLE_SHARE:
            return now - begun, True
    return samples[-1][0] - begun, False


def idle_footprint(pids):
    """Returns the least physical footprint of the processes, summed, over MEMORY_WINDOW seconds."""
    least = None
    deadline = time.monotonic() + MEMORY_WINDOW
    while True:
        total = sum(Counters(pids, gpu=False).footprint.values())
        least = total if least is None else min(least, total)
        if time.monotonic() >= deadline:
            return least
        time.sleep(0.1)


FOOTPRINT_UNITS = {"B": 1, "KB": 1 << 10, "MB": 1 << 20, "GB": 1 << 30}
FOOTPRINT_ROW = re.compile(r"^\s*([\d.]+) (B|KB|MB|GB)\s+[\d.]+ (?:B|KB|MB|GB)\s+[\d.]+ (?:B|KB|MB|GB)\s+\d+\s+(.+?)\s*$")


def footprint_categories(text):
    """Returns the dirty bytes of each category in the report of footprint(1), by category."""
    out = {}
    for line in text.splitlines():
        match = FOOTPRINT_ROW.match(line)
        if match and match.group(3) != "TOTAL":
            size = int(float(match.group(1)) * FOOTPRINT_UNITS[match.group(2)])
            out[match.group(3)] = out.get(match.group(3), 0) + size
    return out


# The most reads of footprint(1) that idle_categories makes, and how close to the idle footprint a read's total must
# come for the reads to stop early.
CATEGORY_READS = 8
CATEGORY_MARGIN = 1.05


def idle_categories(pids, idle):
    """Returns the dirty bytes of the processes' footprint by footprint(1)'s categories, summed over the processes, or
    an empty dictionary if footprint cannot read them. A terminal's idle footprint can swing as a renderer's buffers
    come and go. The reads therefore stop at the first whose total is within CATEGORY_MARGIN of `idle`, the least
    footprint. If no read comes that close, the read with the least total is returned."""
    least = {}
    for _ in range(CATEGORY_READS):
        out = {}
        for pid in pids:
            done = subprocess.run(["footprint", "-p", str(pid)], capture_output=True, text=True)
            for category, size in footprint_categories(done.stdout).items():
                out[category] = out.get(category, 0) + size
        if not least or sum(out.values()) < sum(least.values()):
            least = out
        if not out or sum(out.values()) <= idle * CATEGORY_MARGIN:
            break
        time.sleep(0.25)
    return least


# ----------------------------------------------------------------------------------------------------------------------
# Terminals


class AutomationDenied(Exception):
    pass


class Unavailable(Exception):
    """A terminal that cannot be measured now, and why."""


def osascript(script):
    """Runs AppleScript and returns what it printed. Raises AutomationDenied if macOS does not let this process control
    the application."""
    # An application that shows a modal alert answers no event, so each script gives up after a while.
    done = subprocess.run(["osascript", "-e", "with timeout of 20 seconds\n%s\nend timeout" % script],
                          capture_output=True, text=True)
    if done.returncode != 0:
        if "-1743" in done.stderr or "Not authorized" in done.stderr:
            raise AutomationDenied(done.stderr.strip())
        raise RuntimeError("osascript: " + done.stderr.strip())
    return done.stdout.strip()


def quoted(argument):
    """Quotes an argument for a shell."""
    return "'" + argument.replace("'", "'\\''") + "'"


def applescript_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


class Terminal:
    """A terminal application. `bundle` is its application bundle under /Applications, or, for Prax, the build's
    executable. A shared terminal runs every window in one process, and AppleScript opens its windows.

    `launch(command, directory)` starts the terminal with `command` as its program, and keeps what close() needs.
    `directory` holds the session's temporary files, such as a configuration."""

    shared = False

    def __init__(self, name, bundle):
        self.name = name
        self.bundle = bundle

    def installed(self):
        return os.path.exists(self.bundle)

    def processes(self):
        """The terminal's processes: those whose executable is in its bundle, or is one of its helpers."""
        return processes_under([self.bundle])

    def binary(self, name):
        return os.path.join(self.bundle, "Contents", "MacOS", name)

    def version(self):
        plist = os.path.join(self.bundle, "Contents", "Info.plist")
        return subprocess.run(["plutil", "-extract", "CFBundleShortVersionString", "raw", plist], capture_output=True,
                              text=True).stdout.strip()

    def font(self):
        return "%d pt, its own face" % FONT_SIZE

    def scrollback(self):
        """The scrollback that the terminal keeps, as the benchmark set it."""
        return "%d lines" % SCROLLBACK

    def prepare(self):
        """Readies the terminal before its first session. Raises Unavailable if it cannot be measured now."""

    def finish(self):
        """Undoes what prepare() did, once every session is over."""

    def launch(self, command, directory):
        self.process = subprocess.Popen(self.arguments(command, directory), stdin=subprocess.DEVNULL,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)

    def arguments(self, command, directory):
        raise NotImplementedError

    def close(self, pids):
        """Closes what launch() opened, whose processes are `pids`, once the feed has exited."""
        stop(pids | ({self.process.pid} if getattr(self, "process", None) else set()))
        if getattr(self, "process", None):
            try:
                self.process.wait(5)
            except subprocess.TimeoutExpired:
                self.process.kill()
            self.process = None


def alive(pid):
    try:
        os.kill(pid, 0)
    except OSError as error:
        return error.errno == errno.EPERM
    return True


def stop(pids):
    """Waits briefly for processes to exit on their own, as a terminal does once its last window closes, and then
    terminates those that remain."""
    deadline = time.monotonic() + 2
    while any(alive(pid) for pid in pids) and time.monotonic() < deadline:
        time.sleep(0.05)
    for sig, wait in ((signal.SIGTERM, 3), (signal.SIGKILL, 1)):
        remaining = [pid for pid in pids if alive(pid)]
        for pid in remaining:
            try:
                os.kill(pid, sig)
            except OSError:
                pass
        deadline = time.monotonic() + wait
        while any(alive(pid) for pid in remaining) and time.monotonic() < deadline:
            time.sleep(0.05)


class Prax(Terminal):
    def __init__(self, build):
        super().__init__("prax", os.path.join(build, "terminal"))

    def version(self):
        done = subprocess.run(["git", "-C", common.ROOT, "rev-parse", "--short", "HEAD"], capture_output=True,
                              text=True)
        return done.stdout.strip()

    def font(self):
        return "%d px, Noto Sans Mono" % FONT_SIZE

    def arguments(self, command, directory):
        return [self.bundle, "--grid", "%dx%d" % (COLUMNS, ROWS), "--scrollback", str(SCROLLBACK)] + command


# Ghostty limits its scrollback in bytes. The limit is the size of SCROLLBACK rows of COLUMNS cells, at 8 bytes per cell
# and per row, plus a quarter for the other storage of its pages.
GHOSTTY_SCROLLBACK_BYTES = SCROLLBACK * (COLUMNS + 1) * 8 * 5 // 4


class Ghostty(Terminal):
    def scrollback(self):
        return "%d MB, about %d lines" % (GHOSTTY_SCROLLBACK_BYTES // 10**6, SCROLLBACK)

    def arguments(self, command, directory):
        # Ghostty's executable starts a new instance, which reads its options and no configuration file. Launched
        # through `open`, Ghostty does not run the command.
        return [self.binary("ghostty"), "--config-default-files=false",
                "--font-size=%d" % FONT_SIZE, "--window-width=%d" % COLUMNS, "--window-height=%d" % ROWS,
                "--scrollback-limit=%d" % GHOSTTY_SCROLLBACK_BYTES, "--quit-after-last-window-closed=true",
                "--confirm-close-surface=false", "--window-save-state=never", "--auto-update=off",
                "--shell-integration=none", "-e"] + command


class Kitty(Terminal):
    def arguments(self, command, directory):
        options = {
            "font_size": FONT_SIZE, "initial_window_width": "%dc" % COLUMNS, "initial_window_height": "%dc" % ROWS,
            "remember_window_size": "no", "scrollback_lines": SCROLLBACK, "macos_quit_when_last_window_closed": "yes",
            "confirm_os_window_close": 0, "update_check_interval": 0, "shell_integration": "disabled",
        }
        return [self.binary("kitty"), "--config", "NONE"] + [part for key, value in options.items()
                                                             for part in ("-o", "%s=%s" % (key, value))] + command


class WezTerm(Terminal):
    def version(self):
        # WezTerm's bundle has no version of its own: its executable names the build.
        out = subprocess.run([self.binary("wezterm"), "--version"], capture_output=True, text=True).stdout.split()
        return out[-1] if out else ""

    def arguments(self, command, directory):
        config = os.path.join(directory, "wezterm.lua")
        with open(config, "w", encoding="utf-8") as f:
            f.write("return {\n  font_size = %d.0,\n  initial_cols = %d,\n  initial_rows = %d,\n"
                    "  scrollback_lines = %d,\n  window_close_confirmation = 'NeverPrompt',\n"
                    "  exit_behavior = 'Close',\n  check_for_updates = false,\n}\n"
                    % (FONT_SIZE, COLUMNS, ROWS, SCROLLBACK))
        return [self.binary("wezterm"), "--config-file", config, "start", "--always-new-process", "--"] + command


class Alacritty(Terminal):
    def arguments(self, command, directory):
        config = os.path.join(directory, "alacritty.toml")
        with open(config, "w", encoding="utf-8") as f:
            f.write("[window.dimensions]\ncolumns = %d\nlines = %d\n\n[font]\nsize = %d\n\n[scrolling]\nhistory = %d\n"
                    % (COLUMNS, ROWS, FONT_SIZE, SCROLLBACK))
        return [self.binary("alacritty"), "--config-file", config, "-e"] + command


class Scripted(Terminal):
    """A terminal whose windows AppleScript opens, all in one process. The benchmark measures it only if it is not
    running, so that the benchmark's window is its only one. A session launches it, closes the windows that it opened
    at its launch, and quits it at the end. `application` is its bundle identifier."""

    shared = True
    application = ""

    def prepare(self):
        if self.processes():
            raise Unavailable("it is running, and its other windows would be measured with the benchmark's; quit it, "
                              "and the benchmark launches it")

    def tell(self, script):
        return osascript("tell application id %s\n%s\nend tell" % (applescript_string(self.application), script))

    def launch(self, command, directory):
        # The launch ignores the windows that the application saved at its last quit.
        subprocess.run(["open", "-a", self.bundle, "--args", "-ApplePersistenceIgnoreState", "YES"], check=True,
                       capture_output=True)
        deadline = time.monotonic() + LAUNCH_TIMEOUT
        while True:
            try:
                self.tell("count windows")
                break
            except RuntimeError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        # The application opens its first window shortly after it answers events. That window is closed once the
        # benchmark's window is open.
        deadline = time.monotonic() + 3
        while self.tell("count windows") == "0" and time.monotonic() < deadline:
            time.sleep(0.1)
        self.window = self.open_window(command)
        try:
            self.tell("close (every window whose id is not %s)" % self.window)
        except RuntimeError:
            pass

    def open_window(self, command):
        """Opens a window that runs `command`, and returns its id."""
        raise NotImplementedError

    def close(self, pids):
        try:
            self.tell("quit")
        except (AutomationDenied, RuntimeError):
            pass
        stop(pids)


class TerminalApp(Scripted):
    application = "com.apple.Terminal"

    def font(self):
        return "the default profile's"

    def scrollback(self):
        # Terminal sets a window's scrollback only through its profile, which is the user's.
        out = subprocess.run(["defaults", "export", "com.apple.Terminal", "-"], capture_output=True).stdout
        try:
            settings = plistlib.loads(out)
            profile = settings["Window Settings"][settings["Default Window Settings"]]
        except (plistlib.InvalidFileException, KeyError, ValueError):
            return "the default profile's"
        if profile.get("ShouldLimitScrollback"):
            return "%d lines, the default profile's" % profile.get("ScrollbackLines", 10000)
        return "unlimited, the default profile's"

    def open_window(self, command):
        # The window's shell replaces itself with the feed, so the window's process is the feed. Terminal sets a
        # window's font only through its profile, which is the user's, so the font is the default profile's.
        script = " ".join(quoted(part) for part in command)
        return self.tell('set t to do script %s\nset number of columns of t to %d\nset number of rows of t to %d\n'
                         'return id of front window' % (applescript_string("exec " + script), COLUMNS, ROWS))


class ITerm(Scripted):
    application = "com.googlecode.iterm2"

    def profile_path(self):
        return os.path.expanduser("~/Library/Application Support/iTerm2/DynamicProfiles/prax-terminals-bench.json")

    def font(self):
        return "%d pt, Menlo" % FONT_SIZE

    def processes(self):
        # iTerm2's sessions run under a server of its own, which it installs outside its bundle and which relays each
        # session's output to the application.
        return processes_under([self.bundle, os.path.expanduser("~/Library/Application Support/iTerm2")])

    def prepare(self):
        super().prepare()
        # A dynamic profile, which iTerm2 loads from its folder and forgets once the file is removed, sets the grid,
        # the font and the scrollback without the user's profiles.
        path = self.profile_path()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        profile = {"Name": PROFILE, "Guid": "prax-terminals-bench", "Columns": COLUMNS, "Rows": ROWS,
                   "Normal Font": "Menlo-Regular %d" % FONT_SIZE, "Scrollback Lines": SCROLLBACK,
                   "Unlimited Scrollback": False, "Close Sessions On End": True, "Prompt Before Closing 2": 0,
                   "Silence Bell": True}
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"Profiles": [profile]}, f)

    def finish(self):
        try:
            os.remove(self.profile_path())
        except OSError:
            pass

    def open_window(self, command):
        script = " ".join(quoted(part) for part in command)
        return self.tell("set w to (create window with profile %s command %s)\nreturn id of w"
                         % (applescript_string(PROFILE), applescript_string(script)))


def terminals(build):
    """The terminals that the benchmark knows, Prax first, by name."""
    out = [Prax(build), Ghostty("ghostty", os.path.join(APPLICATIONS, "Ghostty.app")),
           Kitty("kitty", os.path.join(APPLICATIONS, "kitty.app")),
           WezTerm("wezterm", os.path.join(APPLICATIONS, "WezTerm.app")),
           Alacritty("alacritty", os.path.join(APPLICATIONS, "Alacritty.app")),
           ITerm("iterm2", os.path.join(APPLICATIONS, "iTerm.app")),
           TerminalApp("terminal.app", "/System/Applications/Utilities/Terminal.app")]
    return {terminal.name: terminal for terminal in out}


# ----------------------------------------------------------------------------------------------------------------------
# Sessions


class RunFailed(Exception):
    pass


def read_line(fd, buffer, timeout, what):
    """Returns the next line that the feed reports on `fd`, reading into `buffer`, as words. Raises RunFailed after
    `timeout` seconds, or with the feed's error."""
    deadline = time.monotonic() + timeout
    while b"\n" not in buffer:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RunFailed("no %s within %ds" % (what, timeout))
        readable, _, _ = select.select([fd], [], [], remaining)
        if readable:
            buffer += os.read(fd, 4096)
    line, _, rest = bytes(buffer).partition(b"\n")
    buffer[:] = rest
    text = line.decode("utf-8", "replace")
    if text.startswith("error "):
        raise RunFailed("the feed reported: " + text[len("error "):])
    return text.split()


def open_writer(path, timeout):
    """Opens the FIFO at `path` for writing once its reader has opened it."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            return os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as error:
            if error.errno != errno.ENXIO or time.monotonic() > deadline:
                raise RunFailed("the feed did not open its commands")
            time.sleep(0.01)


class Session:
    """A terminal's window running the feed, from open() to close(). `pids` are the terminal's processes, without the
    feed and without any that ran before the launch."""

    def __init__(self, terminal, feed, scratch):
        self.terminal = terminal
        self.feed = feed
        self.directory = tempfile.mkdtemp(prefix="session-", dir=scratch)
        self.report = os.path.join(self.directory, "report")
        self.commands = os.path.join(self.directory, "commands")
        os.mkfifo(self.report)
        os.mkfifo(self.commands)
        # The runner holds its own writer on the report FIFO, so that a read sees no end of file before the feed opens
        # it.
        self.reader = os.open(self.report, os.O_RDONLY | os.O_NONBLOCK)
        self.keeper = os.open(self.report, os.O_WRONLY)
        self.writer = None
        self.buffer = bytearray()
        self.before = set()
        self.pids = set()
        self.feed_pid = None
        # The share of the terminal's window that other windows covered once it was brought to the front, or None if
        # it had no window on screen.
        self.covered = None

    def open(self):
        """Launches the terminal and waits for the feed. Returns the seconds from the launch to the feed's readiness."""
        self.before = self.terminal.processes()
        begun = time.monotonic()
        try:
            self.terminal.launch([self.feed, self.directory], self.directory)
        except RuntimeError as error:
            raise RunFailed(str(error))
        ready = read_line(self.reader, self.buffer, LAUNCH_TIMEOUT, "ready from the feed")
        launched = time.monotonic() - begun
        self.feed_pid = int(ready[1])
        self.writer = open_writer(self.commands, 5)
        self.refresh()
        if not self.pids:
            raise RunFailed("no process of %s found" % self.terminal.bundle)
        self.covered = windows.bring_to_front(self.pids, FRONT_TIMEOUT)
        return launched

    def refresh(self):
        """Finds the terminal's processes again, which may have started helpers since the launch."""
        self.pids = self.terminal.processes() - self.before - {self.feed_pid}

    def ask(self, command, answer, timeout):
        """Sends the feed `command` and returns the words of its report, which starts with `answer`."""
        os.write(self.writer, (command + "\n").encode("utf-8"))
        words = read_line(self.reader, self.buffer, timeout, answer)
        if not words or words[0] != answer:
            raise RunFailed("the feed reported %s to %s" % (" ".join(words), command))
        return words

    def close(self):
        """Ends the feed, which closes its window, and closes the terminal."""
        if self.writer is not None:
            os.close(self.writer)
            self.writer = None
        deadline = time.monotonic() + 2
        while self.feed_pid is not None and alive(self.feed_pid) and time.monotonic() < deadline:
            time.sleep(0.02)
        # A session that failed before it found the terminal's processes closes whatever the launch started.
        pids = self.pids or (self.terminal.processes() - self.before)
        self.terminal.close(pids)
        os.close(self.reader)
        os.close(self.keeper)
        shutil.rmtree(self.directory, ignore_errors=True)


def measure(session, share, timeout, footprint=False):
    """Runs `share` of the loaded workload once from an empty screen and scrollback, with a timeout of `timeout`
    seconds, and returns its measures. With `footprint`, the run's footprint is read as idle_footprint reads it.
    Otherwise it is read once the terminal is idle. The window is first brought to the front if another covers it. The
    run's "covered" is the largest share of the window that other windows covered until the terminal was idle, or None
    if the window was off screen at some time."""
    session.ask("clear", "cleared", ANSWER_TIMEOUT)
    session.refresh()
    pids = session.pids
    windows.bring_to_front(pids, FRONT_TIMEOUT)
    wait_idle(pids, TAIL_LIMIT)
    reset_peaks(pids)
    watch = windows.Watch(pids).start()
    try:
        base = Counters(pids)
        done = session.ask("run %g %d" % (share, timeout * 1000), "done", timeout + ANSWER_TIMEOUT)
        tail, idle = wait_idle(pids, TAIL_LIMIT)
        end = Counters(pids)
    finally:
        covered = watch.stop()
    return {"wall": float(done[1]), "grid": "%sx%s" % (done[2], done[3]), "bytes": int(done[4]),
            "complete": done[5] == "1", "cpu": end.since(base, "cpu"), "instructions": end.since(base, "instructions"),
            "energy": end.since(base, "energy"), "gpu": end.since(base, "gpu") if end.gpu or base.gpu else None,
            "peak": sum(end.peak.values()),
            "footprint": idle_footprint(pids) if footprint else sum(end.footprint.values()), "tail": tail * 1000,
            "idle": idle, "processes": len(pids), "covered": covered}


class Workload:
    """A workload that the feed lists: its kind (fill, throughput or paced), its amount, and its name."""

    def __init__(self, kind, amount, name):
        self.kind = kind
        self.amount = amount
        self.name = name

    def timeout(self):
        return RUN_TIMEOUT + (self.amount if self.kind == "paced" else 0)


class Result:
    """A terminal's measures of one workload: its measured runs, and the timeout that a run passed or the error that
    stopped it. An invalid run carries the reason in its "invalid" entry."""

    def __init__(self, workload):
        self.workload = workload
        self.runs = []
        self.timeout = None
        self.error = None

    def valid(self):
        return [run for run in self.runs if not run.get("invalid")]

    def invalid(self):
        """Returns a note of why runs are invalid, or None if every run is valid."""
        reasons = [run["invalid"] for run in self.runs if run.get("invalid")]
        if not reasons:
            return None
        return "%d of %d runs invalid: %s" % (len(reasons), len(self.runs), "; ".join(sorted(set(reasons))))


def in_front(covered):
    """Returns true if a window counts as in front. `covered` is the share of the window that other windows cover, or
    None if the window is off screen."""
    return covered is not None and covered <= COVER_LIMIT


def invalidate(runs):
    """Marks each run that shows that the terminal did not draw what it was sent. The reason goes in the run's
    "invalid" entry."""
    most = max((run["cpu"] for run in runs), default=0)
    for run in runs:
        reasons = []
        if run["covered"] is None:
            reasons.append("its window was not on screen")
        elif not in_front(run["covered"]):
            reasons.append("%.0f%% of its window was covered" % (run["covered"] * 100))
        if run["gpu"] is not None and run["gpu"] <= 0:
            reasons.append("no GPU time")
        if run["cpu"] < SUSPECT_CPU_SHARE * most:
            reasons.append("CPU time %s, against %s in another run" % (common.duration(run["cpu"]),
                                                                       common.duration(most)))
        if reasons:
            run["invalid"] = ", ".join(reasons)


def bench_workload(session, workload, runs):
    """Runs `workload` in the session: the warm-up, then `runs` measured runs, or one run for the fill. A run that
    passes its timeout ends the workload."""
    result = Result(workload)
    session.ask("load " + workload.name, "loaded", 120)
    if workload.kind != "fill":
        warm = measure(session, WARM_SHARE, workload.timeout())
        if not warm["complete"]:
            result.timeout = workload.timeout()
            return result
    for _ in range(1 if workload.kind == "fill" else runs):
        # Another application may come to the front during a run, as when the user clicks it. The run is then measured
        # again, a few times at most.
        for _ in range(1 + COVER_RETRIES):
            measured = measure(session, 1, workload.timeout(), footprint=workload.kind == "fill")
            if not measured["complete"]:
                result.timeout = workload.timeout()
                result.runs = []
                return result
            if in_front(measured["covered"]):
                break
        result.runs.append(measured)
    invalidate(result.runs)
    return result


class Record:
    """What a terminal's sessions measured: its launch, its idle footprint, and its results by workload."""

    def __init__(self, terminal):
        self.terminal = terminal
        self.launch = None
        self.settled = None
        self.idle = None
        self.categories = {}            # the idle footprint's dirty bytes, by footprint(1)'s category
        self.processes = None
        self.covered = None
        self.results = {}
        self.error = None


def bench_terminal(record, feed, workloads, runs, scratch, stream):
    """Measures the workloads in a session of the record's terminal, and launches a new session after a failure.
    Streams each result into `stream` and the record as it finishes."""
    terminal = record.terminal
    pending = list(workloads)
    while pending:
        session = Session(terminal, feed, scratch)
        try:
            try:
                launched = session.open()
            except AutomationDenied as error:
                record.error = ("macOS does not let this process control it with AppleScript; allow it in System "
                                "Settings > Privacy & Security > Automation, and run again (%s)" % error)
                return
            except RunFailed as error:
                record.error = "cannot be launched: %s" % error
                return
            settle, idle = wait_idle(session.pids, SETTLE_LIMIT)
            if record.idle is None:
                record.launch = launched
                record.settled = settle if idle else None
                record.idle = idle_footprint(session.pids)
                record.categories = idle_categories(session.pids, record.idle)
                record.processes = len(session.pids)
                record.covered = session.covered
                stream.terminal(record)
            while pending:
                workload = pending.pop(0)
                try:
                    result = bench_workload(session, workload, runs)
                except RunFailed as error:
                    result = Result(workload)
                    result.error = str(error)
                record.results[workload.name] = result
                stream.result(record, result)
                if result.error:
                    # The feed may be gone or still writing: the next workload starts in a session of its own.
                    break
        finally:
            session.close()


# ----------------------------------------------------------------------------------------------------------------------
# Reporting


def median(values):
    ordered = sorted(values)
    middle = len(ordered) // 2
    return ordered[middle] if len(ordered) % 2 else (ordered[middle - 1] + ordered[middle]) / 2


def summary(values):
    """Returns the median of `values`, their spread as a share of it, and their range, or None if there are none."""
    values = [value for value in values if value is not None]
    if not values:
        return None
    middle = median(values)
    spread = (max(values) - min(values)) / middle if middle > 0 else 0
    return middle, spread, min(values), max(values)


def geomean(values):
    return math.exp(sum(math.log(v) for v in values) / len(values)) if values else None


def ratio_cell(mine, theirs, higher_is_better=False):
    """Returns the other terminal's cost as a multiple of Prax's, from the summaries of their runs. The result is marked
    with ? if the ranges of the runs overlap. For a measure in which higher is better, the cost is the inverse."""
    if mine is None or theirs is None or mine[0] <= 0 or theirs[0] <= 0:
        return "-"
    overlap = mine[2] <= theirs[3] and theirs[2] <= mine[3]
    ratio = mine[0] / theirs[0] if higher_is_better else theirs[0] / mine[0]
    return "%.2fx%s" % (ratio, "?" if overlap else " ")


def megabytes(size):
    return "%.0f MB" % (size / 1e6) if size >= 1e7 else "%.1f MB" % (size / 1e6)


def energy(millijoules):
    return "%.3gmJ" % millijoules if millijoules < 1000 else "%.3gJ" % (millijoules / 1000)


def per_second(show):
    return lambda value: show(value) + "/s"


def throughput(run):
    return run["bytes"] / (1 << 20) / (run["wall"] / 1000)


# The measures that the tables show. Each has a name, a title, the value of a run for a throughput workload and for a
# paced one (None if the measure does not apply), how to show it for each, and whether higher is better.
MEASURES = [
    ("rate", "throughput: MiB per second of the workload, to the answer to DSR 6; paced: the seconds that its frames "
     "took", throughput, lambda run: run["wall"] / 1000, lambda value: "%.0f MiB/s" % value if value >= 10 else
     "%.2g MiB/s" % value, lambda value: "%.2fs" % value, True),
    ("cpu", "CPU time of the terminal's processes, user and system, until idle; paced: per second of frames",
     lambda run: run["cpu"], lambda run: run["cpu"] / (run["wall"] / 1000), common.duration,
     per_second(common.duration), False),
    ("instructions", "instructions that the terminal's processes retired until idle; paced: per second of frames",
     lambda run: run["instructions"], lambda run: run["instructions"] / (run["wall"] / 1000), common.scaled,
     per_second(common.scaled), False),
    ("energy", "the CPU's energy for the terminal's processes until idle; paced: its power", lambda run: run["energy"],
     lambda run: run["energy"] / (run["wall"] / 1000), energy, lambda mw: "%.3gmW" % mw if mw < 1000 else
     "%.3gW" % (mw / 1000), False),
    ("gpu", "GPU time of the terminal's Metal clients until idle, - for a terminal without one; paced: per second of "
     "frames",
     lambda run: run["gpu"], lambda run: None if run["gpu"] is None else run["gpu"] / (run["wall"] / 1000),
     common.duration, per_second(common.duration), False),
    ("peak", "peak physical footprint of the terminal's processes during the workload", lambda run: run["peak"],
     lambda run: run["peak"], megabytes, megabytes, False),
]


def measure_summary(result, measure):
    """Returns the summary of one measure over a result's valid runs, and how to show it."""
    _, _, of_throughput, of_paced, show_throughput, show_paced, _ = measure
    paced = result.workload.kind == "paced"
    of = of_paced if paced else of_throughput
    return summary(of(run) for run in result.valid()), show_paced if paced else show_throughput


def is_invalid(result):
    """Returns true if a result has runs and none of them is valid."""
    return result is not None and bool(result.runs) and not result.valid()


def cell(result, measure):
    """Returns the cell of one measure of a result: the median and spread of its valid runs, a timeout, a failure, or ?
    if none of its runs is valid."""
    if result is None:
        return "-"
    if result.error:
        return "failed"
    if result.timeout is not None:
        return "> %d s" % result.timeout if measure[0] == "rate" else "-"
    if is_invalid(result):
        return "?"
    value, show = measure_summary(result, measure)
    if value is None:
        return "-"
    return show(value[0]) + (" ±%.0f%%" % (value[1] * 100) if len(result.valid()) > 1 else "")


class Stream:
    """Prints each terminal's results as they finish, a row to a workload, in columns of a fixed width."""

    WIDTHS = [16, 18, 15, 14, 14, 15, 9]

    def __init__(self, out=sys.stdout):
        self.out = out

    def line(self, cells):
        print("  ".join(c.ljust(w) if i == 0 else c.rjust(w) for i, (c, w) in enumerate(zip(cells, self.WIDTHS))),
              file=self.out, flush=True)

    def terminal(self, record):
        settled = "idle after %.1fs" % record.settled if record.settled is not None else "not idle after %.0fs" % (
            SETTLE_LIMIT)
        print("\n%s %s: ready %.1fs after its launch, %s, %s, processes: %d, %s at idle; scrollback %s"
              % (record.terminal.name, record.terminal.version(), record.launch, settled, window(record.covered),
                 record.processes, megabytes(record.idle), record.terminal.scrollback()), file=self.out, flush=True)
        self.line(["workload"] + [measure[0] for measure in MEASURES])

    def result(self, record, result):
        cells = [result.workload.name] + [cell(result, measure) for measure in MEASURES]
        self.line(cells)
        if result.error:
            print("  %s failed: %s" % (result.workload.name, result.error), file=self.out, flush=True)
        if result.invalid():
            print("  %s: %s" % (result.workload.name, result.invalid()), file=self.out, flush=True)
        if result.workload.kind == "fill" and result.runs:
            print("  after the fill: %s, %s a line over the idle footprint" % (
                megabytes(result.runs[0]["footprint"]), bytes_per_line(record, result)), file=self.out, flush=True)


def window(covered):
    """Describes the state of a terminal's window from the share of it that other windows cover."""
    if covered is None:
        return "no window on screen"
    return "window in front" if in_front(covered) else "window %.0f%% covered" % (covered * 100)


def bytes_per_line(record, fill):
    """Returns the bytes that each line of the fill's scrollback added to the terminal's idle footprint, as text."""
    if not fill.runs or record.idle is None:
        return "-"
    return "%.0f B" % ((fill.runs[0]["footprint"] - record.idle) / fill.workload.amount)


def report(records, workloads):
    """Prints a table of each measure by workload, with each terminal's median and spread and its multiple of Prax's,
    then the geometric means of the multiples, and the terminals' memory."""
    names = [record.terminal.name for record in records]
    by_name = {record.terminal.name: record for record in records}
    others = [name for name in names if name != "prax"] if "prax" in names else []
    means = {}
    for measure in MEASURES:
        field, title, *_, higher_is_better = measure
        header = ["workload"] + [c for name in names for c in ([name, "x"] if name in others else [name])]
        lines = []
        for workload in workloads:
            if workload.kind == "fill" and field != "rate":
                continue
            results = {name: by_name[name].results.get(workload.name) for name in names}
            stats = {name: measure_summary(result, measure)[0] if result and result.valid() else None
                     for name, result in results.items()}
            line = [workload.name]
            for name in names:
                line.append(cell(results[name], measure))
                if name not in others:
                    continue
                if is_invalid(results["prax"]) or is_invalid(results[name]):
                    line.append("?")
                    continue
                line.append(ratio_cell(stats.get("prax"), stats[name], higher_is_better))
                # A paced workload's seconds show whether the terminal kept up, and are not a rate to average.
                paced = workload.kind == "paced" and field == "rate"
                if not paced and stats.get("prax") and stats[name] and stats["prax"][0] > 0 and stats[name][0] > 0:
                    ratio = stats["prax"][0] / stats[name][0] if higher_is_better else stats[name][0] / stats["prax"][0]
                    means.setdefault((field, name), []).append(ratio)
            lines.append(line)
        if all(c == "-" for line in lines for c in line[1:]):
            print("\n%s: not measured" % title)
            continue
        print("\n%s" % title)
        print(common.text_table(header, lines))
    if others:
        print("\nsummary: the geometric mean of each terminal's multiples of prax's, over the workloads that both have "
              "valid runs of, which the parentheses count")
        rows = [[measure[0]] + [mean_cell(means.get((measure[0], name))) for name in others] for measure in MEASURES]
        print(common.text_table(["measure"] + others, rows))
    fill = next((workload for workload in workloads if workload.kind == "fill"), None)
    print("\nmemory: physical footprint at idle after the launch, after the scrollback fill of %s lines, and the bytes "
          "that each line added" % ("%d" % fill.amount if fill else "no"))
    rows = []
    for record in records:
        result = record.results.get(fill.name) if fill else None
        after = megabytes(result.runs[0]["footprint"]) if result and result.runs else "-"
        rows.append([record.terminal.name, record.terminal.scrollback(),
                     megabytes(record.idle) if record.idle is not None else "-", after,
                     bytes_per_line(record, result) if result else "-"])
    print(common.text_table(["terminal", "scrollback", "idle", "after fill", "a line"], rows))
    categories_report(records)


# The least dirty memory that a category needs to get a row of its own in the report of the idle footprint's
# categories.
CATEGORY_LEAST = 1 << 20


def categories_report(records):
    """Prints the idle footprint's dirty memory by footprint(1)'s categories, the largest first, with the categories
    smaller than CATEGORY_LEAST in every terminal summed into one row."""
    measured = [record for record in records if record.categories]
    if not measured:
        return
    largest = {}
    for record in measured:
        for category, size in record.categories.items():
            largest[category] = max(largest.get(category, 0), size)
    listed = sorted((category for category, size in largest.items() if size >= CATEGORY_LEAST),
                    key=lambda category: -largest[category])
    rows = [[category] + [megabytes(record.categories.get(category, 0)) for record in measured] for category in listed]
    rows.append(["the rest"] + [megabytes(sum(size for category, size in record.categories.items()
                                              if category not in listed)) for record in measured])
    print("\nidle footprint by footprint(1)'s categories: dirty memory, read after the idle footprint, as close to it as "
          "the reads came")
    print(common.text_table(["category"] + [record.terminal.name for record in measured], rows))


def mean_cell(ratios):
    return "%.2fx (%d)" % (geomean(ratios), len(ratios)) if ratios else "-"


LEGEND = """Each cell is the median of the measured runs and their spread, (max - min) / median. x: a terminal's cost over
prax's; above 1x, prax is faster or cheaper, and ? where the runs' ranges overlap. A cell of ? alone has no valid
run: its terminal did not draw what it was sent, and the notes say why."""


def main(names, workload_names, runs, build, feed):
    if sys.platform != "darwin":
        print("terminals: the terminals benchmark runs on macOS only; skipped")
        return 0
    started = time.monotonic()
    known = terminals(build)
    unknown = [name for name in names if name not in known]
    if unknown:
        sys.exit("no terminal named %s; one of: %s" % (", ".join(unknown), ", ".join(known)))
    chosen = []
    for name, terminal in known.items():
        if names and name not in names:
            continue
        if not terminal.installed():
            print("%s: not installed (%s); skipped" % (name, terminal.bundle or "no build"))
            continue
        try:
            terminal.prepare()
        except Unavailable as error:
            print("%s: not measured: %s" % (name, error))
            continue
        chosen.append(terminal)
    if not chosen:
        sys.exit("no terminal to run")
    listed = [Workload(kind, int(amount), name) for kind, amount, name in
              (line.split("\t") for line in subprocess.run([feed, "--list"], capture_output=True, text=True,
                                                           check=True).stdout.splitlines())]
    workloads = [workload for workload in listed
                 if not workload_names or any(workload.name.startswith(prefix) for prefix in workload_names)]
    if not workloads:
        sys.exit("no workload starts with %s; one of: %s" % (", ".join(workload_names),
                                                             ", ".join(workload.name for workload in listed)))
    print(common.power() + "; %dx%d cells asked for, %d lines of scrollback; workloads:" % (COLUMNS, ROWS, SCROLLBACK))
    for workload in workloads:
        amount = {"fill": "%d lines of %d columns, once" % (workload.amount, COLUMNS),
                  "throughput": "%d MiB" % (workload.amount >> 20),
                  "paced": "%d s of frames at 60 per second" % workload.amount}[workload.kind]
        print("  %s: %s" % (workload.name, amount))
    print("Each workload but the fill runs once on %d%% of it to warm up, and then %d times; a run stops after %d s, "
          "past a paced workload's frames" % (WARM_SHARE * 100, runs, RUN_TIMEOUT))
    print(LEGEND)
    records = []
    scratch = tempfile.mkdtemp(prefix="prax-terminals-")
    stream = Stream()
    stopped = False
    # A stop by signal still closes the open session, removes the temporary configuration, and reports what finished.
    signal.signal(signal.SIGTERM, lambda number, frame: sys.exit(1))
    try:
        for terminal in chosen:
            records.append(Record(terminal))
            bench_terminal(records[-1], feed, workloads, runs, scratch, stream)
            if records[-1].error:
                print("\n%s: not measured: %s" % (terminal.name, records[-1].error), flush=True)
    except (KeyboardInterrupt, SystemExit):
        stopped = True
    finally:
        for terminal in chosen:
            terminal.finish()
        shutil.rmtree(scratch, ignore_errors=True)
    failed = [r for r in records if r.error]
    records = [r for r in records if r.idle is not None]
    if records:
        print("\n" + common.text_table(["terminal", "version", "grid", "font", "processes"], [
            [r.terminal.name, r.terminal.version(), ", ".join(sorted({run["grid"] for result in r.results.values()
                                                                       for run in result.runs})) or "-",
             r.terminal.font(), str(r.processes)] for r in records]))
        report(records, workloads)
    notes = ["%s, %s: %s" % (r.terminal.name, name, result.error) for r in records for name, result in r.results.items()
             if result.error]
    if notes:
        print("\nfailed:\n" + "\n".join("  " + note for note in notes))
    invalid = ["%s, %s: %s" % (r.terminal.name, name, result.invalid()) for r in records
               for name, result in r.results.items() if result.invalid()]
    if invalid:
        print("\ninvalid runs, left out of the tables:\n" + "\n".join("  " + note for note in invalid))
    print("\n%s in %.0f s" % ("stopped" if stopped else "done", time.monotonic() - started))
    if stopped:
        return 1
    # A failure of another terminal is a measure of that terminal. A failure of Prax is a failure of the run.
    return 1 if any(r.terminal.name == "prax" and (r.error or any(x.error for x in r.results.values()))
                    for r in records + failed) else 0

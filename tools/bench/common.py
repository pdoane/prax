"""Code shared by the benchmark runners and reports: paths, running a harness, and printing tables, counts and
times."""

import json
import os
import subprocess
import sys

import tools

ROOT = tools.ROOT
HERE = os.path.dirname(os.path.abspath(__file__))
PERF = os.path.join(ROOT, "perf")

# The default directory of the harnesses that a run measures: the release build.
RELEASE = os.path.join(ROOT, "build-release")

# The real pages that the benchmarks read. They change only when `./dev.sh pages roll` or `./dev.sh pages refresh`
# captures the current pages.
PAGES = os.path.join(ROOT, "third_party", "pages")

# True if any harness that the run started failed. The runner reports this in its exit status after it finishes
# printing, and records nothing.
failed = False


def binary(directory, name):
    """Returns the path of the harness `name` under `directory`. The harness must exist."""
    path = os.path.join(directory, name + (".exe" if os.name == "nt" else ""))
    if not os.path.exists(path):
        sys.exit("no %s under %s; run ./dev.sh bench" % (name, directory))
    return path


def reported(done, what):
    """Returns what a harness printed, whatever its exit status. A run that stopped partway still measured the rows that
    it printed, and discarding them would hide the numbers behind the failure."""
    global failed
    if done.returncode != 0:
        print("%s exited %d" % (what, done.returncode), file=sys.stderr)
        failed = True
    out = done.stdout.decode("utf-8").strip()
    if not out:
        sys.exit("%s reported nothing" % what)
    return out


def run_json(command, what):
    """Returns the rows that a harness printed, either as one JSON array or as one JSON object per line."""
    out = reported(subprocess.run(command, stdout=subprocess.PIPE), what)
    if out.startswith("["):
        return json.loads(out)
    return [json.loads(line) for line in out.splitlines()]


def peak_working_set(process):
    """Returns the peak working set of `process`, which has exited, in bytes: Windows' measure of the peak resident
    size. Its handle, which Popen holds until the object goes, keeps the process's counters readable."""
    import ctypes
    from ctypes import wintypes

    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

    counters = Counters()
    counters.cb = ctypes.sizeof(Counters)
    query = ctypes.windll.kernel32.K32GetProcessMemoryInfo
    query.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    if not query(wintypes.HANDLE(int(process._handle)), ctypes.byref(counters), counters.cb):
        raise OSError(ctypes.get_last_error(), "cannot read the peak working set of %s" % process.args[0])
    return counters.PeakWorkingSetSize


def run_peak(command):
    """Runs `command` and returns what it printed and the peak resident size of the process, in bytes."""
    process = subprocess.Popen(command, stdout=subprocess.PIPE)
    out = process.stdout.read()
    if os.name == "nt":
        process.wait()
        return subprocess.CompletedProcess(process.args, process.returncode, out), peak_working_set(process)
    _, status, usage = os.wait4(process.pid, 0)
    done = subprocess.CompletedProcess(process.args, os.waitstatus_to_exitcode(status), out)
    # ru_maxrss is bytes on macOS and kilobytes on Linux.
    return done, usage.ru_maxrss if sys.platform == "darwin" else usage.ru_maxrss * 1024


def power():
    """Returns the machine's power source. On battery power the clocks are capped, which changes times and cycles but
    not instructions."""
    try:
        out = subprocess.run(["pmset", "-g", "batt"], capture_output=True, text=True).stdout
    except OSError:
        return "on an unknown power source"
    return "on battery" if "Battery Power" in out else "on mains power"


# ----------------------------------------------------------------------------------------------------------------------
# Corpus


def pages(directory):
    """Returns the pages in a corpus directory, as its manifest lists them, followed by the manifest itself."""
    manifest = os.path.join(directory, "pages.json")
    with open(manifest, encoding="utf-8") as f:
        return [os.path.join(directory, name) for name in json.load(f)] + [manifest]


def captures(directory):
    """Returns the files that hold what each page of a corpus directory loads, as tools/pages/pages.py captures it: each
    page's JSON and the files of the resources that it names, each once."""
    out = []
    for page in pages(directory)[:-1]:
        capture = page[:-len(".html")] + ".json"
        if not os.path.exists(capture):
            continue
        out.append(capture)
        with open(capture, encoding="utf-8") as f:
            files = dict.fromkeys(os.path.join(directory, r["file"]) for r in json.load(f).get("resources", []))
        out += files
    return out


# ----------------------------------------------------------------------------------------------------------------------
# Formatting


def text_table(header, lines):
    """Returns rows of cells laid out in columns, each as wide as its widest cell. The first column is left-aligned, and
    the others are right-aligned."""
    widths = [max(len(line[i]) for line in [header] + lines) for i in range(len(header))]
    return "\n".join("  ".join(cell.ljust(widths[i]) if i == 0 else cell.rjust(widths[i])
                               for i, cell in enumerate(line)).rstrip()
                     for line in [header] + lines)


def scaled(number):
    """Returns a count to three significant figures, with a k, M or G suffix for thousands, millions or billions."""
    for limit, suffix in ((1e9, "G"), (1e6, "M"), (1e3, "k")):
        if abs(number) >= limit:
            value = number / limit
            return ("%.3g" % value) + suffix if abs(value) < 1000 else "%.0f%s" % (value, suffix)
    return "%.0f" % number


def duration(millis):
    """Returns a time given in milliseconds, to three significant figures, in the unit that suits it."""
    for scale, unit in ((1e3, "s"), (1, "ms"), (1e-3, "us"), (1e-6, "ns")):
        # A reading slightly below the unit, such as 0.001 ms read from a clock, is shown in that unit.
        if abs(millis) >= scale * (1 - 1e-9) or unit == "ns":
            value = millis / scale
            return ("%.0f" % value if abs(value) >= 100 else "%.3g" % value) + unit
    return "0"

"""The builds: the default build and the others, configured and built with CMake and Ninja.

The default build is optimized, with debug symbols and without link-time optimization. Everything runs against it
except the benchmarks, which read their numbers from Release. Debug keeps the assertions. The sanitizer builds are
Debug as well. One catches reads past the end of a buffer that the others let through, and the other catches races
between threads.
"""

import json
import os
import shutil
import subprocess
import tempfile

from tools import dev

# The build directories, under the root. PR_BUILD_DIR names the default build's directory in place of build, for a
# second checkout's work beside the first's, or work that must not share a build with another running at the same time.
DEFAULT = os.path.join(dev.ROOT, os.environ.get("PR_BUILD_DIR", "build"))
DEBUG = os.path.join(dev.ROOT, "build-debug")
RELEASE = os.path.join(dev.ROOT, "build-release")
ASAN = os.path.join(dev.ROOT, "build-asan")
TSAN = os.path.join(dev.ROOT, "build-tsan")
# The base of an A/B comparison: its sources, its builds, and its harnesses by commit (ab.py).
AB = os.path.join(dev.ROOT, "build-ab")
# The libraries built and installed as a consumer takes them, and the consumers built against them.
PACKAGE = os.path.join(dev.ROOT, "build-package")

# MSVC has AddressSanitizer alone.
SANITIZE = "-fsanitize=address" if dev.WINDOWS else "-fsanitize=address,undefined -fno-omit-frame-pointer " \
                                                    "-fno-sanitize-recover=all"
THREAD_SANITIZE = None if dev.WINDOWS else "-fsanitize=thread -fno-omit-frame-pointer"

# How many times longer the sanitizer build's harnesses take, for the runners' deadlines.
ASAN_SLOWDOWN = 4


class Mode:
    """A build: its directory, its CMake build type, its compiler and linker flags, and how many times longer its
    harnesses take than the default build's."""

    def __init__(self, name, directory, build_type, flags="", slowdown=1):
        self.name = name
        self.directory = directory
        self.build_type = build_type
        self.flags = flags
        self.slowdown = slowdown

    def environment(self):
        """The environment that the runners read the build from (tools/suite/corpus.py)."""
        return {"PR_BUILD": self.directory, "PR_SLOWDOWN": self.slowdown}


MODES = {
    "default": Mode("default", DEFAULT, "RelWithDebInfo"),
    "debug": Mode("debug", DEBUG, "Debug"),
    "release": Mode("release", RELEASE, "Release"),
    "asan": Mode("asan", ASAN, "Debug", SANITIZE, ASAN_SLOWDOWN),
    "tsan": Mode("tsan", TSAN, "Debug", THREAD_SANITIZE),
}


def mode(name):
    """Returns the build that `name` names: default, debug, release, asan or tsan."""
    found = MODES[name or "default"]
    if found.flags is None:
        dev.fail("MSVC has no thread sanitizer")
    return found


# The file in a build directory that keeps what vcvars added to the environment, since vcvars takes a second to run.
MSVC_ENVIRONMENT = "msvc-environment.json"

# Where under Visual Studio its own CMake and Ninja are, as os.path.normcase spells it.
VS_CMAKE = os.path.normcase(os.path.join("CommonExtensions", "Microsoft", "CMake"))


def msvc_environment(directory):
    """Takes MSVC's environment from the newest Visual Studio's vcvars, unless cl is on the path already. What vcvars
    adds is kept in `directory` and taken from there while the compiler it names is still installed. The compiler and
    its tools go ahead of the path, where Git Bash has a link of its own, and Visual Studio's CMake and Ninja go behind
    it, so that a CMake installed for itself, which is newer, configures the build."""
    if dev.has("cl"):
        return
    kept = os.path.join(directory, MSVC_ENVIRONMENT)
    try:
        with open(kept, encoding="utf-8") as f:
            added = json.load(f)
    except (FileNotFoundError, ValueError):
        added = None
    if added is None or not any(os.path.exists(os.path.join(entry, "cl.exe")) for entry in added["path"]):
        added = run_vcvars()
        os.makedirs(directory, exist_ok=True)
        with open(kept, "w", encoding="utf-8", newline="\n") as f:
            json.dump(added, f)
    os.environ.update(added["variables"])
    bundled = [entry for entry in added["path"] if VS_CMAKE in os.path.normcase(entry)]
    ahead = [entry for entry in added["path"] if entry not in bundled]
    os.environ["PATH"] = os.pathsep.join(ahead + [os.environ.get("PATH", "")] + bundled)
    if not dev.has("cl"):
        dev.fail("no MSVC environment from vcvars")


def run_vcvars():
    """Runs the newest Visual Studio's vcvars and returns what it adds to the environment: the variables it sets, and
    the directories it puts on the path."""
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Microsoft Visual Studio",
                           "Installer", "vswhere.exe")
    if not os.path.exists(vswhere):
        dev.fail("Visual Studio with its C++ tools is required")
    vs = dev.output([vswhere, "-latest", "-products", "*", "-requires",
                     "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"]).strip()
    arch = "arm64" if os.environ.get("PROCESSOR_ARCHITECTURE") == "ARM64" else "64"
    vcvars = os.path.join(vs, "VC", "Auxiliary", "Build", "vcvars%s.bat" % arch)
    # vcvars runs vswhere itself, from the path.
    with tempfile.NamedTemporaryFile("w", suffix=".bat", delete=False) as script:
        script.write('@set "PATH=%%PATH%%;%s"\r\n@call "%s" >nul\r\n@set\r\n' % (os.path.dirname(vswhere), vcvars))
    try:
        listing = subprocess.run(["cmd", "/c", script.name], capture_output=True, text=True).stdout
    finally:
        os.remove(script.name)
    variables = {}
    for line in listing.splitlines():
        name, _, value = line.partition("=")
        if name and name.replace("_", "").isalnum():
            variables[name.upper()] = value
    before = {name.upper(): value for name, value in os.environ.items()}
    original = {os.path.normcase(entry) for entry in before.get("PATH", "").split(os.pathsep)}
    path = [entry for entry in variables.pop("PATH", "").split(os.pathsep)
            if entry and os.path.normcase(entry) not in original]
    return {"variables": {name: value for name, value in variables.items() if before.get(name) != value},
            "path": path}


def cache(directory):
    """Returns the lines of a build directory's CMakeCache.txt, or None if it has none."""
    try:
        with open(os.path.join(directory, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return None


def cached_home(lines):
    """Returns the source directory that a build directory's cache was configured from, or None."""
    for line in lines:
        if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
            return line.partition("=")[2]
    return None


def same_path(cached, path):
    """Returns true if `cached`, a path as CMake writes it, with forward slashes on Windows, names `path`."""
    return cached is not None and os.path.normcase(os.path.realpath(cached)) == os.path.normcase(os.path.realpath(path))


def strict_warnings():
    """The options that make CMake's warnings for project authors and its deprecation warnings errors. CMake 4.4 renamed
    -Werror=dev to -Werror=author, and warns of the old name, which the CMake before it alone knows."""
    version = dev.output(["cmake", "--version"]).split()[2]
    major, minor = (int(part) for part in version.split(".")[:2])
    return ["-Werror=author" if (major, minor) >= (4, 4) else "-Werror=dev", "-Werror=deprecated"]


def openssl_root():
    """Homebrew's OpenSSL, where lib/tls's OpenSSL backend finds it on macOS, or None."""
    if dev.WINDOWS or not dev.has("brew"):
        return None
    return dev.output(["brew", "--prefix", "openssl"], check=False)


def configure(source, directory, build_type, flags="", out=None):
    """Configures the build of the sources in `source` into `directory`, of `build_type`, with the compiler and linker
    flags `flags`, with Ninja. CMake writes to `out`, or to the standard output."""
    if dev.WINDOWS:
        msvc_environment(directory)
    if not dev.has("ninja"):
        dev.fail("ninja is required (brew install ninja, or winget install Ninja-build.Ninja)")
    # lib/tls's OpenSSL backend builds where CMake finds OpenSSL: the system's, or Homebrew's on macOS.
    openssl = openssl_root()

    # CMake fetches what the build needs (cmake/dependencies.cmake) and keeps the archives in third_party/downloads,
    # which every build directory and worktree shares. A development build has the tests, the tools and the examples,
    # with warnings as errors, and every backend that the platform supports. CMake's own warnings are errors too, as
    # the compiler's and the linker's are. Development and the benchmarks allocate with mimalloc, which a library build
    # leaves to its embedder. The options are computed only when CMake configures, which a build that is current skips.
    def options():
        found = ["-DPR_DOWNLOAD_DIR=" + os.path.join(dev.ROOT, "third_party", "downloads"), "-DPR_DEVELOP=ON",
                 "-DPR_EXAMPLES=ON", "-DPR_PLATFORM_MIMALLOC=ON", *strict_warnings()]
        if openssl:
            found.append("-DOPENSSL_ROOT_DIR=" + openssl)
        return found

    # A tree that another generator configured, or that was configured from another checkout's sources, is configured
    # again, with Ninja.
    lines = cache(directory)
    if lines is not None and ("CMAKE_GENERATOR:INTERNAL=Ninja" not in lines
                              or not same_path(cached_home(lines), source)):
        shutil.rmtree(directory)
        lines = None
    if lines is None and dev.WINDOWS:
        # MSVC's default flags, such as /EHsc, stay, and the build's flags add to them. The linker takes none.
        dev.run(["cmake", "-S", source, "-B", directory, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + build_type,
                 "-DCMAKE_C_FLAGS_INIT=" + flags, "-DCMAKE_CXX_FLAGS_INIT=" + flags, *options()], stdout=out,
                stderr=out)
    elif lines is None:
        dev.run(["cmake", "-S", source, "-B", directory, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + build_type,
                 "-DCMAKE_C_FLAGS=" + flags, "-DCMAKE_CXX_FLAGS=" + flags, "-DCMAKE_EXE_LINKER_FLAGS=" + flags,
                 *options()], stdout=out, stderr=out)
    elif ("PR_DEVELOP:BOOL=ON" not in lines or "PR_PLATFORM_MIMALLOC:BOOL=ON" not in lines
          or "CMAKE_BUILD_TYPE:STRING=" + build_type not in lines
          or (openssl and not any(line.startswith("OPENSSL_ROOT_DIR:") and line.endswith("=" + openssl)
                                  for line in lines))):
        dev.run(["cmake", "-S", source, "-B", directory, "-DCMAKE_BUILD_TYPE=" + build_type, *options()], stdout=out,
                stderr=out)


def build(name="default", targets=(), keep_going=False):
    """Configures and builds the build that `name` names, or only `targets` and what they need, and returns it. With
    `keep_going`, Ninja builds everything that does not depend on a failure, so one build reports every error."""
    chosen = mode(name)
    configure(dev.ROOT, chosen.directory, chosen.build_type, chosen.flags)
    command = ["cmake", "--build", chosen.directory]
    for target in targets:
        command += ["--target", target]
    if keep_going:
        command += ["--", "-k", "0"]
    dev.run(command)
    return chosen

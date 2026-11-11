"""The builds: the default build and the others, configured and built with CMake and Ninja.

The default build is optimized, with debug symbols and without link-time optimization. Everything runs against it
except the benchmarks, which read their numbers from Release. Debug keeps the assertions. The sanitizer builds are
Debug as well. One catches reads past the end of a buffer that the others let through, and the other catches races
between threads. The WebAssembly build compiles with Emscripten (emsdk.py), runs on one thread, and runs its tests under
Node.js. The builds for UIKit's platforms, iOS, tvOS and visionOS, are the default cross-compiled with Xcode's SDKs, on
macOS: for a platform's devices, which only builds, or for its simulator, whose programs run on a simulator device. Each
of these cross-compiled builds has a Debug form beside it, which keeps the assertions.

Each build has a lean form, in a directory of its own, for a machine that only tests, as ./dev.sh ci and CI's runners
do: PR_LEAN, which builds the tests and fetches only what they read (cmake/lean.cmake). It compiles through ccache where
ccache is installed.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

from tools import dev
from tools.build import emsdk
from tools.fetch import checkouts

# The checkouts that an example reads: at run time on the host, and packaged into its page when the web build links it.
EXAMPLE_CHECKOUTS = {"video": ["videos"]}

# The build directories, under the root. PR_BUILD_DIR names the default build's directory in place of build, for a
# second checkout's work beside the first's, or work that must not share a build with another running at the same time.
DEFAULT = os.path.join(dev.ROOT, os.environ.get("PR_BUILD_DIR", "build"))
DEBUG = os.path.join(dev.ROOT, "build-debug")
RELEASE = os.path.join(dev.ROOT, "build-release")
ASAN = os.path.join(dev.ROOT, "build-asan")
TSAN = os.path.join(dev.ROOT, "build-tsan")
WASM = os.path.join(dev.ROOT, "build-wasm")
WASM_DEBUG = os.path.join(dev.ROOT, "build-wasm-debug")
# What runs the simulator builds' programs, as their tests run them (pr_write_tests).
SIMULATOR_RUNNER = os.path.join(dev.ROOT, "tools", "build", "simulator.py")
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


class UikitPlatform:
    """A platform whose applications are UIKit's: its name in CMake (CMAKE_SYSTEM_NAME) and in Xcode's downloads, and
    the SDKs of its devices and of its simulator."""

    def __init__(self, system, sdk, simulator_sdk):
        self.system = system
        self.sdk = sdk
        self.simulator_sdk = simulator_sdk


UIKIT_PLATFORMS = {
    "ios": UikitPlatform("iOS", "iphoneos", "iphonesimulator"),
    "tvos": UikitPlatform("tvOS", "appletvos", "appletvsimulator"),
    "visionos": UikitPlatform("visionOS", "xros", "xrsimulator"),
}


class Mode:
    """A build: its directory, its CMake build type, its compiler and linker flags, how many times longer its
    harnesses take than the default build's, whether it compiles to WebAssembly or for the UIKit platform that
    `platform` names, for its devices or for its simulator, whose programs run on a simulator device
    (tools/build/simulator.py), the cache entries that it sets beyond the development build's, by name, and whether it
    compiles through a compiler cache where one is installed."""

    def __init__(self, name, directory, build_type, flags="", slowdown=1, wasm=False, platform=None, simulator=False,
                 cache=None, compiler_cache=False):
        self.name = name
        self.directory = directory
        self.build_type = build_type
        self.flags = flags
        self.slowdown = slowdown
        self.wasm = wasm
        self.platform = platform
        self.uikit = platform is not None
        self.simulator = simulator
        self.cache = cache or {}
        self.compiler_cache = compiler_cache

    def lean(self):
        """Returns the lean form of the build, in the build's directory with -lean after it."""
        return Mode(self.name + " lean", self.directory + "-lean", self.build_type, self.flags, self.slowdown,
                    self.wasm, self.platform, self.simulator, dict(self.cache, PR_LEAN="ON"), True)

    def cache_entries(self):
        """The cache entries that the build sets beyond the development build's: its own, and ccache as the compilers'
        launcher if it compiles through a compiler cache and ccache is installed. MSVC then keeps the debug information
        in each object, where ccache can keep it too."""
        entries = dict(self.cache)
        if self.compiler_cache and dev.has("ccache"):
            entries.update({"CMAKE_C_COMPILER_LAUNCHER": "ccache", "CMAKE_CXX_COMPILER_LAUNCHER": "ccache"})
            if dev.WINDOWS:
                entries["CMAKE_MSVC_DEBUG_INFORMATION_FORMAT"] = "Embedded"
        return entries

    def environment(self):
        """The environment that the runners read the build from (tools/suite/corpus.py)."""
        return {"PR_BUILD": self.directory, "PR_SLOWDOWN": self.slowdown}


MODES = {
    "default": Mode("default", DEFAULT, "RelWithDebInfo"),
    "debug": Mode("debug", DEBUG, "Debug"),
    "release": Mode("release", RELEASE, "Release"),
    "asan": Mode("asan", ASAN, "Debug", SANITIZE, ASAN_SLOWDOWN),
    "tsan": Mode("tsan", TSAN, "Debug", THREAD_SANITIZE),
    "wasm": Mode("wasm", WASM, "RelWithDebInfo", wasm=True),
    "wasm-debug": Mode("wasm-debug", WASM_DEBUG, "Debug", wasm=True),
}


def uikit_modes():
    """Returns each UIKit platform's builds by name, as for iOS: ios in build-ios for its devices and ios-sim in
    build-ios-sim for its simulator, each with a Debug form beside it, as ios-debug in build-ios-debug."""
    found = {}
    for platform in UIKIT_PLATFORMS:
        for target, simulator in ((platform, False), (platform + "-sim", True)):
            for suffix, build_type in (("", "RelWithDebInfo"), ("-debug", "Debug")):
                found[target + suffix] = Mode(target + suffix, os.path.join(dev.ROOT, "build-" + target + suffix),
                                              build_type, platform=platform, simulator=simulator)
    return found


MODES.update(uikit_modes())


# The configurations of a build, as the commands name them, and the platforms that it compiles for. The host's platform
# is the default's. The others cross-compile, in the default and debug configurations.
CONFIGURATIONS = ("default", "debug", "release", "asan", "tsan")
# A UIKit platform's devices are named for the platform, as ios, and its simulator after it, as ios-sim.
SIMULATOR_PLATFORMS = tuple(name + "-sim" for name in UIKIT_PLATFORMS)
CROSS_PLATFORMS = ("wasm", *UIKIT_PLATFORMS, *SIMULATOR_PLATFORMS)
PLATFORMS = ("macos", "windows", "linux", *CROSS_PLATFORMS)


def host_platform():
    """Returns the platform of this machine, one of PLATFORMS."""
    return "macos" if dev.darwin() else "windows" if dev.WINDOWS else "linux"


def mode_name(configuration=None, platform=None):
    """Returns the name in MODES of the build of `configuration` for `platform`, or None if there is none. Either may
    be None, for the default configuration and the host's platform."""
    configuration = configuration or "default"
    if platform is None or platform == host_platform():
        return configuration
    if platform in CROSS_PLATFORMS and configuration in ("default", "debug"):
        return platform + ("-debug" if configuration == "debug" else "")
    return None


def take_mode(argv, configurations=CONFIGURATIONS, platforms=PLATFORMS):
    """Removes a configuration of `configurations` from the front of `argv`, then a platform of `platforms`, and returns
    the name in MODES of their build. Fails if there is none."""
    configuration = argv.pop(0) if argv[:1] and argv[0] in configurations else None
    platform = argv.pop(0) if argv[:1] and argv[0] in platforms else None
    name = mode_name(configuration, platform)
    if name is None:
        dev.fail("there is no %s build for %s on %s" % (configuration or "default", platform, host_platform()))
    return name


def mode(name, lean=False):
    """Returns the build that `name` names, a key of MODES, or the default build for None, or with `lean`, its lean
    form."""
    found = MODES[name or "default"]
    if lean:
        found = found.lean()
    if found.flags is None:
        dev.fail("MSVC has no thread sanitizer")
    if found.uikit and not dev.darwin():
        dev.fail("the %s build needs macOS and Xcode" % UIKIT_PLATFORMS[found.platform].system)
    return found


def uikit_sdk(platform, simulator):
    """Fails unless Xcode has the SDK that a UIKit platform's build compiles against, for its devices or for its
    simulator."""
    chosen = UIKIT_PLATFORMS[platform]
    sdk = chosen.simulator_sdk if simulator else chosen.sdk
    if dev.output(["xcrun", "--sdk", sdk, "--show-sdk-path"], check=False) is None:
        dev.fail("the %s build needs Xcode's %s SDK (xcodebuild -downloadPlatform %s)" %
                 (chosen.system, sdk, chosen.system))


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


def simulator_runner(platform):
    """Returns the command, as a CMake list, that runs a program of the build for the simulator of the UIKit platform
    `platform` (CMAKE_CROSSCOMPILING_EMULATOR)."""
    return "%s;%s;--platform;%s" % (sys.executable, SIMULATOR_RUNNER, platform)


def configure(source, directory, build_type, flags="", out=None, wasm=False, platform=None, simulator=False,
              entries=None):
    """Configures the build of the sources in `source` into `directory`, of `build_type`, with the compiler and linker
    flags `flags`, with Ninja, for the host or, with `wasm`, for WebAssembly, or with `platform`, for a UIKit platform's
    devices, or with `simulator` too, for its simulator, and with the cache `entries` beyond the development build's, by
    name. CMake writes to `out`, or to the standard output."""
    entries = entries or {}
    if wasm:
        emsdk.fetch()
        emsdk.environment()
    elif platform:
        uikit_sdk(platform, simulator)
    elif dev.WINDOWS:
        msvc_environment(directory)
    if not dev.has("ninja"):
        dev.fail("ninja is required (brew install ninja, or winget install Ninja-build.Ninja)")
    # lib/tls's OpenSSL backend builds where CMake finds OpenSSL: the system's, or Homebrew's on macOS.
    openssl = None if wasm or platform else openssl_root()
    # The WebAssembly build allocates through the C library, until mimalloc is tried there.
    mimalloc = "OFF" if wasm else "ON"

    # CMake fetches what the build needs (cmake/dependencies.cmake) and keeps the archives in third_party/downloads,
    # which every build directory and worktree shares. A development build has the tests, the tools, the examples and
    # the apps, with warnings as errors, and every backend that the platform supports. CMake's own warnings are errors
    # too, as the compiler's and the linker's are. Development and the benchmarks allocate with mimalloc, which a
    # library build leaves to its embedder. The options are computed only when CMake configures, which a build that is
    # current skips.
    def options():
        found = ["-DPR_DOWNLOAD_DIR=" + os.path.join(dev.ROOT, "third_party", "downloads"), "-DPR_DEVELOP=ON",
                 "-DPR_EXAMPLES=ON", "-DPR_APPS=ON", "-DPR_PLATFORM_MIMALLOC=" + mimalloc, *strict_warnings()]
        if openssl:
            found.append("-DOPENSSL_ROOT_DIR=" + openssl)
        # The WebAssembly build's programs run under emsdk's Node.js, as the tests and their discovery run them.
        # Emscripten's toolchain finds Node.js only on the path. A simulator build's run on a simulator device.
        if wasm:
            found.append("-DCMAKE_CROSSCOMPILING_EMULATOR=" + emsdk.node())
        if simulator:
            found.append("-DCMAKE_CROSSCOMPILING_EMULATOR=" + simulator_runner(platform))
        found += ["-D%s=%s" % entry for entry in entries.items()]
        return found

    def cached(name, value):
        """Returns true if the cache holds `value` for the entry `name`, of whatever type."""
        return any(line.startswith(name + ":") and line.endswith("=" + value) for line in lines)

    # A tree that another generator configured, or that was configured from another checkout's sources, is configured
    # again, with Ninja.
    lines = cache(directory)
    if lines is not None and ("CMAKE_GENERATOR:INTERNAL=Ninja" not in lines
                              or not same_path(cached_home(lines), source)):
        shutil.rmtree(directory)
        lines = None
    if lines is None and wasm:
        dev.run(["cmake", "-S", source, "-B", directory, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + build_type,
                 "-DCMAKE_TOOLCHAIN_FILE=" + emsdk.toolchain(), *options()], stdout=out, stderr=out)
    elif lines is None and platform:
        chosen = UIKIT_PLATFORMS[platform]
        sysroot = chosen.simulator_sdk if simulator else chosen.sdk
        dev.run(["cmake", "-S", source, "-B", directory, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + build_type,
                 "-DCMAKE_SYSTEM_NAME=" + chosen.system, "-DCMAKE_OSX_SYSROOT=" + sysroot,
                 "-DCMAKE_OSX_ARCHITECTURES=arm64", *options()], stdout=out, stderr=out)
    elif lines is None and dev.WINDOWS:
        # MSVC's default flags, such as /EHsc, stay, and the build's flags add to them. The linker takes none.
        dev.run(["cmake", "-S", source, "-B", directory, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + build_type,
                 "-DCMAKE_C_FLAGS_INIT=" + flags, "-DCMAKE_CXX_FLAGS_INIT=" + flags, *options()], stdout=out,
                stderr=out)
    elif lines is None:
        dev.run(["cmake", "-S", source, "-B", directory, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + build_type,
                 "-DCMAKE_C_FLAGS=" + flags, "-DCMAKE_CXX_FLAGS=" + flags, "-DCMAKE_EXE_LINKER_FLAGS=" + flags,
                 *options()], stdout=out, stderr=out)
    # A configure that was stopped before it generated leaves a cache and no build.ninja, which CMake finishes.
    elif (not os.path.exists(os.path.join(directory, "build.ninja"))
          or "PR_DEVELOP:BOOL=ON" not in lines or "PR_APPS:BOOL=ON" not in lines
          or "PR_PLATFORM_MIMALLOC:BOOL=" + mimalloc not in lines
          or "CMAKE_BUILD_TYPE:STRING=" + build_type not in lines
          or (wasm and not cached("CMAKE_CROSSCOMPILING_EMULATOR", emsdk.node()))
          or (simulator and not cached("CMAKE_CROSSCOMPILING_EMULATOR", simulator_runner(platform)))
          or (openssl and not cached("OPENSSL_ROOT_DIR", openssl))
          or not all(cached(name, value) for name, value in entries.items())):
        dev.run(["cmake", "-S", source, "-B", directory, "-DCMAKE_BUILD_TYPE=" + build_type, *options()], stdout=out,
                stderr=out)


def build_targets(directory):
    """Returns the names of the targets of the configured build in `directory`, once CMake has regenerated it for any
    change to the sources. A target that needs a tool, such as Cargo, is in the build only where the tool is."""
    dev.run(["ninja", "-C", directory, "build.ninja"], stdout=sys.stderr)
    listed = dev.output(["ninja", "-C", directory, "-t", "targets", "all"])
    return {line.split(":")[0] for line in listed.splitlines()}


def build(name="default", targets=(), keep_going=False, lean=False, configure_only=False):
    """Configures and builds the build that `name` names, or with `lean` its lean form, or only `targets` and what they
    need, and returns it. With `keep_going`, Ninja builds everything that does not depend on a failure, so one build
    reports every error. With `configure_only`, it is configured and nothing is built."""
    chosen = mode(name, lean)
    if chosen.wasm and chosen.cache.get("PR_LEAN") != "ON":
        for example, names in EXAMPLE_CHECKOUTS.items():
            if not targets or example in targets:
                checkouts.fetch(*names)
    configure(dev.ROOT, chosen.directory, chosen.build_type, chosen.flags, wasm=chosen.wasm, platform=chosen.platform,
              simulator=chosen.simulator, entries=chosen.cache_entries())
    if configure_only:
        return chosen
    command = ["cmake", "--build", chosen.directory]
    for target in targets:
        command += ["--target", target]
    if keep_going:
        command += ["--", "-k", "0"]
    dev.run(command)
    return chosen

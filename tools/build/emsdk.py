"""Emscripten, which the WebAssembly build compiles with: emsdk under third_party, at a pinned release.

emsdk installs the release's compiler, its Node.js and its Python beside itself, and activating the release writes the
configuration that Emscripten reads (.emscripten). The build reads it through EM_CONFIG, so nothing outside
third_party/emsdk changes. The tests run under that Node.js, which CMake's Emscripten toolchain names as the emulator
that runs a test.

Emscripten keeps the system libraries and headers that it compiles in a cache, which by default lies in the release's
directory and records the path of whoever filled it. Each build directory has its own instead (EM_CACHE), so that the
builds of two worktrees, which share third_party, do not clear each other's.
"""

import ast
import os
import sys

from tools import dev
from tools.fetch import checkouts

# The release, and the commit of emsdk that its tag names.
VERSION = "6.0.13"
COMMIT = "35659964bc2762fffec0d257997fb61475f765b7"

DIRECTORY = os.path.join(checkouts.THIRD_PARTY, "emsdk")
CONFIG = os.path.join(DIRECTORY, ".emscripten")
# The release that the directory has installed and activated, written once both succeed.
STAMP = os.path.join(DIRECTORY, "prax-version")


def fetch():
    """Fetches emsdk at its commit, and installs and activates the release, unless the directory has done so."""
    checkouts.Git("emsdk", "https://github.com/emscripten-core/emsdk.git", COMMIT).fetch()
    try:
        with open(STAMP) as f:
            installed = f.read().strip()
    except FileNotFoundError:
        installed = None
    if installed == VERSION and os.path.exists(CONFIG):
        return
    script = os.path.join(DIRECTORY, "emsdk.py")
    dev.run([sys.executable, script, "install", VERSION], cwd=DIRECTORY)
    dev.run([sys.executable, script, "activate", VERSION], cwd=DIRECTORY)
    with open(STAMP, "w") as f:
        f.write(VERSION + "\n")


def toolchain():
    """The CMake toolchain file of the release."""
    return os.path.join(DIRECTORY, "upstream", "emscripten", "cmake", "Modules", "Platform", "Emscripten.cmake")


def cache(directory):
    """The cache of Emscripten's compiled system libraries and headers for the build in `directory`."""
    return os.path.join(directory, "emcache")


def environment(directory):
    """Points Emscripten at the configuration that activating the release wrote, and at the cache of the build in
    `directory`, for this process and what it runs."""
    os.environ["EM_CONFIG"] = CONFIG
    os.environ["EM_CACHE"] = cache(directory)


def node(directory):
    """The Node.js that the release installed, which runs the programs of the build in `directory`. The configuration
    computes its path, so Emscripten's own em-config reads it."""
    environment(directory)
    found = dev.output([sys.executable, os.path.join(DIRECTORY, "upstream", "emscripten", "em-config.py"), "NODE_JS"])
    if not found:
        dev.fail("%s names no NODE_JS" % CONFIG)
    # A command with its arguments prints as a Python list, of which the program comes first.
    value = found.strip()
    if value.startswith("["):
        value = ast.literal_eval(value)[0]
    return os.path.normpath(value)

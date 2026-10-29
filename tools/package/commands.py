"""`./dev.sh package`: the libraries built with the build's defaults and installed, then tools/package built and run
against the installed package, and against the source tree as FetchContent adds it."""

import os
import shutil

from tools import dev
from tools.build import build as builds

USAGE = {
    "package": """\
./dev.sh package
    the libraries as a consumer takes them: built with the build's defaults and installed, and tools/package built
    against the installed package and against the source tree, and run""",
}


def package(argv):
    dev.usage(argv, USAGE["package"])
    downloads = "-DPR_DOWNLOAD_DIR=" + os.path.join(dev.ROOT, "third_party", "downloads")
    strict = builds.strict_warnings()
    if dev.WINDOWS:
        builds.msvc_environment(builds.PACKAGE)
    library = os.path.join(builds.PACKAGE, "library")
    prefix = os.path.join(builds.PACKAGE, "prefix")
    dev.run(["cmake", "-S", ".", "-B", library, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
             "-DCMAKE_INSTALL_PREFIX=" + prefix, *strict, downloads])
    dev.run(["cmake", "--build", library])
    shutil.rmtree(prefix, ignore_errors=True)
    dev.run(["cmake", "--install", library])
    for name, options in (("installed", ["-DCMAKE_PREFIX_PATH=" + prefix]),
                          ("source", ["-DPRAX_SOURCE_DIR=" + dev.ROOT, downloads])):
        directory = os.path.join(builds.PACKAGE, name)
        dev.run(["cmake", "-S", "tools/package", "-B", directory, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 *options, *strict])
        dev.run(["cmake", "--build", directory])
        dev.run([dev.executable(directory, "consumer")])

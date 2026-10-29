#!/usr/bin/env python3
"""Lists the pages of our suite that do not fit the suite's viewport of 600 by 800, as Chrome lays them out.

A page of suites/prax is read without scrolling (docs/testing.md). Chrome loads tools/suite/prax/suite_fit.html from a
server for the checkout (tools/browsers/browsers.py). That page loads each page that the index files list into a frame
of 600 by 800, and once the page's fonts have loaded, reports the size of its document. A page wider or taller than the
frame is listed with its size.

Usage: python3 -m tools.suite.prax.suite_fit --build <directory> [directory-of-the-suite...] [--timeout S]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

from tools.browsers import browsers
from tools.suite.prax import suite

WIDTH, HEIGHT = 600, 800


def main():
    parser = argparse.ArgumentParser(description="Our pages that do not fit 600 by 800.")
    parser.add_argument("--build", required=True)
    parser.add_argument("directories", nargs="*", help="the suite's directories to measure; every one by default")
    parser.add_argument("--timeout", type=int, default=600)
    options = parser.parse_args()

    names = options.directories or [d.name for d in suite.read_suite([])]
    pages = ["/suites/prax/%s/%s" % (d, p) for d, p in suite.pages_of(names)]
    if not pages:
        sys.exit("no pages to measure")

    server = browsers.Server()
    server.config = pages
    url = "http://127.0.0.1:%d/tools/suite/prax/suite_fit.html" % server.server_address[1]
    browser = browsers.BROWSERS["chrome"]
    profile = tempfile.mkdtemp(prefix="suite-fit-")
    process = None
    try:
        process = browser.launch(url, profile, options.build, ["--headless", "--force-device-scale-factor=1"])
        if not server.reported.wait(options.timeout):
            sys.exit("Chrome did not report within %ds" % options.timeout)
    finally:
        if process is not None:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
        shutil.rmtree(profile, ignore_errors=True)

    over = 0
    for page, size in server.report["pages"].items():
        if isinstance(size, str):
            print("%s: %s" % (page[1:], size))
            over += 1
        elif size[0] > WIDTH or size[1] > HEIGHT:
            print("%s: %d by %d" % (page[1:], size[0], size[1]))
            over += 1
    print("\n%d of %d pages do not fit %d by %d" % (over, len(pages), WIDTH, HEIGHT))
    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main())

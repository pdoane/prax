"""The base of an A/B comparison, which `./dev.sh suite --ab` and `./dev.sh bench ab` share: a commit, or a directory
of sources (a worktree, or a copy of the tree made before a change, for a base that is not a commit). The base's
harnesses are built in Release from its own sources, beside this checkout's third_party.
"""

import os
import shutil
import subprocess
import sys

from tools import dev
from tools.build import build


def commit(base=None):
    """Returns the hash of the commit that an A/B compares with: `base`, or else HEAD where the tree has changes, and
    HEAD's parent where it has none. The scoreboards that suite runs write are not changes, so that the A/B of a second
    suite after the first wrote its scoreboard compares with the same commit."""
    if not base:
        clean = dev.run(["git", "diff", "--quiet", "HEAD", "--", ".", ":(exclude)suites/*/scoreboard.md"],
                        check=False) == 0
        base = "HEAD^" if clean else "HEAD"
    sha = dev.output(["git", "rev-parse", "--verify", "--quiet", base + "^{commit}"], check=False)
    if not sha:
        dev.fail("no commit " + base)
    return sha


def third_party(directory):
    """Links this checkout's third_party into the sources in `directory`."""
    link = os.path.join(directory, "third_party")
    if os.path.islink(link) or os.path.isfile(link):
        os.remove(link)
    elif os.path.isdir(link):
        shutil.rmtree(link)
    os.symlink(os.path.join(dev.ROOT, "third_party"), link)


def sync(source, excludes):
    """Brings the sources of the base to the files of `source`, a directory. Only the files whose contents differ are
    written, so a build of them sees only them change."""
    destination = os.path.join(build.AB, "src")
    os.makedirs(destination, exist_ok=True)
    command = ["rsync", "-rc", "--delete"]
    for exclude in excludes:
        command += ["--exclude", exclude]
    dev.run(command + [source + "/", destination + "/"], stdout=sys.stderr)
    third_party(destination)
    return destination


def extract(sha, paths, directory):
    """Writes the files of commit `sha` under `paths`, or every file, into `directory`."""
    files = subprocess.Popen(["git", "archive", sha, *paths], cwd=dev.ROOT, stdout=subprocess.PIPE)
    unpack = subprocess.run(["tar", "-x", "-C", directory], stdin=files.stdout)
    files.wait()
    if files.returncode != 0 or unpack.returncode != 0:
        dev.fail("cannot extract the files of " + sha)


def commit_sources(sha):
    """Brings the base's sources to the files of commit `sha`, and returns their directory."""
    archive = os.path.join(build.AB, "archive")
    shutil.rmtree(archive, ignore_errors=True)
    os.makedirs(archive)
    extract(sha, [], archive)
    sources = sync(archive, ["/third_party"])
    shutil.rmtree(archive)
    return sources


class Base:
    """The base of an A/B: commit `sha`, or the sources in `directory`."""

    def __init__(self, ref=None, directory=None):
        if directory:
            if not os.path.isdir(directory):
                dev.fail("no directory " + directory)
            self.directory = os.path.abspath(directory)
            self.sha = None
            self.label = directory
        else:
            self.directory = None
            self.sha = commit(ref)
            short = dev.output(["git", "rev-parse", "--short", self.sha])
            self.label = "%s, %s" % (ref, short) if ref else short

    def sources(self):
        """Brings the base's sources up to date in build-ab/src, and returns their directory."""
        if self.sha:
            return commit_sources(self.sha)
        return sync(self.directory, ["/third_party", "/build*", "/.git", "/perf", "/_cache"])

    def harnesses(self, targets):
        """Returns the directory that holds the base's harnesses `targets`, in Release. A commit's harnesses are kept by
        commit, so they are built once. A directory's files can change, so its harnesses are built each time, and only
        what differs is rebuilt."""
        bin = os.path.join(build.AB, "bin", self.sha or "directory")
        if self.sha and all(os.path.exists(dev.executable(bin, target)) for target in targets):
            return bin
        directory = os.path.join(build.AB, "build")
        self.compile(directory, "Release", targets)
        shutil.rmtree(bin, ignore_errors=True)
        os.makedirs(bin)
        for target in targets:
            shutil.copy2(dev.executable(directory, target), bin)
        return bin

    def compile(self, directory, build_type, targets, log=None):
        """Builds `targets` of the base's sources into `directory`, of `build_type`. With `log`, the build writes to
        that file and says only where it failed."""
        sources = self.sources()
        command = ["cmake", "--build", directory] + [part for target in targets for part in ("--target", target)]
        if log is None:
            build.configure(sources, directory, build_type, out=sys.stderr)
            dev.run(command, stdout=sys.stderr)
            return
        with open(log, "w") as out:
            build.configure(sources, directory, build_type, out=out)
            status = dev.run(command, check=False, stdout=out, stderr=out)
        if status != 0:
            with open(log) as f:
                sys.stderr.write("".join(f.readlines()[-20:]))
            dev.fail("the base's build failed; see " + log)

    def runner(self):
        """Returns the directory of the base's tools and suites, which runs the base's harnesses: a base runs with its
        own runner, which speaks to its own harnesses. A commit's are kept by commit."""
        if self.directory:
            return self.directory
        directory = os.path.join(build.AB, "runner", self.sha)
        if not os.path.isdir(os.path.join(directory, "tools")):
            os.makedirs(directory, exist_ok=True)
            extract(self.sha, ["tools", "suites"], directory)
            third_party(directory)
        return directory

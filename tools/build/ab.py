"""The base of an A/B comparison, which `./dev.sh suite --ab` and `./dev.sh bench ab` share: a commit, or a directory
of sources (a worktree, or a copy of the tree made before a change, for a base that is not a commit). The base's
harnesses are built in Release from its own sources, beside this checkout's third_party.
"""

import filecmp
import fnmatch
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
    """Links this checkout's third_party into the sources in `directory`. On Windows a symbolic link needs Developer
    Mode, or the privilege to create one."""
    link = os.path.join(directory, "third_party")
    if os.path.islink(link) or os.path.isfile(link):
        os.remove(link)
    elif os.path.isdir(link):
        shutil.rmtree(link)
    os.symlink(os.path.join(dev.ROOT, "third_party"), link, target_is_directory=True)


def excluded(relative, excludes):
    """Returns true if the path `relative` to the root of a sync is under one of `excludes`, patterns of a top-level
    name that start with "/", such as "/build*"."""
    top = relative.replace(os.sep, "/").split("/")[0]
    return any(fnmatch.fnmatchcase(top, pattern.lstrip("/")) for pattern in excludes)


def sync(source, excludes):
    """Brings the sources of the base to the files of `source`, a directory, apart from the `excludes` and the linked
    third_party. Only the files whose contents differ are written, so a build of them sees only them change, and the
    files that `source` no longer has are removed. A written file takes the time it is written, not its source's: a
    commit's archive dates its files by the commit, which can be older than the objects of a later base's build."""
    destination = os.path.join(build.AB, "src")
    os.makedirs(destination, exist_ok=True)
    kept = set()
    for directory, names, files in os.walk(source):
        relative = os.path.relpath(directory, source)
        relative = "" if relative == "." else relative
        names[:] = [name for name in names if not excluded(os.path.join(relative, name), excludes + ["/third_party"])]
        os.makedirs(os.path.join(destination, relative), exist_ok=True)
        kept.add(relative)
        for name in files:
            path = os.path.join(relative, name)
            if excluded(path, excludes + ["/third_party"]):
                continue
            kept.add(path)
            target = os.path.join(destination, path)
            if not os.path.isfile(target) or not filecmp.cmp(os.path.join(source, path), target, shallow=False):
                # A copy takes the time of the sync, not the source's, which for a commit is its time: a base
                # older than the last one built would otherwise look older than its objects, which would not build.
                shutil.copy(os.path.join(source, path), target)
    for directory, names, files in os.walk(destination, topdown=False):
        relative = os.path.relpath(directory, destination)
        relative = "" if relative == "." else relative
        if excluded(relative, ["/third_party"]) and relative != "":
            continue
        for name in files:
            path = os.path.join(relative, name)
            if path not in kept and not excluded(path, ["/third_party"]):
                os.remove(os.path.join(destination, path))
        for name in names:
            path = os.path.join(relative, name)
            full = os.path.join(destination, path)
            if path not in kept and not excluded(path, ["/third_party"]) and not os.path.isjunction(full) \
                    and not os.path.islink(full):
                os.rmdir(full)
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
        """Returns the directory that holds the base's harnesses `targets`, in Release, and the targets that the base's
        build does not have, such as a harness that a later commit added or that needs a tool the base's build did not
        find. A commit's harnesses are kept by commit, so they are built once; the targets it lacks are kept beside
        them, in `absent`. A directory's files can change, so its harnesses are built each time, and only what differs
        is rebuilt."""
        bin = os.path.join(build.AB, "bin", self.sha or "directory")
        absent_file = os.path.join(bin, "absent")
        if self.sha and os.path.exists(absent_file):
            with open(absent_file) as f:
                absent = set(f.read().split())
            if all(target in absent or os.path.exists(dev.executable(bin, target)) for target in targets):
                return bin, sorted(absent & set(targets))
        directory = os.path.join(build.AB, "build")
        build.configure(self.sources(), directory, "Release", out=sys.stderr)
        present = build.build_targets(directory)
        built = [target for target in targets if target in present]
        if built:
            dev.run(["cmake", "--build", directory] + [part for target in built for part in ("--target", target)],
                    stdout=sys.stderr)
        shutil.rmtree(bin, ignore_errors=True)
        os.makedirs(bin)
        for target in built:
            shutil.copy2(dev.executable(directory, target), bin)
        absent = sorted(set(targets) - set(built))
        with open(absent_file, "w") as f:
            f.write("\n".join(absent))
        return bin, absent

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

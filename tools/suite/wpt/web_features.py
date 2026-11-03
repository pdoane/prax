"""Reads the engine's rulings on web features, and the features that WPT's pages test.

tools/suite/wpt/web_features.txt rules on each feature of web-features
(https://github.com/web-platform-dx/web-features), at the version that its header states, that the WPT suites test. It
also rules on each feature that the engine has considered and web-features does not name, which it names by the anchor
of its definition (css-color-5/#at-profile). ./dev.sh fetch webfeatures fetches that version. A WPT directory's
WEB_FEATURES.yml maps its files to features. Each rule pairs a file name pattern with the features that the matching
files test, and the first rule that matches decides. A directory without a WEB_FEATURES.yml uses its parent's."""

import fnmatch
import json
import os
import posixpath
import re

from tools.suite import corpus

RULINGS_FILE = os.path.join(corpus.ROOT, "tools", "suite", "wpt", "web_features.txt")
DATA_DIR = os.path.join(corpus.ROOT, "third_party", "web-features")

# The rulings that mean the engine builds a feature, and every ruling that the file may give. A page that tests a
# feature needs it built.
BUILT = {"build", "quickjs"}
RULINGS = BUILT | {"cut", "open"}


def version():
    """Returns the web-features version that the rulings are made against."""
    with open(RULINGS_FILE, encoding="utf-8") as f:
        for line in f:
            match = re.match(r"# web-features (\S+)$", line.rstrip())
            if match:
                return match.group(1)
    raise SystemExit("%s names no web-features version" % RULINGS_FILE)


def rulings():
    """Returns each feature's ruling, by identifier, and the file's problems: an unknown ruling, or a feature ruled
    twice."""
    ruled = {}
    problems = []
    with open(RULINGS_FILE, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            if line.startswith("#"):
                continue
            fields = line.split()
            if not fields:
                continue
            if len(fields) != 2 or fields[1] not in RULINGS:
                problems.append((number,
                                 "a line is a feature and one of %s" % ", ".join(sorted(RULINGS))))
                continue
            if fields[0] in ruled:
                problems.append((number, "%s is ruled twice" % fields[0]))
            ruled[fields[0]] = fields[1]
    return ruled, problems


def data():
    """Returns the features of the pinned web-features data, by identifier, or None if the data is not fetched."""
    path = os.path.join(DATA_DIR, version() + ".json")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        return json.load(f)["features"]


def unnamed(name):
    """Returns true if a ruling is of a feature that web-features does not name, which it names by a spec's anchor: a
    draft and its anchor, or a page of a draft with several pages and its anchor (svg2/paths.html#PathLengthProperty).
    No identifier of web-features holds a #."""
    return "#" in name


def check(features, tested):
    """Returns the problems with the rulings, checked against the data and the features that the suites test: a tested
    feature without a ruling, a ruling that names no feature, a ruling for a feature that moved or split, whose ruling
    belongs to the features that it became, and a ruling on a feature that no suite tests."""
    ruled, problems = rulings()
    live = {name for name, feature in features.items() if feature.get("kind") == "feature"}
    for name in sorted(tested & live - set(ruled)):
        problems.append((0, "%s has no ruling" % name))
    for name in sorted(name for name in set(ruled) - live if not unnamed(name)):
        problems.append((0, "%s is not a feature of web-features %s" % (name, version())))
    for name in sorted(set(ruled) & live - tested):
        problems.append((0, "%s is tested by no suite" % name))
    return problems


class Pages:
    """The features that WPT's pages test, from the WEB_FEATURES.yml files of a checkout."""

    def __init__(self, root, features):
        self.root = root
        self.features = features
        self.manifests = {}
        for directory, _, files in os.walk(root):
            if "WEB_FEATURES.yml" in files:
                relative = os.path.relpath(directory, root).replace(os.sep, "/")
                with open(os.path.join(directory, "WEB_FEATURES.yml"), encoding="utf-8") as f:
                    self.manifests["" if relative == "." else relative] = parse(f.read())

    def resolve(self, name):
        """Returns the feature that a name now refers to, following the features that web-features moved or split."""
        for _ in range(8):
            feature = self.features.get(name)
            if feature is None or feature.get("kind") not in ("moved", "split"):
                return name
            moved = feature["kind"] == "moved"
            name = feature["redirect_target"] if moved else feature["redirect_targets"][0]
        return name

    def named(self, directories, excluded=()):
        """Returns the features that the manifests of `directories` name, leaving out those of `excluded`, which are
        directories inside them."""
        def within(directory, tops):
            return any(directory == top or directory.startswith(top + "/") for top in tops)
        return {self.resolve(feature) for directory, rules in self.manifests.items()
                if within(directory, directories) and not within(directory, excluded)
                for _, features in rules for feature in features}

    def of(self, name):
        """Returns the features that the page `name`, a path in the checkout, tests: the features of the first matching
        rule in the nearest manifest, or none if no rule matches."""
        directory = posixpath.dirname(name)
        while True:
            rules = self.manifests.get(directory)
            if rules is not None:
                relative = posixpath.relpath(name, directory) if directory else name
                for pattern, features in rules:
                    if fnmatch.fnmatchcase(relative,
                                           pattern) or fnmatch.fnmatchcase(posixpath.basename(name),
                                                                           pattern):
                        return [self.resolve(feature) for feature in features]
                return []
            if not directory:
                return []
            directory = posixpath.dirname(directory)


def parse(text):
    """Returns the rules of a WEB_FEATURES.yml in order, each as a pattern and its features."""
    rules = []
    for line in text.splitlines():
        match = re.match(r"^\s*-\s*(.+?)\s*:\s*\[(.*)\]\s*(#.*)?$", line)
        if match:
            pattern = match.group(1).strip().strip("\"'")
            rules.append((pattern,
                          [f.strip().strip("\"'") for f in match.group(2).split(",") if f.strip()]))
    return rules

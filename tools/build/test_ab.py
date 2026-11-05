"""Tests of the A/B base's sources (tools/build/ab.py)."""

import os
import tempfile
import time
import unittest
from unittest import mock

from tools.build import ab
from tools.build import build


class Sync(unittest.TestCase):
    def test_a_changed_file_is_newer_than_an_older_source(self):
        with tempfile.TemporaryDirectory() as root:
            source = os.path.join(root, "source")
            os.makedirs(source)
            path = os.path.join(source, "a.cpp")
            with open(path, "w") as f:
                f.write("old")
            with mock.patch.object(build, "AB", os.path.join(root, "ab")), mock.patch.object(ab, "third_party"):
                written = os.path.join(ab.sync(source, []), "a.cpp")
                # A later sync of a file that a commit dated long ago, as an archive dates it, still writes it now.
                with open(path, "w") as f:
                    f.write("new")
                os.utime(path, (1, 1))
                before = time.time() - 1
                ab.sync(source, [])
            with open(written) as f:
                self.assertEqual(f.read(), "new")
            self.assertGreater(os.path.getmtime(written), before)


if __name__ == "__main__":
    unittest.main()

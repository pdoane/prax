"""Shards of a module of the tools' tests, which the unit tests' run gives programs of their own so that a long module's
tests run beside each other (tools/lint/commands.py, tools_tests).

A module that declares `SHARDS = <n>` at its top level is run as n programs, each with PR_TEST_SHARD set to
`<index>/<n>`, and keeps its shard's tests with `load_tests = shards.load_tests`. Without PR_TEST_SHARD, every test
runs.
"""

import os
import unittest

# The environment variable that names a program's shard, as <index>/<count>.
VARIABLE = "PR_TEST_SHARD"


def tests_of(suite):
    """Returns the tests of `suite`, in order, outside the suites that hold them."""
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            yield from tests_of(test)
        else:
            yield test


def load_tests(loader, tests, pattern):
    """unittest's load_tests protocol: keeps every count-th test of the module, from the shard's index."""
    shard = os.environ.get(VARIABLE)
    if not shard:
        return tests
    index, count = (int(part) for part in shard.split("/"))
    return unittest.TestSuite(test for i, test in enumerate(tests_of(tests)) if i % count == index)

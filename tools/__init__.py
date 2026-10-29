"""The development build's tools, as one package: run each with `python3 -m tools.<package>.<module>` from the
repository's root, as dev.sh does."""

import os

# The repository's root.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

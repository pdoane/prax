#!/bin/sh
# The developer workflow, which tools/__main__.py defines: `./dev.sh help` lists the commands, and
# `./dev.sh <command> --help` describes one. It runs python3, or python where that is the name Python is installed by.
cd "$(dirname "$0")" || exit 1
if python3 --version >/dev/null 2>&1; then
    exec python3 -m tools "$@"
fi
exec python -m tools "$@"

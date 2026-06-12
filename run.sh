#!/bin/bash
set -e
cd "$(dirname "$0")"

# clang/make/git ship with the Xcode Command Line Tools; everything else
# (static raylib 6, Freedoom WAD) is fetched and built by the Makefile on
# first run, so there is no Homebrew dependency to check.
command -v clang >/dev/null 2>&1 || {
    echo "clang not found — install Xcode Command Line Tools: xcode-select --install" >&2
    exit 1
}

pkill -x helldrop 2>/dev/null || true

# Incremental: make only relinks/recompiles when sources or headers changed.
make -s game

# Every run writes logs/run-<timestamp>.ndjson (logs/latest.ndjson points at
# the live one — tail it with ./debug.sh). Keep the last 10 runs around.
mkdir -p logs
ls -t logs/run-*.ndjson 2>/dev/null | tail -n +10 | xargs rm -f 2>/dev/null || true

# Run the binary inside the bundle directly (not via `open`) so it stays
# attached to this terminal — Ctrl+C kills it and output lands here — while
# macOS still picks up the bundle's icon for cmd+tab/Dock.
exec ./Helldrop.app/Contents/MacOS/helldrop "$@"

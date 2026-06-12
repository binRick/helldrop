#!/bin/bash
# Tail the game's NDJSON state log. Open in a second terminal while the game
# runs to watch full gamestate snapshots (5 Hz) and events live. Copy chunks
# from it into a message to describe bugs precisely. Pipe through jq for
# pretty records:  ./debug.sh | jq .
cd "$(dirname "$0")"
LOG=logs/latest.ndjson
if [[ ! -e "$LOG" ]]; then
    echo "Waiting for $LOG — launch the game first (./run.sh)."
    until [[ -e "$LOG" ]]; do sleep 0.3; done
fi
exec tail -F "$LOG"

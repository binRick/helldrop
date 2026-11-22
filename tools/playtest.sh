#!/bin/bash
# Headless balance/regression matrix (make test). Every E1 map, unaided and
# with scripted drops, via --simonly (no rendering, hidden window, seconds
# per map). Contract (CLAUDE.md): every match ends; scripted drops kill him
# everywhere; unaided he usually exits. Exit 1 on any violation.
set -u
cd "$(dirname "$0")/.."
# Run a copy from outside the .app bundle: a bundled binary registers as a
# Dock app on launch, so a test matrix would flash a row of game icons.
mkdir -p build
cp ./Helldrop.app/Contents/MacOS/helldrop build/helldrop-test
BIN=./build/helldrop-test
MAPS="E1M1 E1M2 E1M3 E1M4 E1M5 E1M6 E1M7 E1M8 E1M9"
MAXTICS=63000   # 30 minutes of game time: a match that runs this long stalled
fail=0; exits=0; n=0
printf "%-5s %-8s %-8s %7s %4s %6s %5s\n" MAP MODE RESULT SECS HP KILLS SKIPS
for m in $MAPS; do
  for mode in solo auto; do
    extra=""; [ $mode = auto ] && extra="--autodrop"
    line=$(perl -e 'alarm shift; exec @ARGV' 120 $BIN --map $m --simonly $MAXTICS --no-log $extra 2>/dev/null | grep '^{"map"')
    res=$(echo "$line" | sed -E 's/.*"result":"([a-z_]+)".*/\1/')
    secs=$(echo "$line" | sed -E 's/.*"secs":([0-9.]+).*/\1/')
    hp=$(echo "$line" | sed -E 's/.*"hp":(-?[0-9]+).*/\1/')
    kills=$(echo "$line" | sed -E 's/.*"kills":([0-9]+).*/\1/')
    skips=$(echo "$line" | sed -E 's/.*"nav_skips":([0-9]+).*/\1/')
    printf "%-5s %-8s %-8s %7s %4s %6s %5s\n" $m $mode "$res" "$secs" "$hp" "$kills" "$skips"
    case "$res" in exited|dead) ;; *) echo "  FAIL: $m $mode never ended ($res)"; fail=1;; esac
    if [ $mode = auto ] && [ "$res" != dead ]; then echo "  FAIL: scripted drops did not kill him on $m"; fail=1; fi
    if [ $mode = solo ]; then n=$((n+1)); [ "$res" = exited ] && exits=$((exits+1)); fi
  done
done
echo "unaided exits: $exits/$n"
if [ $exits -lt 6 ]; then echo "  FAIL: unaided he should usually exit (>= 6/9)"; fail=1; fi
seeded_result() {
  "$BIN" --map E1M1 --wad assets/helldrop.wad --simonly 10000 --no-log \
    --autodrop --seed "$1" 2>/dev/null | grep '^{"map"'
}
seeded=$(seeded_result 12345)
replayed=$(seeded_result 12345)
alternate=$(seeded_result 54321)
if [ -z "$seeded" ] || [ "$seeded" != "$replayed" ] || [ -z "$alternate" ] || [ "$seeded" = "$alternate" ]; then
  echo "FAIL: seeded combat must replay identically and vary between seeds"
  fail=1
else
  echo "seeded combat replay: PASS"
fi
[ $fail = 0 ] && echo "PASS" || echo "FAILED"
exit $fail

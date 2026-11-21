#!/bin/bash
# Rebuild the Helldrop 3D model roster.
#
#   tools/models/build_all.sh [name ...]        # default: every model
#
# 1. Blender (on the "mini" host by default; BLENDER_HOST=local runs it on this Mac) builds each
#    GLB from tools/models/*.py and renders EEVEE previews of the re-imported GLBs.
# 2. Back here: GLBs -> assets/models/, raylib check of every GLB with the vendored raylib (hidden
#    window, no Dock tile), assets/models/manifest.json, contact sheets in build/model_previews/.
# The scratch dir on the Blender host is deleted afterwards (mini's disk is nearly full).
set -euo pipefail
cd "$(dirname "$0")/../.."
R=$PWD
HOST=${BLENDER_HOST:-mini}
BL=/Applications/Blender.app/Contents/MacOS/Blender
SCR=$R/build/models_tmp
OUT=$R/build/models_out
NAMES="$*"

run() {   # run a shell command on the Blender host
  if [[ $HOST == local ]]; then bash -c "$1"; else ssh "$HOST" "$1"; fi
}

echo "==> Blender on $HOST: ${NAMES:-all models}"
if [[ $HOST != local ]]; then
  ssh "$HOST" "mkdir -p $R/tools/models"
  rsync -a --delete --exclude __pycache__ tools/models/ "$HOST:$R/tools/models/"
fi
run "rm -rf $SCR && mkdir -p $SCR && cd $R && $BL -b --factory-startup -P tools/models/build.py -- $SCR $NAMES 2>&1 | grep -E 'BUILT|FAILED|DONE|Error|Traceback' ; $BL -b --factory-startup -P tools/models/preview.py -- $SCR $SCR/prev $NAMES 2>&1 | grep -cE 'PREVIEW' | sed 's/^/previews: /'"
mkdir -p "$OUT"
if [[ $HOST != local ]]; then
  rsync -a "$HOST:$SCR/" "$OUT/"
  ssh "$HOST" "rm -rf $SCR"
else
  rsync -a "$SCR/" "$OUT/" && rm -rf "$SCR"
fi

echo "==> assets/models"
mkdir -p assets/models build/model_previews/raylib
for f in "$OUT"/*.glb; do cp "$f" assets/models/; done

echo "==> raylib check (hidden window)"
cc -O1 tools/models/check_models.c -Ivendor/raylib-src/src vendor/raylib-src/src/libraylib.mac.a \
   -framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL -framework QuartzCore -o build/check_models
build/check_models build/model_previews/raylib assets/models/*.glb > "$OUT/check.ndjson"

echo "==> contact sheets + manifest"
python3 tools/models/contact_sheet.py "$OUT/prev" build/model_previews
python3 tools/models/make_manifest.py "$OUT" "$OUT/check.ndjson" assets/models/manifest.json

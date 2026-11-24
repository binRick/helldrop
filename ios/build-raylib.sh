#!/bin/bash
set -euo pipefail
sdk=${1:?Pass iphoneos or iphonesimulator}
case "$sdk" in
    iphoneos) target=arm64-apple-ios16.0 ;;
    iphonesimulator) target=arm64-apple-ios16.0-simulator ;;
    *) exit 2 ;;
esac
cd "$(dirname "$0")/.."
out="$PWD/build/ios-$sdk"
source_dir="$PWD/vendor/raylib-src/src"
mkdir -p "$out/src"
cp "$source_dir"/*.h "$out/src/"
ln -sfn "$source_dir/external" "$out/src/external"
modules=(rcore rshapes rtextures rtext rmodels raudio)
for mod in "${modules[@]}"; do cp "$source_dir/$mod.c" "$out/src/"; done
python3 - "$out/src" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
def replace(file, before, after):
    path = root / file
    text = path.read_text()
    if before not in text:
        raise RuntimeError(f"Unsupported raylib source in {file}")
    path.write_text(text.replace(before, after, 1))
replace("rcore.c", "#elif defined(PLATFORM_MEMORY)\n    #include", '#elif defined(PLATFORM_IOS)\n    #include "rcore_ios.c"\n#elif defined(PLATFORM_MEMORY)\n    #include')
# The Apple GLES declarations are imported by the compatibility header.
path = root / "rlgl.h"
lines = path.read_text().splitlines(keepends=True)
idx = next(i for i,line in enumerate(lines) if "#include <GLES3/gl3.h>" in line)
assert "GLES2/gl2ext.h" in lines[idx+2]
del lines[idx:idx+3]
path.write_text("".join(lines))
replace("rlgl.h", "    glBindFramebuffer(GL_FRAMEBUFFER, 0);", "    extern unsigned int KitRcoreGetDefaultFbo(void);\n    glBindFramebuffer(GL_FRAMEBUFFER, KitRcoreGetDefaultFbo());")
PY
flags=(-isysroot "$(xcrun --sdk "$sdk" --show-sdk-path)" -target "$target" -O2 -DPLATFORM_IOS -DGRAPHICS_API_OPENGL_ES3 -Wno-deprecated-declarations -Iios -include ios/raylib_ios_glcompat.h)
for mod in "${modules[@]}"; do
    lang=(-x c)
    if [[ $mod == raudio ]]; then lang=(-x objective-c -fobjc-arc -fmodules); fi
    xcrun clang "${flags[@]}" "${lang[@]}" -c "$out/src/$mod.c" -o "$out/$mod.o"
done
xcrun libtool -static -o "$out/libraylib.a" "$out"/*.o

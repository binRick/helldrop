CC = clang

# Single source-of-truth raylib clone, pinned to a tag so every machine
# compiles against the same version (same pattern as ../chernobyl). Bump
# RAYLIB_TAG and `rm -rf vendor/raylib-src` to upgrade. Statically linked:
# no Homebrew raylib needed at build or run time.
RAYLIB_TAG ?= 6.2
RAYLIB_SRC ?= vendor/raylib-src
RAYLIB_A    = $(RAYLIB_SRC)/src/libraylib.mac.a

# libtess2 (vendored, SGI Free B license): robust tessellation of concave
# Doom sector polygons with holes. Compiled straight into the binary.
TESS_DIR = third_party/libtess2
TESS_SRC = $(wildcard $(TESS_DIR)/Source/*.c)

SRC  = src/main.c src/wad.c src/map.c src/levelmesh.c src/sim.c src/doomgfx.c \
       src/sprites.c src/sound.c src/statelog.c src/wadtex.c src/models.c $(TESS_SRC)
HDRS = $(wildcard src/*.h) $(wildcard $(TESS_DIR)/Source/*.h) $(TESS_DIR)/Include/tesselator.h

CFLAGS  = -O2 -Wall -Wno-unused-result -std=c11 \
          -I$(RAYLIB_SRC)/src -I$(TESS_DIR)/Include -Isrc \
          -DGL_SILENCE_DEPRECATION
LDFLAGS = $(RAYLIB_A) -lm \
          -framework OpenGL -framework Cocoa -framework IOKit \
          -framework CoreAudio -framework CoreVideo -framework AudioToolbox \
          -framework QuartzCore

# Built as a minimal .app bundle (same pattern as ../chernobyl) so macOS
# shows the game's icon in cmd+tab/Dock; run.sh still execs the binary
# inside it directly, attached to the terminal.
APP   = Helldrop.app
BIN   = $(APP)/Contents/MacOS/helldrop
ICNS  = $(APP)/Contents/Resources/icon.icns
PLIST = $(APP)/Contents/Info.plist
ICON_MASTER = assets/icon.png

# Freedoom (BSD-licensed, Doom-compatible IWAD) is the bundled default
# level data; owners of the real DOOM.WAD can pass --wad. Fetched once.
FREEDOOM_VERSION = 0.13.0
WAD = assets/freedoom1.wad

# The Helldrop asset pack: our own redistributable WAD, assembled from the
# Freedoom base plus the override lumps committed under assets/helldrop/.
# Built by tools/mkwad (reuses the engine's wad + doomgfx decoders). Unlike
# an id IWAD this is legal to ship; the loader prefers it over raw Freedoom.
MKWAD        = tools/mkwad
HELLDROP_WAD = assets/helldrop.wad
HELLDROP_SRC = $(wildcard assets/helldrop/*.txt assets/helldrop/*.lmp assets/helldrop/graphics/*.png)

game: $(BIN) $(ICNS) $(PLIST) $(WAD) $(HELLDROP_WAD)

$(APP)/Contents/MacOS:
	mkdir -p $@

$(APP)/Contents/Resources:
	mkdir -p $@

$(BIN): $(SRC) $(HDRS) $(RAYLIB_A) | $(APP)/Contents/MacOS
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $(BIN)

$(PLIST): | $(APP)/Contents/MacOS
	@echo '<?xml version="1.0" encoding="UTF-8"?>' > $@
	@echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' >> $@
	@echo '<plist version="1.0"><dict>' >> $@
	@echo '  <key>CFBundleName</key><string>Helldrop</string>' >> $@
	@echo '  <key>CFBundleDisplayName</key><string>Helldrop</string>' >> $@
	@echo '  <key>CFBundleIdentifier</key><string>com.helldrop.game</string>' >> $@
	@echo '  <key>CFBundleVersion</key><string>0.1.0</string>' >> $@
	@echo '  <key>CFBundleShortVersionString</key><string>0.1.0</string>' >> $@
	@echo '  <key>CFBundleExecutable</key><string>helldrop</string>' >> $@
	@echo '  <key>CFBundlePackageType</key><string>APPL</string>' >> $@
	@echo '  <key>CFBundleIconFile</key><string>icon</string>' >> $@
	@echo '  <key>NSHighResolutionCapable</key><true/>' >> $@
	@echo '  <key>LSMinimumSystemVersion</key><string>11.0</string>' >> $@
	@echo '</dict></plist>' >> $@

$(ICNS): $(ICON_MASTER) | $(APP)/Contents/Resources
	rm -rf /tmp/helldrop_icon.iconset
	mkdir -p /tmp/helldrop_icon.iconset
	sips -z 16 16     $(ICON_MASTER) --out /tmp/helldrop_icon.iconset/icon_16x16.png      >/dev/null
	sips -z 32 32     $(ICON_MASTER) --out /tmp/helldrop_icon.iconset/icon_16x16@2x.png   >/dev/null
	sips -z 32 32     $(ICON_MASTER) --out /tmp/helldrop_icon.iconset/icon_32x32.png      >/dev/null
	sips -z 64 64     $(ICON_MASTER) --out /tmp/helldrop_icon.iconset/icon_32x32@2x.png   >/dev/null
	sips -z 128 128   $(ICON_MASTER) --out /tmp/helldrop_icon.iconset/icon_128x128.png    >/dev/null
	sips -z 256 256   $(ICON_MASTER) --out /tmp/helldrop_icon.iconset/icon_128x128@2x.png >/dev/null
	sips -z 256 256   $(ICON_MASTER) --out /tmp/helldrop_icon.iconset/icon_256x256.png    >/dev/null
	sips -z 512 512   $(ICON_MASTER) --out /tmp/helldrop_icon.iconset/icon_256x256@2x.png >/dev/null
	sips -z 512 512   $(ICON_MASTER) --out /tmp/helldrop_icon.iconset/icon_512x512.png    >/dev/null
	sips -z 1024 1024 $(ICON_MASTER) --out /tmp/helldrop_icon.iconset/icon_512x512@2x.png >/dev/null
	iconutil -c icns /tmp/helldrop_icon.iconset -o $(ICNS)
	@rm -rf /tmp/helldrop_icon.iconset

$(RAYLIB_SRC):
	@mkdir -p vendor
	@echo "==> Cloning raylib $(RAYLIB_TAG) (first run only)"
	git clone --depth 1 --branch $(RAYLIB_TAG) https://github.com/raysan5/raylib.git $@

# raylib's Makefile drops libraylib.a in its src dir; copy to a suffixed
# name so other-platform builds added later can't clobber it.
$(RAYLIB_A): | $(RAYLIB_SRC)
	@echo "==> Building static raylib $(RAYLIB_TAG) (first run only, ~1 min)"
	cd $(RAYLIB_SRC)/src && $(MAKE) PLATFORM=PLATFORM_DESKTOP -B
	cp $(RAYLIB_SRC)/src/libraylib.a $(RAYLIB_A)
	@rm -f $(RAYLIB_SRC)/src/libraylib.a

$(WAD):
	@mkdir -p assets
	@echo "==> Fetching Freedoom $(FREEDOOM_VERSION) (first run only, ~25MB)"
	curl -sSL https://github.com/freedoom/freedoom/releases/download/v$(FREEDOOM_VERSION)/freedoom-$(FREEDOOM_VERSION).zip -o /tmp/freedoom-dl.zip
	unzip -p /tmp/freedoom-dl.zip freedoom-$(FREEDOOM_VERSION)/freedoom1.wad > $(WAD)
	@rm -f /tmp/freedoom-dl.zip

# WAD assembler: links the engine's own wad + doomgfx decoders so the patch
# codec and palette logic stay in one place. Headless (CPU-only raylib).
$(MKWAD): tools/mkwad.c src/wad.c src/doomgfx.c $(RAYLIB_A) $(HDRS)
	$(CC) $(CFLAGS) tools/mkwad.c src/wad.c src/doomgfx.c $(LDFLAGS) -o $(MKWAD)

$(HELLDROP_WAD): $(MKWAD) $(WAD) $(HELLDROP_SRC)
	$(MKWAD) build $(WAD) assets/helldrop $(HELLDROP_WAD)

# Re-derive the tinted HUD source art from the current Freedoom base.
# Run once after a Freedoom version bump; the PNGs are committed.
bake-hud: $(MKWAD) $(WAD)
	@mkdir -p assets/helldrop/graphics
	$(MKWAD) bake $(WAD) assets/helldrop/graphics

# Optional: Doom 2 assets for the heavy bestiary's sprites/sounds.
# Usage: make assets/freedoom2.wad, then --wad assets/freedoom2.wad --map MAP01
assets/freedoom2.wad:
	@mkdir -p assets
	curl -sSL https://github.com/freedoom/freedoom/releases/download/v$(FREEDOOM_VERSION)/freedoom-$(FREEDOOM_VERSION).zip -o /tmp/freedoom-dl.zip
	unzip -p /tmp/freedoom-dl.zip freedoom-$(FREEDOOM_VERSION)/freedoom2.wad > assets/freedoom2.wad
	@rm -f /tmp/freedoom-dl.zip

# ─── Web build (Emscripten / WebAssembly) ──────────────────────────────────
# Same single-source raylib tree (RAYLIB_TAG), built a second time for
# PLATFORM_WEB into libraylib.web.a (coexists with the macOS .a). The whole
# game is portable C — no platform .m files — so it cross-compiles unchanged;
# main.c swaps its loop for emscripten_set_main_loop under PLATFORM_WEB.
# Requires emscripten on PATH (emcc/emmake). Then:
#   make web          # builds dist-web/index.{html,js,wasm,data}
#   make web-serve    # builds, then serves dist-web/ on http://localhost:8000/
# The browser WAD is baked into the .data via --preload-file, so
# the browser build is self-contained (Freedoom-based, legal to ship).
WEBDIR       = dist-web
MODEL_GLBS   = $(wildcard assets/models/*.glb)
RAYLIB_WEB_A = $(RAYLIB_SRC)/src/libraylib.web.a
WEBSHELL     = web/shell.html
WEB_WAD      = build/helldrop-web.wad
WEB_MODELS   = $(WEBDIR)/models.pack

# GRAPHICS_API_OPENGL_ES3 pairs with WebGL 2 (the alpha-test sprite shader has
# a matching GLSL ES 3.00 variant). USE_GLFW=3 is raylib's web windowing.
WEBCFLAGS = -O2 -Wall -Wno-unused-result -std=c11 \
            -I$(RAYLIB_SRC)/src -I$(TESS_DIR)/Include -Isrc \
            -DPLATFORM_WEB -DGRAPHICS_API_OPENGL_ES3
# ASYNCIFY is required by raylib's web audio backend: miniaudio spins on
# emscripten_sleep() while the WebAudio device initialises (and rcore_web does
# the same on context loss). The render loop itself stays rAF-driven, so we do
# NOT call SetTargetFPS on web (that would add a per-frame asyncify sleep).
WEBLDFLAGS = -s USE_GLFW=3 \
             -s MIN_WEBGL_VERSION=2 -s MAX_WEBGL_VERSION=2 \
             -s FORCE_FILESYSTEM=1 \
             -s ALLOW_MEMORY_GROWTH=1 \
             -s INITIAL_MEMORY=268435456 \
             -s STACK_SIZE=1048576 \
             -s ASYNCIFY \
             -s EXPORTED_RUNTIME_METHODS=FS \
             --preload-file $(WEB_WAD)@assets/helldrop.wad \
             --shell-file $(WEBSHELL)

web: $(WEBDIR)/index.html $(WEB_MODELS)
	@echo "Built $(WEBDIR)/index.html — serve with: make web-serve"

$(WEBDIR):
	mkdir -p $(WEBDIR)

# raylib's web build emits libraylib.web.a directly (no clobber of the .mac.a).
$(RAYLIB_WEB_A): | $(RAYLIB_SRC)
	@command -v emcc >/dev/null || { echo "error: emcc not on PATH — activate emscripten (emsdk_env.sh)"; exit 1; }
	@echo "==> Building raylib $(RAYLIB_TAG) for web (first run only, ~1 min)"
	cd $(RAYLIB_SRC)/src && emmake $(MAKE) PLATFORM=PLATFORM_WEB GRAPHICS=GRAPHICS_API_OPENGL_ES3 -B

web-raylib: $(RAYLIB_WEB_A)

$(WEB_WAD): tools/web_assets.py $(HELLDROP_WAD)
	mkdir -p $(@D)
	python3 $< wad $(HELLDROP_WAD) $@

$(WEB_MODELS): tools/web_assets.py $(MODEL_GLBS) | $(WEBDIR)
	python3 $< models assets/models $@

$(WEBDIR)/index.html: $(SRC) $(HDRS) $(RAYLIB_WEB_A) $(WEBSHELL) $(WEB_WAD) $(WEB_MODELS) | $(WEBDIR)
	@command -v emcc >/dev/null || { echo "error: emcc not on PATH — activate emscripten (emsdk_env.sh)"; exit 1; }
	emcc $(SRC) $(RAYLIB_WEB_A) $(WEBCFLAGS) $(WEBLDFLAGS) -o $@
	@rev=$$(shasum $(WEBDIR)/index.js $(WEBDIR)/index.wasm $(WEBDIR)/index.data $(WEB_MODELS) | shasum | cut -c1-12); \
	  sed -i '' -e "s/HELLDROP_REV/$$rev/g" -e "s/src=index.js>/src=index.js?v=$$rev>/" -e "s/src=\"index.js\"/src=\"index.js?v=$$rev\"/" $@; \
	  echo "web build stamped v=$$rev"

web-serve: web
	@echo "Serving $(WEBDIR) at http://localhost:8000/ — Ctrl-C to stop"
	@cd $(WEBDIR) && python3 -m http.server 8000

web-clean:
	rm -rf $(WEBDIR)

clean:
	rm -rf $(APP) selftest.png logs $(MKWAD) $(WEBDIR)

distclean: clean
	rm -rf vendor assets/freedoom1.wad $(HELLDROP_WAD)

# Headless gameplay regression matrix (all E1 maps, unaided + scripted drops).
test: game
	./tools/playtest.sh

test-web: web
	node tools/web_playtest.mjs

.PHONY: game clean distclean bake-hud web web-raylib web-serve web-clean test test-web

# The native and browser targets share this WAD's training maps and Episode 1.
build/ios-%/libraylib.a: ios/build-raylib.sh ios/rcore_ios.c ios/raylib_ios_glcompat.h | $(RAYLIB_SRC)
	ios/build-raylib.sh $*

ios-project: $(WEB_WAD) | $(RAYLIB_SRC)
	xcodegen generate --spec ios/project.yml

.PHONY: ios-project

CC = clang

# Single source-of-truth raylib clone, pinned to a tag so every machine
# compiles against the same version (same pattern as ../chernobyl). Bump
# RAYLIB_TAG and `rm -rf vendor/raylib-src` to upgrade. Statically linked:
# no Homebrew raylib needed at build or run time.
RAYLIB_TAG ?= 6.0
RAYLIB_SRC ?= vendor/raylib-src
RAYLIB_A    = $(RAYLIB_SRC)/src/libraylib.mac.a

# libtess2 (vendored, SGI Free B license): robust tessellation of concave
# Doom sector polygons with holes. Compiled straight into the binary.
TESS_DIR = third_party/libtess2
TESS_SRC = $(wildcard $(TESS_DIR)/Source/*.c)

SRC  = src/main.c src/wad.c src/map.c src/levelmesh.c src/sim.c src/doomgfx.c \
       src/sprites.c src/sound.c src/statelog.c $(TESS_SRC)
HDRS = $(wildcard src/*.h) $(wildcard $(TESS_DIR)/Source/*.h) $(TESS_DIR)/Include/tesselator.h

CFLAGS  = -O2 -Wall -Wno-unused-result -std=c11 \
          -I$(RAYLIB_SRC)/src -I$(TESS_DIR)/Include -Isrc \
          -DGL_SILENCE_DEPRECATION
LDFLAGS = $(RAYLIB_A) -lm \
          -framework OpenGL -framework Cocoa -framework IOKit \
          -framework CoreAudio -framework CoreVideo -framework AudioToolbox

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

game: $(BIN) $(ICNS) $(PLIST) $(WAD)

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

# Optional: Doom 2 assets for the heavy bestiary's sprites/sounds.
# Usage: make assets/freedoom2.wad, then --wad assets/freedoom2.wad --map MAP01
assets/freedoom2.wad:
	@mkdir -p assets
	curl -sSL https://github.com/freedoom/freedoom/releases/download/v$(FREEDOOM_VERSION)/freedoom-$(FREEDOOM_VERSION).zip -o /tmp/freedoom-dl.zip
	unzip -p /tmp/freedoom-dl.zip freedoom-$(FREEDOOM_VERSION)/freedoom2.wad > assets/freedoom2.wad
	@rm -f /tmp/freedoom-dl.zip

clean:
	rm -rf $(APP) selftest.png logs

distclean: clean
	rm -rf vendor assets/freedoom1.wad

.PHONY: game clean distclean

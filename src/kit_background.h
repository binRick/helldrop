// kit_background.h - run a raylib game with no window, no keyboard focus, no Dock tile and no
// sound, while it still renders every frame. For test, screenshot and scripted runs on a machine
// someone is working at. Single header, desktop only (a no-op on iOS and in the browser).
//
//   #define KIT_BACKGROUND_IMPLEMENTATION      // in one .c file
//   #include "kit_background.h"
//   ...
//   if (quits_by_itself) kit_background(1);    // e.g. --shot, --frames, --script were given
//   SetConfigFlags(kit_window_flags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT, argc, argv));
//   InitWindow(1280, 720, "game");
//   InitAudioDevice();
//   kit_background_apply();                    // after InitWindow, InitAudioDevice and your SetTargetFPS
//   if (!kit_in_background()) DisableCursor(); // never grab the mouse from a hidden window
//
// Command line: --bg hides any run, --show shows it whatever else says. Environment:
// KIT_BACKGROUND=1 hides (handy for a game you cannot pass flags to).
// Checked on macOS 26 with raylib 6.2 (GLFW): rlReadScreenPixels() returns the same pixels as
// a visible run. A hidden window's buffer swaps do not wait for the display, so vsync no longer
// paces the loop: kit_background_apply() caps the frame rate at the monitor's refresh rate
// instead. Call SetTargetFPS(0) after it to run flat out (fine with a fixed timestep).
#ifndef KIT_BACKGROUND_H
#define KIT_BACKGROUND_H

void kit_background(int on);                                        // ask from code, before kit_window_flags
unsigned kit_window_flags(unsigned flags, int argc, char **argv);   // pass the result to SetConfigFlags
void kit_background_apply(void);                                    // after InitWindow / InitAudioDevice
int kit_in_background(void);

#endif

#ifdef KIT_BACKGROUND_IMPLEMENTATION
#include <stdlib.h>
#include <string.h>

#include "raylib.h"

#if !defined(__EMSCRIPTEN__) && !defined(PLATFORM_IOS) && !defined(__ANDROID__)
#define KIT_BG_DESKTOP 1
#else
#define KIT_BG_DESKTOP 0
#endif
#if defined(__APPLE__) && KIT_BG_DESKTOP
#include <objc/message.h>
#include <objc/runtime.h>
extern void glfwInitHint(int hint, int value) __attribute__((weak));   // weak: absent in non-GLFW raylib builds
#endif

static int kit_bg_on;

void kit_background(int on) { kit_bg_on = on; }
int kit_in_background(void) { return kit_bg_on; }

unsigned kit_window_flags(unsigned flags, int argc, char **argv) {
#if KIT_BG_DESKTOP
    const char *env = getenv("KIT_BACKGROUND");
    if (env && *env && strcmp(env, "0")) kit_bg_on = 1;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--bg")) kit_bg_on = 1;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--show")) kit_bg_on = 0;
    if (!kit_bg_on) return flags;
    // HIDDEN: no window on screen, but its framebuffer exists and can be read back.
    // UNFOCUSED: the keyboard stays where it was. ALWAYS_RUN: raylib otherwise blocks in
    // glfwWaitEvents once a window is not on screen.
    flags |= FLAG_WINDOW_HIDDEN | FLAG_WINDOW_UNFOCUSED | FLAG_WINDOW_ALWAYS_RUN;
#if defined(__APPLE__)
    // GLFW makes an unbundled program a regular Dock app while it starts unless its menu bar is
    // turned off first; without this the tile flashes in the Dock.
    if (glfwInitHint) glfwInitHint(0x00051002 /* GLFW_COCOA_MENUBAR */, 0);
#endif
#else
    (void)argc; (void)argv;
    kit_bg_on = 0;
#endif
    return flags;
}

void kit_background_apply(void) {
    if (!kit_bg_on) return;
    if (IsAudioDeviceReady()) SetMasterVolume(0.0f);   // a hidden run is silent too
    SetTargetFPS(GetMonitorRefreshRate(GetCurrentMonitor()));   // stands in for vsync, which a hidden window lacks
#if defined(__APPLE__) && KIT_BG_DESKTOP
    // an "accessory" app has no Dock tile and no place in the Cmd+Tab switcher
    id (*send)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
    void (*send_long)(id, SEL, long) = (void (*)(id, SEL, long))objc_msgSend;
    id app = send((id)objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
    send_long(app, sel_registerName("setActivationPolicy:"), 1);   // NSApplicationActivationPolicyAccessory
#endif
}
#endif

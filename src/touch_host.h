#ifndef HELLDROP_TOUCH_HOST_H
#define HELLDROP_TOUCH_HOST_H

enum { WEB_START, WEB_RETRY, WEB_CONTINUE, WEB_PICK, WEB_TOOL, WEB_BLAST, WEB_MODELS,
       WEB_PAUSE, WEB_FOLLOW, WEB_ZOOM, WEB_MUTE, WEB_DOOMCAM, WEB_INTERRUPT, IOS_AMBUSH };

// These names are also the browser's exported WASM interface.
void WebAction(int action, int value);
void WebTap(float x, float y);
void WebPan(float dx, float dy);

#ifdef PLATFORM_IOS
#include <stdbool.h>
int HelldropIOSStart(int argc, char **argv);
bool HelldropIOSFrame(void);
void TouchAddDemon(int kind, const char *name, int cost);
void TouchStatus(int step, int kind, int souls, int health, int near, int mode,
                 int damage, int level, int paused, int chain);
void TouchEpisodeStatus(int next, int ticks, int barrelCooldown, int doorCooldown);
void TouchAmbushPrice(int souls);
void TouchNotice(const char *message);
#endif
#endif

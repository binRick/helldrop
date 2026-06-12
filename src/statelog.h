#ifndef HELLDROP_STATELOG_H
#define HELLDROP_STATELOG_H

#include <stdbool.h>

// NDJSON gamestate log: one JSON object per line, written to
// logs/run-YYYYMMDD-HHMMSS.ndjson with logs/latest.ndjson symlinked to the
// live file. The log is the debugging contract for this project — it must
// always contain enough state to reconstruct what the game was doing
// (see CLAUDE.md). Records carry "type", "frame" and "t" (unix epoch
// seconds, fractional) automatically.
//
// Usage:
//   StateLogBegin("level_loaded");
//   StateLogStr("wad", path); StateLogInt("sectors", n);
//   StateLogObjBegin("camera"); StateLogFloat("x", c.x); ... StateLogObjEnd();
//   StateLogEnd();
//
// Inside an array, pass key = NULL to emit unkeyed elements. All strings
// are JSON-escaped. Every record is flushed on StateLogEnd so the log
// survives crashes mid-frame.

bool StateLogOpen(const char *dir);   // NULL -> "logs"; false if unwritable
void StateLogClose(void);
bool StateLogEnabled(void);
void StateLogSetFrame(long frame);    // stamped into subsequent records

void StateLogBegin(const char *type);
void StateLogEnd(void);

void StateLogStr(const char *key, const char *val);
void StateLogInt(const char *key, long long val);
void StateLogFloat(const char *key, double val);
void StateLogBool(const char *key, bool val);
void StateLogObjBegin(const char *key);
void StateLogObjEnd(void);
void StateLogArrBegin(const char *key);
void StateLogArrEnd(void);

#endif

// NDJSON gamestate log writer. See statelog.h for the contract.
#define _POSIX_C_SOURCE 200809L

#include "statelog.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define STATELOG_MAX_DEPTH 64

static FILE *g_file = NULL;
static long g_frame = 0;
static int g_depth = -1;                    // -1 = no record open; 0 = record root
static bool g_comma[STATELOG_MAX_DEPTH];    // comma needed before next element
static bool g_is_arr[STATELOG_MAX_DEPTH];
static int g_ignored = 0;                   // open Begin calls dropped (cap/misuse)

static void WriteJsonStr(const char *s) {
    fputc('"', g_file);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", g_file); break;
        case '\\': fputs("\\\\", g_file); break;
        case '\n': fputs("\\n", g_file); break;
        case '\t': fputs("\\t", g_file); break;
        case '\r': fputs("\\r", g_file); break;
        default:
            if (*p < 0x20) fprintf(g_file, "\\u%04x", *p);
            else fputc(*p, g_file);
        }
    }
    fputc('"', g_file);
}

// Writes the leading comma and, in object context, the key. Returns false
// (writing nothing) when the call must be dropped to keep the JSON valid.
static bool EmitPrefix(const char *key) {
    if (!g_file || g_depth < 0 || g_ignored > 0) return false;
    if (!g_is_arr[g_depth] && !key) return false;   // keyless value in object
    if (g_comma[g_depth]) fputc(',', g_file);
    g_comma[g_depth] = true;
    if (!g_is_arr[g_depth]) {                       // key inside array: ignored
        WriteJsonStr(key);
        fputc(':', g_file);
    }
    return true;
}

bool StateLogOpen(const char *dir) {
    StateLogClose();
    if (!dir) dir = "logs";
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return false;

    time_t now = time(NULL);
    struct tm tmv;
    if (!localtime_r(&now, &tmv)) return false;
    char name[48];
    if (strftime(name, sizeof name, "run-%Y%m%d-%H%M%S.ndjson", &tmv) == 0)
        return false;

    char path[1024];
    int n = snprintf(path, sizeof path, "%s/%s", dir, name);
    if (n < 0 || n >= (int)sizeof path) return false;
    g_file = fopen(path, "w");
    if (!g_file) return false;

    char link[1024];
    n = snprintf(link, sizeof link, "%s/latest.ndjson", dir);
    if (n < 0 || n >= (int)sizeof link ||
        (unlink(link) != 0 && errno != ENOENT) ||
        symlink(name, link) != 0) {                 // relative target on purpose
        fclose(g_file);
        g_file = NULL;
        unlink(path);
        return false;
    }
    g_depth = -1;
    g_ignored = 0;
    return true;
}

void StateLogClose(void) {
    if (!g_file) return;
    if (g_depth >= 0) StateLogEnd();
    fclose(g_file);
    g_file = NULL;
    g_depth = -1;
    g_ignored = 0;
}

bool StateLogEnabled(void) {
    return g_file != NULL;
}

void StateLogSetFrame(long frame) {
    g_frame = frame;
}

void StateLogBegin(const char *type) {
    if (!g_file) return;
    if (g_depth >= 0) StateLogEnd();                // unterminated previous record
    struct timeval tv;
    double t = 0.0;
    if (gettimeofday(&tv, NULL) == 0)
        t = (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
    fputs("{\"type\":", g_file);
    WriteJsonStr(type ? type : "");
    fprintf(g_file, ",\"frame\":%ld,\"t\":%.3f", g_frame, t);
    g_depth = 0;
    g_comma[0] = true;
    g_is_arr[0] = false;
    g_ignored = 0;
}

void StateLogEnd(void) {
    if (!g_file || g_depth < 0) return;
    g_ignored = 0;
    while (g_depth > 0) {
        fputc(g_is_arr[g_depth] ? ']' : '}', g_file);
        g_depth--;
    }
    fputs("}\n", g_file);
    fflush(g_file);
    g_depth = -1;
}

void StateLogStr(const char *key, const char *val) {
    if (!EmitPrefix(key)) return;
    if (val) WriteJsonStr(val);
    else fputs("null", g_file);
}

void StateLogInt(const char *key, long long val) {
    if (!EmitPrefix(key)) return;
    fprintf(g_file, "%lld", val);
}

void StateLogFloat(const char *key, double val) {
    if (!EmitPrefix(key)) return;
    if (!isfinite(val)) fputs("null", g_file);
    else if (fabs(val) >= 1e9) fprintf(g_file, "%.9g", val);
    else fprintf(g_file, "%.3f", val);
}

void StateLogBool(const char *key, bool val) {
    if (!EmitPrefix(key)) return;
    fputs(val ? "true" : "false", g_file);
}

static void BeginContainer(const char *key, bool is_arr) {
    if (!g_file || g_depth < 0) return;
    if (g_ignored > 0 || g_depth + 1 >= STATELOG_MAX_DEPTH || !EmitPrefix(key)) {
        g_ignored++;                  // keep matching End from popping a real level
        return;
    }
    fputc(is_arr ? '[' : '{', g_file);
    g_depth++;
    g_comma[g_depth] = false;
    g_is_arr[g_depth] = is_arr;
}

// Ends close whichever container is open so mismatched Obj/Arr calls
// cannot corrupt the output.
static void EndContainer(void) {
    if (!g_file || g_depth < 0) return;
    if (g_ignored > 0) {
        g_ignored--;
        return;
    }
    if (g_depth == 0) return;                       // record root: only StateLogEnd
    fputc(g_is_arr[g_depth] ? ']' : '}', g_file);
    g_depth--;
}

void StateLogObjBegin(const char *key) { BeginContainer(key, false); }
void StateLogObjEnd(void)              { EndContainer(); }
void StateLogArrBegin(const char *key) { BeginContainer(key, true); }
void StateLogArrEnd(void)              { EndContainer(); }

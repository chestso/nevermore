/* history.c - prompt history persistence (ported from ditty/cli/history.c)
 *
 * One escaped entry per line; multiline prompts ride \n escapes.
 * The ring buffer, duplicate squashing, and Up/Down navigation live
 * in boba's textinput — this module is only the durable copy.
 *
 * Memory model: escape/unescape buffers are per-call by nature
 * (translating between the textinput's string and the file's line)
 * and freed immediately; nothing accumulates.
 */

#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------- */
/* Escape / unescape                                                 */
/* ---------------------------------------------------------------- */

char *nm_history_escape(const char *line)
{
    if (!line)
        return NULL;

    size_t len = strlen(line);
    /* Worst case: every char needs escaping */
    char *result = malloc(len * 2 + 1);
    if (!result)
        return NULL;

    char *out = result;
    for (size_t i = 0; i < len; i++) {
        if (line[i] == '\n') {
            *out++ = '\\';
            *out++ = 'n';
        } else if (line[i] == '\\') {
            *out++ = '\\';
            *out++ = '\\';
        } else {
            *out++ = line[i];
        }
    }
    *out = '\0';
    return result;
}

char *nm_history_unescape(const char *escaped)
{
    if (!escaped)
        return NULL;

    size_t len = strlen(escaped);
    char *result = malloc(len + 1);
    if (!result)
        return NULL;

    char *out = result;
    for (size_t i = 0; i < len; i++) {
        if (escaped[i] == '\\' && i + 1 < len) {
            if (escaped[i + 1] == 'n') {
                *out++ = '\n';
                i++;
            } else if (escaped[i + 1] == '\\') {
                *out++ = '\\';
                i++;
            } else {
                /* Unknown escape: keep the backslash */
                *out++ = '\\';
            }
        } else {
            *out++ = escaped[i];
        }
    }
    *out = '\0';
    return result;
}

/* ---------------------------------------------------------------- */
/* Path                                                              */
/* ---------------------------------------------------------------- */

static char g_override[1024];

const char *nm_history_path(void)
{
    if (g_override[0])
        return g_override;

    static char path[4096];

#ifdef _WIN32
    /* %LOCALAPPDATA%\nevermore\history */
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0,
                                   appdata))) {
        snprintf(path, sizeof(path), "%s\\nevermore\\history", appdata);
    } else {
        const char *home = getenv("USERPROFILE");
        if (home) {
            snprintf(path, sizeof(path),
                     "%s\\AppData\\Local\\nevermore\\history", home);
        } else {
            snprintf(path, sizeof(path), "nevermore\\history");
        }
    }
#else
    /* $XDG_STATE_HOME/nevermore/history or
     * ~/.local/state/nevermore/history */
    const char *xdg_state = getenv("XDG_STATE_HOME");
    if (xdg_state && xdg_state[0] != '\0') {
        snprintf(path, sizeof(path), "%s/nevermore/history", xdg_state);
    } else {
        const char *home = getenv("HOME");
        snprintf(path, sizeof(path), "%s/.local/state/nevermore/history",
                 home && *home ? home : "/");
    }
#endif

    return path;
}

void nm_history_set_path(const char *path)
{
    if (!path || !*path) {
        g_override[0] = '\0';
        return;
    }
    snprintf(g_override, sizeof(g_override), "%s", path);
}

/* ---------------------------------------------------------------- */
/* Save / load                                                       */
/* ---------------------------------------------------------------- */

/* mkdir -p, character-level path walk (no regex, no system()). */
static int mkdir_p(const char *dir)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t len = strlen(tmp);
    if (len == 0)
        return -1;

    /* Strip trailing separators (keep the root slash). */
    while (len > 1 &&
           (tmp[len - 1] == '/' || tmp[len - 1] == '\\'))
        tmp[--len] = '\0';

    for (size_t i = 1; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\' || i == len) {
            char saved = tmp[i];
            tmp[i] = '\0';
#ifdef _WIN32
            if (GetFileAttributesA(tmp) == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryA(tmp, NULL) &&
                    GetLastError() != ERROR_ALREADY_EXISTS)
                    return -1;
            }
#else
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
#endif
            tmp[i] = saved;
        }
    }
    return 0;
}

int nm_history_load(TuiTextInput *input)
{
    if (!input)
        return 0;

    const char *path = nm_history_path();
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    int count = 0;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
#ifdef _WIN32
            if (len > 1 && line[len - 2] == '\r')
                line[len - 2] = '\0';
#endif
        }

        char *unescaped = nm_history_unescape(line);
        if (unescaped) {
            tui_textinput_history_add(input, unescaped);
            free(unescaped);
            count++;
        }
    }
    fclose(f);
    return count;
}

void nm_history_save(TuiTextInput *input)
{
    if (!input || input->history_count == 0)
        return;

    const char *path = nm_history_path();

    /* Ensure the directory exists */
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    char *bslash = strrchr(dir, '\\');
    char *sep = (slash && bslash) ? (slash > bslash ? slash : bslash)
                                  : (slash ? slash : bslash);
    if (!sep)
        return; /* no directory part: refuse to write to CWD-adjacent */
    *sep = '\0';
    if (mkdir_p(dir) != 0)
        return; /* silent fail (convenience surface) */

    FILE *f = fopen(path, "w");
    if (!f)
        return;

    /* Write oldest-first (the array is newest-first: add() prepends)
     * so a load's re-add reconstructs the same order — history
     * survives a save/load round-trip unchanged. */
    for (int i = input->history_count - 1; i >= 0; i--) {
        char *escaped = nm_history_escape(input->history[i]);
        if (escaped) {
            fprintf(f, "%s\n", escaped);
            free(escaped);
        }
    }
    fclose(f);
}

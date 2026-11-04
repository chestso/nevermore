/* mention_source_w32.c - the directory read on Windows
 * (mention_source_internal.h's seam; the POSIX half is
 * mention_source_posix.c, selected by src/Makefile.am).
 *
 * FindFirstFileW / FindNextFileW, keyed on UTF-16: a project may hold a
 * name that is not in the ANSI code page, and every caller here has
 * already produced UTF-8 (the prompt's bytes). The conversions are the
 * boundary, not a refactor.
 *
 * A reparse point is remembered as a link but reported by its TARGET's
 * kind, the same rule as the POSIX half's stat-vs-lstat: a directory
 * junction completes with a '/'. Every entry the WIN32_FIND_DATAW
 * already carries — the directory bit, the size — costs no second call.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "mention_source.h"
#include "mention_source_internal.h"

/* UTF-8 -> a fresh UTF-16 string (NULL on OOM or a bad sequence). */
static wchar_t *to_wide(const char *s)
{
    if (!s)
        return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (!w)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
}

/* UTF-16 -> a fresh UTF-8 string (NULL on OOM). */
static char *to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    char *s = malloc((size_t)n);
    if (!s)
        return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) != n) {
        free(s);
        return NULL;
    }
    return s;
}

void nm_mention_os_free_raw(NmMentionRaw *v, size_t n)
{
    if (!v)
        return;
    for (size_t i = 0; i < n; i++)
        free(v[i].name);
    free(v);
}

int nm_mention_os_read_dir(const char *abs, NmMentionRaw **out, size_t *n,
                           int *truncated)
{
    if (out)
        *out = NULL;
    if (n)
        *n = 0;
    if (truncated)
        *truncated = 0;
    if (!abs)
        return 0;

    /* The search pattern is the directory plus "/*"; FindFirstFileW
     * takes '/' in a path as readily as '\\'. */
    size_t al = strlen(abs);
    char *pattern = malloc(al + 3);
    if (!pattern)
        return -1;
    memcpy(pattern, abs, al);
    int slash = !(al > 0 && (abs[al - 1] == '/' || abs[al - 1] == '\\'));
    size_t at = al;
    if (slash)
        pattern[at++] = '/';
    pattern[at++] = '*';
    pattern[at] = '\0';
    wchar_t *wpat = to_wide(pattern);
    free(pattern);
    if (!wpat)
        return -1;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    free(wpat);
    if (h == INVALID_HANDLE_VALUE)
        return 0; /* does not exist / unreadable: the no-match state */

    NmMentionRaw *v = NULL;
    size_t cnt = 0, cap = 0;
    for (;;) {
        if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
            if (cnt >= NM_MENTION_MAX_ENTRIES) {
                if (truncated)
                    *truncated = 1;
                break;
            }
            if (cnt == cap) {
                size_t ncap = cap ? cap * 2 : 32;
                NmMentionRaw *nv = realloc(v, ncap * sizeof *nv);
                if (!nv)
                    break; /* OOM: keep the names already read */
                v = nv;
                cap = ncap;
            }
            char *name = to_utf8(fd.cFileName);
            if (!name)
                break;
            v[cnt].name = name;
            int dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            v[cnt].is_dir = dir ? 1 : 0;
            v[cnt].is_link =
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
            v[cnt].size = dir ? -1
                              : (long long)(((unsigned long long)
                                                 fd.nFileSizeHigh
                                             << 32) |
                                            fd.nFileSizeLow);
            cnt++;
        }
        if (!FindNextFileW(h, &fd))
            break;
    }
    FindClose(h);
    if (out)
        *out = v;
    else
        nm_mention_os_free_raw(v, cnt);
    if (n)
        *n = cnt;
    return 1;
}

int nm_mention_os_kind(const char *abs, long long *size)
{
    if (size)
        *size = -1;
    wchar_t *w = to_wide(abs);
    if (!w)
        return 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int kind = 0;
    if (GetFileAttributesExW(w, GetFileExInfoStandard, &fa)) {
        if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            kind = 2;
        } else {
            kind = 1;
            if (size)
                *size = (long long)(((unsigned long long)fa.nFileSizeHigh << 32) |
                                    fa.nFileSizeLow);
        }
    }
    free(w);
    return kind;
}

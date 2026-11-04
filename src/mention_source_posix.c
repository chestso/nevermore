/* mention_source_posix.c - the directory read on POSIX
 * (mention_source_internal.h's seam; the Windows half is
 * mention_source_w32.c, selected by src/Makefile.am).
 *
 * opendir/readdir for the names, stat for what each one is (the dir bit
 * follows a symlink, so a link to a directory completes with a '/'),
 * lstat only to remember that it WAS a link. A path that cannot be
 * stat'd (a dangling link, a file removed between the read and the
 * stat) is still a name: the user typed it, and it may exist again by
 * the time they press Enter.
 */

#include "mention_source.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mention_source_internal.h"

/* "dir" + "/" + "name", never a doubled separator. malloc'd. */
static char *join_path(const char *dir, const char *name)
{
    size_t dl = strlen(dir), nl = strlen(name);
    int slash = !(dl > 0 && dir[dl - 1] == '/');
    char *out = malloc(dl + (size_t)slash + nl + 1);
    if (!out)
        return NULL;
    memcpy(out, dir, dl);
    size_t at = dl;
    if (slash)
        out[at++] = '/';
    memcpy(out + at, name, nl + 1);
    return out;
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
    DIR *d = opendir(abs);
    if (!d)
        return 0;

    NmMentionRaw *v = NULL;
    size_t cnt = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
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
        char *full = join_path(abs, e->d_name);
        char *name = strdup(e->d_name);
        if (!full || !name) {
            free(full);
            free(name);
            break;
        }
        v[cnt].name = name;
        v[cnt].is_dir = 0;
        v[cnt].is_link = 0;
        v[cnt].size = -1;
        struct stat st;
        if (stat(full, &st) == 0) {
            v[cnt].is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            if (!v[cnt].is_dir && S_ISREG(st.st_mode))
                v[cnt].size = (long long)st.st_size;
        }
        struct stat lst;
        if (lstat(full, &lst) == 0 && S_ISLNK(lst.st_mode))
            v[cnt].is_link = 1;
        free(full);
        cnt++;
    }
    closedir(d);
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
    struct stat st;
    if (!abs || stat(abs, &st) != 0)
        return 0;
    if (S_ISDIR(st.st_mode))
        return 2;
    if (S_ISREG(st.st_mode) && size)
        *size = (long long)st.st_size;
    return 1;
}

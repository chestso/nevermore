/* nm_config.c - central configuration: user file + runtime shadow layer
 *
 * See nm_config.h for the contract, the precedence table and the grammar.
 * Character-level scans throughout (the authinfo.c tokenizer family):
 * no regex, no quoting, no per-line allocation.
 *
 * Memory model: the file bytes are read into one reused scratch buffer
 * (bounded: a config larger than NM_CONFIG_MAX is a misconfiguration);
 * values live in a fixed per-key slot table rebuilt per load. The
 * rewrite serializes into one reused buffer (grown geometrically, never
 * per change) and writes tmp + rename, so a crash can never leave a
 * half-written shadow.
 *
 * The key table lives in the NmConfig object; one config per process
 * (main.c loads exactly one, tests load one per test).
 */

#include "nm_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* 16 KiB cap: a config file larger than that is a misconfiguration.
 * Truncation lands inside a line, which the scanner reads as the end
 * of input — silent and safe. */
#define NM_CONFIG_MAX  16384
#define NM_CONFIG_LINE 2048
#define NM_CONFIG_VAL  1024
#define NM_CONFIG_PATH 4096

#define NM_CFG_NKEYS 4

typedef struct
{
    const char *name;
    const char *env;
    char user[NM_CONFIG_VAL];
    char shadow[NM_CONFIG_VAL];
    char env_v[NM_CONFIG_VAL];
    char cli[NM_CONFIG_VAL];
} CfgKey;

struct NmConfig
{
    CfgKey keys[NM_CFG_NKEYS];
    char user_path[NM_CONFIG_PATH];
    char shadow_path[NM_CONFIG_PATH];
    int user_present;
    int shadow_count;
    /* Reused rewrite scratch (serialization). */
    char *out;
    size_t out_cap;
};

/* Path overrides (test seam). Process-global: the paths are a process
 * property, and a test overrides them before nm_config_load. */
static char g_user_override[NM_CONFIG_PATH];
static char g_shadow_override[NM_CONFIG_PATH];

/* ---------------------------------------------------------------- */
/* Key table / validation                                            */
/* ---------------------------------------------------------------- */

static void init_keys(NmConfig *c)
{
    static const struct
    {
        const char *name;
        const char *env;
    } defs[NM_CFG_NKEYS] = {
        { NM_CFG_KEY_PROVIDER, "NEVERMORE_PROVIDER" },
        { NM_CFG_KEY_MODEL, "NEVERMORE_MODEL" },
        { NM_CFG_KEY_ROUNDS, "NEVERMORE_MAX_ROUNDS" },
        { NM_CFG_KEY_REASONING, "NEVERMORE_ECHO_REASONING" },
    };
    for (int i = 0; i < NM_CFG_NKEYS; i++) {
        CfgKey *k = &c->keys[i];
        memset(k, 0, sizeof(*k));
        k->name = defs[i].name;
        k->env = defs[i].env;
    }
}

static CfgKey *key_by_name(NmConfig *c, const char *name)
{
    if (!c || !name || !*name)
        return NULL;
    for (int i = 0; i < NM_CFG_NKEYS; i++) {
        if (strcmp(c->keys[i].name, name) == 0)
            return &c->keys[i];
    }
    return NULL;
}

/* The provider-name validator: installed by main.c (the registry is a
 * different TU, and the unit test must not have to link it). No hook =
 * any non-empty name. */
static int (*g_provider_validator)(const char *name);

void nm_config_set_provider_validator(int (*fn)(const char *name))
{
    g_provider_validator = fn;
}

int nm_config_valid_provider(const char *name)
{
    if (!name || !*name)
        return 0;
    if (g_provider_validator)
        return g_provider_validator(name);
    return 1;
}

int nm_config_valid_rounds(const char *value)
{
    if (!value || !*value)
        return 0;
    int v = 0;
    for (const char *p = value; *p; p++) {
        if (*p < '0' || *p > '9' || v > 100000)
            return 0;
        v = v * 10 + (*p - '0');
    }
    return v > 0;
}

int nm_config_valid_reasoning(const char *value)
{
    if (!value || !*value)
        return 0;
    char v[8];
    size_t n = 0;
    for (; value[n] && n < sizeof(v) - 1; n++) {
        char ch = value[n];
        v[n] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    v[n] = '\0';
    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
           strcmp(v, "on") == 0 || strcmp(v, "yes") == 0 ||
           strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
           strcmp(v, "off") == 0 || strcmp(v, "no") == 0;
}

/* Normalize a validated truthy spelling to "on"/"off", so the shadow
 * file, /config's view and the env layer all read the same. */
static const char *normalize_bool(const char *value)
{
    return (value[0] == '1' || value[0] == 't' || value[0] == 'T' ||
            value[0] == 'y' || value[0] == 'Y' || value[0] == 'o' ||
            value[0] == 'O')
               ? "on"
               : "off";
}

const char *nm_config_key_at(size_t i)
{
    static const char *const names[NM_CFG_NKEYS] = {
        NM_CFG_KEY_PROVIDER, NM_CFG_KEY_MODEL, NM_CFG_KEY_ROUNDS,
        NM_CFG_KEY_REASONING
    };
    return i < NM_CFG_NKEYS ? names[i] : NULL;
}

const char *nm_config_env_name(const char *key)
{
    static const char *const envs[NM_CFG_NKEYS] = {
        "NEVERMORE_PROVIDER", "NEVERMORE_MODEL", "NEVERMORE_MAX_ROUNDS",
        "NEVERMORE_ECHO_REASONING"
    };
    for (size_t i = 0; i < NM_CFG_NKEYS; i++) {
        if (key && strcmp(key, nm_config_key_at(i)) == 0)
            return envs[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Paths                                                             */
/* ---------------------------------------------------------------- */

/* $XDG_CONFIG_HOME wins, then HOME (USERPROFILE on Windows) + "/.config"
 * — context.c's chain, the same "~/.config" AGENTS.md discovery uses.
 * Empty when the home directory cannot be resolved. */
static void default_user_path(char *out, size_t cap)
{
    out[0] = '\0';
    if (g_user_override[0]) {
        snprintf(out, cap, "%s", g_user_override);
        return;
    }
    const char *env = getenv("NEVERMORE_CONFIG");
    if (env && *env) {
        snprintf(out, cap, "%s", env);
        return;
    }
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(out, cap, "%s/nevermore/config", xdg);
        return;
    }
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home)
        home = getenv("USERPROFILE");
#endif
    if (!home || !*home)
        return;
    snprintf(out, cap, "%s/.config/nevermore/config", home);
}

/* $XDG_STATE_HOME/nevermore/config or ~/.local/state/nevermore/config
 * — history.c's chain, so the shadow sits next to history/ and wire/.
 * %USERPROFILE%\AppData\Local\nevermore\config on Windows when no
 * XDG_STATE_HOME is set (no shlobj: the wire recorder's fallback
 * chain). */
static void default_shadow_path(char *out, size_t cap)
{
    out[0] = '\0';
    if (g_shadow_override[0]) {
        snprintf(out, cap, "%s", g_shadow_override);
        return;
    }
    const char *env = getenv("NEVERMORE_SHADOW_CONFIG");
    if (env && *env) {
        snprintf(out, cap, "%s", env);
        return;
    }
    /* $XDG_STATE_HOME is the documented state-dir knob on every
     * platform (history.c honors it too); a Windows session that sets
     * it is running under MSYS2, where "/" paths are what its tools
     * expect. Fall back to the native %LOCALAPPDATA% shape. */
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg && *xdg) {
        snprintf(out, cap, "%s/nevermore/config", xdg);
        return;
    }
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    if (!home || !*home)
        return;
    snprintf(out, cap, "%s\\AppData\\Local\\nevermore\\config", home);
#else
    const char *home = getenv("HOME");
    if (!home || !*home)
        return;
    snprintf(out, cap, "%s/.local/state/nevermore/config", home);
#endif
}

const char *nm_config_user_path(void)
{
    static char path[NM_CONFIG_PATH];
    default_user_path(path, sizeof(path));
    return path;
}

const char *nm_config_shadow_path(void)
{
    static char path[NM_CONFIG_PATH];
    default_shadow_path(path, sizeof(path));
    return path;
}

void nm_config_set_paths(const char *user_path, const char *shadow_path)
{
    snprintf(g_user_override, sizeof(g_user_override), "%s",
             user_path && *user_path ? user_path : "");
    snprintf(g_shadow_override, sizeof(g_shadow_override), "%s",
             shadow_path && *shadow_path ? shadow_path : "");
}

/* ---------------------------------------------------------------- */
/* Scanner (character-level)                                         */
/* ---------------------------------------------------------------- */

static int file_exists(const char *path)
{
    if (!path || !*path)
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* Read a file into `buf` (cap bytes, NUL-terminated). Returns the byte
 * count, 0 when absent/unreadable/empty. */
static size_t read_file(const char *path, char *buf, size_t cap)
{
    if (!path || !*path)
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, cap, f);
    if (ferror(f))
        n = 0;
    fclose(f);
    buf[n] = '\0';
    return n;
}

static int is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
           ch == '\f' || ch == '\v';
}

/* One config file into the given slot table. Character scan: skip
 * whitespace/comments, read `key`, expect `=`, take the rest of the
 * line trimmed as the value (verbatim — no quoting, no escapes).
 * Unknown keys, malformed lines and invalid values warn on stderr and
 * are skipped: a stale file must never brick startup. Returns the
 * number of keys accepted. */
static int scan_file(NmConfig *c, const char *path, const char *which,
                     int into_shadow)
{
    static char buf[NM_CONFIG_MAX + 1];
    size_t n = read_file(path, buf, NM_CONFIG_MAX);
    if (n == 0)
        return 0;

    int count = 0;
    const char *p = buf;
    while (*p) {
        while (*p && is_space(*p))
            p++;
        if (!*p)
            break;
        if (*p == '#') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        const char *key_start = p;
        while (*p && *p != '=' && *p != '\n')
            p++;
        const char *key_end = p;
        while (key_end > key_start && is_space(key_end[-1]))
            key_end--;
        if (*p != '=') {
            fprintf(stderr,
                    "nevermore: %s: malformed line (no '='): ignored\n",
                    which);
            while (*p && *p != '\n')
                p++;
            continue;
        }
        p++; /* past '=' */
        while (*p == ' ' || *p == '\t')
            p++;
        const char *val_start = p;
        while (*p && *p != '\n')
            p++;
        const char *val_end = p;
        while (val_end > val_start && is_space(val_end[-1]))
            val_end--;

        size_t klen = (size_t)(key_end - key_start);
        size_t vlen = (size_t)(val_end - val_start);
        if (klen == 0 || klen >= NM_CONFIG_LINE || vlen >= NM_CONFIG_VAL) {
            fprintf(stderr, "nevermore: %s: over-long line: ignored\n",
                    which);
            continue;
        }
        char key[NM_CONFIG_LINE];
        memcpy(key, key_start, klen);
        key[klen] = '\0';
        char val[NM_CONFIG_VAL];
        memcpy(val, val_start, vlen);
        val[vlen] = '\0';

        CfgKey *k = key_by_name(c, key);
        if (!k) {
            fprintf(stderr,
                    "nevermore: %s: unknown key '%s' (keys: provider, "
                    "model, rounds, reasoning): ignored\n",
                    which, key);
            continue;
        }
        int ok;
        if (strcmp(k->name, NM_CFG_KEY_PROVIDER) == 0)
            ok = nm_config_valid_provider(val);
        else if (strcmp(k->name, NM_CFG_KEY_ROUNDS) == 0)
            ok = nm_config_valid_rounds(val);
        else if (strcmp(k->name, NM_CFG_KEY_REASONING) == 0)
            ok = nm_config_valid_reasoning(val);
        else
            ok = 1; /* model: any non-empty id (a local daemon may serve
                     * private ids the static catalog does not know) */
        if (!ok) {
            fprintf(stderr, "nevermore: %s: %s: invalid value '%s': "
                            "ignored\n",
                    which, k->name, val);
            continue;
        }
        char *slot = into_shadow ? k->shadow : k->user;
        if (strcmp(k->name, NM_CFG_KEY_REASONING) == 0)
            snprintf(slot, NM_CONFIG_VAL, "%s", normalize_bool(val));
        else
            snprintf(slot, NM_CONFIG_VAL, "%s", val);
        count++;
    }
    return count;
}

/* ---------------------------------------------------------------- */
/* Load / free                                                       */
/* ---------------------------------------------------------------- */

NmConfig *nm_config_load(void)
{
    NmConfig *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    init_keys(c);
    snprintf(c->user_path, sizeof(c->user_path), "%s", nm_config_user_path());
    snprintf(c->shadow_path, sizeof(c->shadow_path), "%s",
             nm_config_shadow_path());

    scan_file(c, c->user_path, "config", 0);
    c->shadow_count = scan_file(c, c->shadow_path, "shadow", 1);
    c->user_present = file_exists(c->user_path);
    return c;
}

void nm_config_free(NmConfig *c)
{
    if (!c)
        return;
    free(c->out);
    free(c);
}

/* ---------------------------------------------------------------- */
/* Resolution                                                        */
/* ---------------------------------------------------------------- */

static const char *slot_for(NmCfgSource s, const CfgKey *k)
{
    switch (s) {
    case NM_CFG_CLI:
        return k->cli;
    case NM_CFG_ENV:
        return k->env_v;
    case NM_CFG_SHADOW:
        return k->shadow;
    case NM_CFG_USER:
        return k->user;
    default:
        return "";
    }
}

NmCfgSource nm_config_source(const NmConfig *c, const char *key)
{
    CfgKey *k = key_by_name((NmConfig *)c, key);
    if (!k)
        return NM_CFG_DEFAULT;
    if (k->cli[0])
        return NM_CFG_CLI;
    if (k->env_v[0])
        return NM_CFG_ENV;
    if (k->shadow[0])
        return NM_CFG_SHADOW;
    if (k->user[0])
        return NM_CFG_USER;
    return NM_CFG_DEFAULT;
}

const char *nm_config_get(const NmConfig *c, const char *key)
{
    CfgKey *k = key_by_name((NmConfig *)c, key);
    if (!k)
        return NULL;
    NmCfgSource s = nm_config_source(c, key);
    if (s == NM_CFG_DEFAULT)
        return NULL;
    const char *v = slot_for(s, k);
    return *v ? v : NULL;
}

int nm_config_get_bool(const NmConfig *c, const char *key, int fallback)
{
    const char *v = nm_config_get(c, key);
    if (!v || !*v)
        return fallback;
    return strcmp(v, "on") == 0 || strcmp(v, "1") == 0 ||
           strcmp(v, "true") == 0 || strcmp(v, "yes") == 0;
}

int nm_config_get_int(const NmConfig *c, const char *key, int fallback)
{
    const char *v = nm_config_get(c, key);
    if (!v || !*v || !nm_config_valid_rounds(v))
        return fallback;
    int n = 0;
    for (const char *p = v; *p; p++)
        n = n * 10 + (*p - '0');
    return n > 100000 ? 100000 : n;
}

const char *nm_config_source_name(NmCfgSource s)
{
    switch (s) {
    case NM_CFG_CLI:
        return "command line";
    case NM_CFG_ENV:
        return "env";
    case NM_CFG_SHADOW:
        return "session shadow";
    case NM_CFG_USER:
        return "user config";
    default:
        return "built-in default";
    }
}

int nm_config_shadow_count(const NmConfig *c)
{
    return c ? c->shadow_count : 0;
}

int nm_config_user_present(const NmConfig *c)
{
    return c ? c->user_present : 0;
}

/* ---------------------------------------------------------------- */
/* Environment + CLI layers                                          */
/* ---------------------------------------------------------------- */

void nm_config_set_env(NmConfig *c)
{
    if (!c)
        return;
    for (int i = 0; i < NM_CFG_NKEYS; i++) {
        CfgKey *k = &c->keys[i];
        k->env_v[0] = '\0';
        const char *v = getenv(k->env);
        if (!v || !*v)
            continue;
        /* Validated exactly like the file layer: a typo can never
         * silently enable, disable or repoint anything. */
        if (strcmp(k->name, NM_CFG_KEY_PROVIDER) == 0) {
            if (!nm_config_valid_provider(v))
                continue;
            snprintf(k->env_v, NM_CONFIG_VAL, "%s", v);
        } else if (strcmp(k->name, NM_CFG_KEY_ROUNDS) == 0) {
            if (!nm_config_valid_rounds(v))
                continue;
            snprintf(k->env_v, NM_CONFIG_VAL, "%s", v);
        } else if (strcmp(k->name, NM_CFG_KEY_REASONING) == 0) {
            if (!nm_config_valid_reasoning(v))
                continue;
            snprintf(k->env_v, NM_CONFIG_VAL, "%s", normalize_bool(v));
        } else {
            snprintf(k->env_v, NM_CONFIG_VAL, "%s", v);
        }
    }
}

void nm_config_set_cli(NmConfig *c, const char *key, const char *value)
{
    CfgKey *k = key_by_name(c, key);
    if (!k)
        return;
    snprintf(k->cli, NM_CONFIG_VAL, "%s", value && *value ? value : "");
}

/* ---------------------------------------------------------------- */
/* Shadow write-back                                                 */
/* ---------------------------------------------------------------- */

/* mkdir -p, character-level path walk (history.c's pattern). */
static int mkdir_p(const char *dir)
{
    char tmp[NM_CONFIG_PATH];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t len = strlen(tmp);
    if (len == 0)
        return -1;
    while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\'))
        tmp[--len] = '\0';
    for (size_t i = 1; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\' || i == len) {
            char saved = tmp[i];
            tmp[i] = '\0';
#ifdef _WIN32
            DWORD attrs = GetFileAttributesA(tmp);
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryA(tmp, NULL) &&
                    GetLastError() != ERROR_ALREADY_EXISTS)
                    return -1;
            } else if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                return -1; /* a file is in the way: not our directory */
            }
#else
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            /* EEXIST can also mean a FILE with that name. */
            struct stat st;
            if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode))
                return -1;
#endif
            tmp[i] = saved;
        }
    }
    return 0;
}

/* Serialize the shadow layer into c->out (grown geometrically, reused
 * across writes: no per-change churn). Empty result = no keys. */
static void shadow_render(NmConfig *c)
{
    size_t need = 1;
    for (int i = 0; i < NM_CFG_NKEYS; i++)
        need += strlen(c->keys[i].name) + strlen(c->keys[i].shadow) + 8;
    if (need > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 256;
        while (cap < need)
            cap *= 2;
        char *p = realloc(c->out, cap);
        if (!p)
            return;
        c->out = p;
        c->out_cap = cap;
    }
    char *o = c->out;
    for (int i = 0; i < NM_CFG_NKEYS; i++) {
        if (!c->keys[i].shadow[0])
            continue;
        int n = snprintf(o, c->out_cap - (size_t)(o - c->out), "%s = %s\n",
                         c->keys[i].name, c->keys[i].shadow);
        if (n > 0)
            o += n;
    }
    *o = '\0';
}

/* Write the shadow file (tmp + rename; removed when no key remains).
 * Returns 0 on success, -1 when there is no path or the write failed. */
static int shadow_flush(NmConfig *c)
{
    if (!c->shadow_path[0])
        return -1;
    shadow_render(c);
    if (!c->out || !c->out[0]) {
#ifdef _WIN32
        DeleteFileA(c->shadow_path);
#else
        remove(c->shadow_path);
#endif
        return 0;
    }
    char dir[NM_CONFIG_PATH];
    snprintf(dir, sizeof(dir), "%s", c->shadow_path);
    char *sep = strrchr(dir, '/');
    char *bslash = strrchr(dir, '\\');
    char *d = (sep && bslash) ? (sep > bslash ? sep : bslash)
                              : (sep ? sep : bslash);
    if (d) {
        *d = '\0';
        if (dir[0] && mkdir_p(dir) != 0)
            return -1;
    }

    char tmp[NM_CONFIG_PATH + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", c->shadow_path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return -1;
    size_t len = strlen(c->out);
    int ok = fwrite(c->out, 1, len, f) == len && fflush(f) == 0;
    if (fclose(f) != 0)
        ok = 0;
    if (!ok) {
        remove(tmp);
        return -1;
    }
#ifdef _WIN32
    if (!MoveFileExA(tmp, c->shadow_path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        return -1;
    }
#else
    if (rename(tmp, c->shadow_path) != 0) {
        remove(tmp);
        return -1;
    }
#endif
    return 0;
}

int nm_config_shadow_set(NmConfig *c, const char *key, const char *value)
{
    CfgKey *k = key_by_name(c, key);
    if (!k)
        return -1;
    if (!value || !*value)
        return nm_config_shadow_reset(c, key);

    char norm[NM_CONFIG_VAL];
    if (strcmp(k->name, NM_CFG_KEY_REASONING) == 0) {
        if (!nm_config_valid_reasoning(value))
            return -1;
        snprintf(norm, sizeof(norm), "%s", normalize_bool(value));
    } else if (strcmp(k->name, NM_CFG_KEY_ROUNDS) == 0) {
        if (!nm_config_valid_rounds(value))
            return -1;
        snprintf(norm, sizeof(norm), "%s", value);
    } else if (strcmp(k->name, NM_CFG_KEY_PROVIDER) == 0) {
        if (!nm_config_valid_provider(value))
            return -1;
        snprintf(norm, sizeof(norm), "%s", value);
    } else {
        snprintf(norm, sizeof(norm), "%s", value);
    }

    if (!k->shadow[0])
        c->shadow_count++;
    snprintf(k->shadow, NM_CONFIG_VAL, "%s", norm);
    return shadow_flush(c);
}

int nm_config_shadow_reset(NmConfig *c, const char *key)
{
    if (!c)
        return -1;
    if (!key) {
        for (int i = 0; i < NM_CFG_NKEYS; i++)
            c->keys[i].shadow[0] = '\0';
        c->shadow_count = 0;
        return shadow_flush(c);
    }
    CfgKey *k = key_by_name(c, key);
    if (!k)
        return -1;
    if (k->shadow[0])
        c->shadow_count--;
    k->shadow[0] = '\0';
    return shadow_flush(c);
}

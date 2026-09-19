/* nm_process.c - process-job registry, buffer, and dumb-terminal render
 *
 * Platform-neutral half of the process layer (nm_process.h): the registry,
 * the bounded per-job output buffer with its omission counter, the
 * report/delta bookkeeping, and the dumb-terminal renderer.  The OS
 * half (spawn/read/write/kill/reap) is nm_process_posix.c /
 * nm_process_win.c.
 *
 * Memory-reuse: one raw buffer and one report buffer per job, both
 * grown geometrically and reused across drains and takes (the report is
 * handed out borrowed and rewritten in place on the next take).
 * Threading: only a Windows job's pipe-reader thread feeds the buffer,
 * so the append/take pair is lock-guarded there (a no-op where the
 * neutral layer is the only reader).  The lock lives here, not in the
 * OS layer, because the buffer does.
 */

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION NmProcLock;
#define PROC_LOCK_INIT(l) InitializeCriticalSection(l)
#define PROC_LOCK_FREE(l) DeleteCriticalSection(l)
#define PROC_LOCK(l)      EnterCriticalSection(l)
#define PROC_UNLOCK(l)    LeaveCriticalSection(l)
#else
typedef char NmProcLock;
#define PROC_LOCK_INIT(l) ((void)0)
#define PROC_LOCK_FREE(l) ((void)0)
#define PROC_LOCK(l)      ((void)0)
#define PROC_UNLOCK(l)    ((void)0)
#endif

#include "nm_process.h"
#include "nm_process_internal.h"

#define PROC_BUF_MAX_DEFAULT (256 * 1024) /* == SPAWN_CAPTURE_MAX */
#define PROC_REPORT_SEED     256

struct NmProc
{
    int id;
    intptr_t handle; /* loop readiness handle; -1 once exhausted */
    NmProcOs *os;    /* per-OS state (child, pipes, reader) */
    int live;        /* 1 while the child runs */
    int reaped;
    int exit_code;
    char *cmd;       /* for /ps */
    NmProcLock lock; /* guards buf/len/report_pos/dropped (see above) */
    /* Raw accumulation: bytes [report_pos, len) are unreported. */
    char *buf;
    size_t len;
    size_t cap;
    size_t report_pos;
    size_t dropped; /* unreported bytes the bounded buffer evicted */
    /* Last take_output (borrowed out; rewritten in place next take). */
    char *report;
    size_t report_cap;
};

static NmProc *g_jobs[NM_PROC_MAX_JOBS];
static int g_max_jobs = NM_PROC_MAX_JOBS;
static int g_next_id = 1;
static size_t g_buf_max = PROC_BUF_MAX_DEFAULT;

/* ---------------------------------------------------------------- */
/* Registry                                                         */
/* ---------------------------------------------------------------- */

static int registry_count(void)
{
    int n = 0;
    for (int i = 0; i < g_max_jobs; i++)
        if (g_jobs[i])
            n++;
    return n;
}

static int registry_add(NmProc *p)
{
    for (int i = 0; i < g_max_jobs; i++) {
        if (!g_jobs[i]) {
            g_jobs[i] = p;
            return 0;
        }
    }
    return -1;
}

static void registry_remove(NmProc *p)
{
    for (int i = 0; i < g_max_jobs; i++) {
        if (g_jobs[i] == p) {
            g_jobs[i] = NULL;
            return;
        }
    }
}

/* ---------------------------------------------------------------- */
/* Job lifecycle                                                */
/* ---------------------------------------------------------------- */

static void proc_free(NmProc *p)
{
    nm_proc_os_free(p->os);
    free(p->cmd);
    free(p->buf);
    free(p->report);
    PROC_LOCK_FREE(&p->lock);
    free(p);
}

NmProc *nm_proc_start(const char *cmd, const char *cwd, int *job_id,
                      char *err, size_t errsz)
{
    if (job_id)
        *job_id = -1;
    if (err && errsz)
        err[0] = '\0';
    if (!cmd || !*cmd) {
        nm_proc_set_err(err, errsz, "missing or empty cmd");
        return NULL;
    }
    if (registry_count() >= g_max_jobs) {
        char msg[64];
        snprintf(msg, sizeof(msg), "job cap of %d reached",
                 g_max_jobs);
        nm_proc_set_err(err, errsz, msg);
        return NULL;
    }

    NmProc *p = calloc(1, sizeof(*p));
    if (!p) {
        nm_proc_set_err(err, errsz, "out of memory");
        return NULL;
    }
    p->handle = -1;
    PROC_LOCK_INIT(&p->lock);

    NmProcOs *os = NULL;
    if (nm_proc_os_spawn(p, cmd, cwd, &os, err, errsz) != 0) {
        PROC_LOCK_FREE(&p->lock);
        free(p);
        return NULL;
    }

    p->id = g_next_id++;
    p->os = os;
    p->handle = nm_proc_os_handle(os);
    p->live = 1;
    p->cmd = strdup(cmd);
    if (registry_add(p) != 0) {
        /* Only reachable if the cap changed under us; be safe. */
        nm_proc_os_kill(os);
        nm_proc_os_reap(os, &p->exit_code, 1);
        proc_free(p);
        nm_proc_set_err(err, errsz, "job cap reached");
        return NULL;
    }
    if (job_id)
        *job_id = p->id;
    return p;
}

/* Reap the child if it has exited (non-blocking).  Once reaped the
 * job is no longer live and its readiness handle is retired (POSIX
 * closes the master, so a reaped job drops out of the loop's set
 * instead of polling readable-forever); the buffered output stays
 * available for a later take. */
static void try_reap(NmProc *p)
{
    if (p->reaped || !p->os)
        return;
    int code = -1;
    int r = nm_proc_os_reap(p->os, &code, 0);
    if (r == 0)
        return; /* still running */
    p->reaped = 1;
    p->live = 0;
    p->exit_code = (r == 1) ? code : -1;
    p->handle = nm_proc_os_handle(p->os);
}

void nm_proc_close(NmProc *p)
{
    if (!p)
        return;
    registry_remove(p);
    if (!p->reaped) {
        nm_proc_os_kill(p->os);
        /* Blocking reap: the child is a zombie or was just killed, so
         * waitpid/WaitForSingleObject returns at once (run_command's
         * reap rationale). */
        int code = -1;
        if (nm_proc_os_reap(p->os, &code, 1) == 1)
            p->exit_code = code;
        p->reaped = 1;
        p->live = 0;
        p->handle = -1;
    }
    proc_free(p);
}

void nm_proc_close_all(void)
{
    for (int i = 0; i < g_max_jobs; i++) {
        if (g_jobs[i])
            nm_proc_close(g_jobs[i]);
    }
}

int nm_proc_id(const NmProc *p) { return p ? p->id : -1; }
const char *nm_proc_command(const NmProc *p) { return p ? p->cmd : NULL; }
intptr_t nm_proc_handle(NmProc *p) { return p ? p->handle : -1; }

int nm_proc_source_kind(void)
{
#ifdef _WIN32
    return NM_SRC_HANDLE;
#else
    return NM_SRC_FD;
#endif
}

size_t nm_proc_buffered(const NmProc *p)
{
    if (!p)
        return 0;
    NmProc *m = (NmProc *)p; /* the lock is not part of the const view */
    PROC_LOCK(&m->lock);
    size_t n = m->len;
    PROC_UNLOCK(&m->lock);
    return n;
}

int nm_proc_live(NmProc *p)
{
    if (!p)
        return 0;
    try_reap(p);
    return p->live;
}

int nm_proc_exit(NmProc *p)
{
    if (!p)
        return -1;
    try_reap(p);
    return p->live ? -1 : p->exit_code;
}

NmProc *nm_proc_find(int job_id)
{
    for (int i = 0; i < g_max_jobs; i++) {
        if (g_jobs[i] && g_jobs[i]->id == job_id)
            return g_jobs[i];
    }
    return NULL;
}

NmProc *nm_proc_by_handle(intptr_t handle)
{
    if (handle < 0)
        return NULL;
    for (int i = 0; i < g_max_jobs; i++) {
        if (g_jobs[i] && g_jobs[i]->handle == handle)
            return g_jobs[i];
    }
    return NULL;
}

int nm_proc_count(void) { return registry_count(); }

NmProc *nm_proc_at(int i)
{
    if (i < 0)
        return NULL;
    for (int k = 0; k < g_max_jobs; k++) {
        if (!g_jobs[k])
            continue;
        if (i-- == 0)
            return g_jobs[k];
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Output buffer                                                    */
/* ---------------------------------------------------------------- */

/* Append to the bounded raw buffer, evicting the oldest bytes when it
 * is full (the newest output is what a reader wants for a spinner or a
 * tail) and counting any unreported bytes lost as omitted. */
static void buf_append(NmProc *p, const char *data, size_t n)
{
    if (n == 0 || g_buf_max == 0)
        return;
    if (n > g_buf_max) { /* a single huge chunk: keep its tail */
        size_t lose = n - g_buf_max;
        data += lose;
        n = g_buf_max;
        p->dropped += lose; /* the dropped prefix was never reported */
    }
    if (p->len + n > g_buf_max) {
        size_t evict = p->len + n - g_buf_max;
        if (p->report_pos < evict) {
            p->dropped += evict - p->report_pos;
            p->report_pos = 0;
        } else {
            p->report_pos -= evict;
        }
        memmove(p->buf, p->buf + evict, p->len - evict);
        p->len -= evict;
    }
    if (p->len + n > p->cap) {
        size_t ncap = p->cap ? p->cap : 4096;
        while (ncap < p->len + n)
            ncap *= 2;
        if (ncap > g_buf_max)
            ncap = g_buf_max;
        char *nb = realloc(p->buf, ncap);
        if (!nb) { /* OOM: drop the chunk rather than lose the job */
            p->dropped += n;
            return;
        }
        p->buf = nb;
        p->cap = ncap;
    }
    memcpy(p->buf + p->len, data, n);
    p->len += n;
}

void nm_proc_feed(NmProc *p, const char *data, size_t n)
{
    if (!p)
        return;
    PROC_LOCK(&p->lock);
    buf_append(p, data, n);
    PROC_UNLOCK(&p->lock);
}

void nm_proc_drain(NmProc *p)
{
    if (!p)
        return;
    if (p->handle >= 0) {
        /* POSIX reads here; on Windows the reader thread already fed the
         * buffer, so this is the point where the handle's liveness is
         * re-checked (a closed master / an ended reader retires it). */
        nm_proc_os_gather(p->os, p);
        p->handle = nm_proc_os_handle(p->os);
    }
    try_reap(p);
}

int nm_proc_write(NmProc *p, const char *bytes, size_t n)
{
    if (!p || !p->os)
        return -1;
    if (n == 0)
        return 0;
    size_t off = 0;
    while (off < n) {
        long w = nm_proc_os_write(p->os, bytes + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w == 0)
            break; /* would block: report the partial write */
        return off ? (int)off : -1;
    }
    return (int)off;
}

void nm_proc_write_eof(NmProc *p)
{
    if (p && p->os)
        nm_proc_os_write_eof(p->os);
}

/* ---------------------------------------------------------------- */
/* Dumb-terminal renderer                                           */
/* ---------------------------------------------------------------- */

#define RENDER_MAX_COL 65536

typedef struct
{
    char *p;
    size_t len, cap;
} RBuf;

static int rbuf_reserve(RBuf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return 0;
    size_t ncap = b->cap ? b->cap : PROC_REPORT_SEED;
    while (ncap < b->len + extra + 1)
        ncap *= 2;
    char *np = realloc(b->p, ncap);
    if (!np)
        return -1;
    b->p = np;
    b->cap = ncap;
    return 0;
}

static void rbuf_puts(RBuf *b, const char *s, size_t n)
{
    if (rbuf_reserve(b, n) != 0)
        return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void rbuf_putc(RBuf *b, char c)
{
    if (rbuf_reserve(b, 1) != 0)
        return;
    b->p[b->len++] = c;
    b->p[b->len] = '\0';
}

/* Column scratch: spaces, grown geometrically, so a cursor write at an
 * arbitrary column lands correctly. */
static int col_ensure(char **cols, size_t *cap, size_t need)
{
    if (need <= *cap)
        return 0;
    if (need > RENDER_MAX_COL)
        return -1;
    size_t ncap = *cap ? *cap : 256;
    while (ncap < need)
        ncap *= 2;
    if (ncap > RENDER_MAX_COL)
        ncap = RENDER_MAX_COL;
    char *nb = realloc(*cols, ncap);
    if (!nb)
        return -1;
    memset(nb + *cap, ' ', ncap - *cap);
    *cols = nb;
    *cap = ncap;
    return 0;
}

/* Parse a CSI sequence in line[i..n) (i points just past "ESC [").
 * On success fills *final_idx (the final byte's index) and *params
 * (*nparams entries, an omitted parameter read as 0) and returns 0;
 * returns -1 when the sequence is unterminated at end of line or
 * malformed (the caller then drops the rest of the line). */
static int csi_parse(const char *line, size_t n, size_t i, size_t *final_idx,
                     long *params, int max_params, int *nparams)
{
    long cur = 0;
    int have_cur = 0;
    int np = 0;
    for (size_t j = i; j < n; j++) {
        unsigned char c = (unsigned char)line[j];
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            have_cur = 1;
            continue;
        }
        if (c == ';' || c == ':' || c == '?') { /* separators/subparams */
            if (np < max_params)
                params[np++] = have_cur ? cur : 0;
            cur = 0;
            have_cur = 0;
            continue;
        }
        if (c >= 0x40 && c <= 0x7e) { /* the final byte */
            if (np < max_params)
                params[np++] = have_cur ? cur : 0;
            *final_idx = j;
            *nparams = np;
            return 0;
        }
        return -1; /* malformed: a stray control byte */
    }
    return -1; /* unterminated at end of line */
}

/* Collapse one newline-free line to its final visible text. */
static void render_line(RBuf *out, const char *line, size_t n)
{
    char *cols = NULL;
    size_t cols_cap = 0;
    size_t len = 0; /* highest column ever written, +1 */
    size_t col = 0;
    size_t i = 0;

    while (i < n) {
        unsigned char c = (unsigned char)line[i];

        if (c == '\r') {
            col = 0;
            i++;
            continue;
        }
        if (c == '\b') {
            if (col)
                col--;
            i++;
            continue;
        }
        if (c == '\t') {
            size_t next = (col / 8 + 1) * 8;
            while (col < next) {
                if (col_ensure(&cols, &cols_cap, col + 1) != 0) {
                    i = n;
                    break;
                }
                cols[col++] = ' ';
                if (col > len)
                    len = col;
            }
            if (i == n)
                break;
            i++;
            continue;
        }
        if (c == 0x1b) {
            if (i + 1 >= n) { /* lone ESC: drop */
                i++;
                continue;
            }
            unsigned char c2 = (unsigned char)line[i + 1];
            if (c2 == '[') {
                size_t fidx = 0;
                long params[8];
                int np = 0;
                if (csi_parse(line, n, i + 2, &fidx, params, 8, &np) != 0) {
                    i = n; /* malformed/unterminated: drop the rest */
                    break;
                }
                long p1 = np > 0 ? params[0] : 1;
                switch (line[fidx]) {
                case 'K':
                case 'J':
                {
                    long mode = np > 0 ? params[0] : 0;
                    if (mode == 0) {
                        if (len > col)
                            len = col; /* erase to end of line */
                    } else if (mode == 1) {
                        /* erase to start: spaces are real columns */
                        for (size_t k = 0; k < col && k < cols_cap; k++)
                            cols[k] = ' ';
                    } else {
                        len = 0; /* whole line */
                    }
                    break;
                }
                case 'C': /* cursor forward */
                    col += (size_t)(p1 > 0 ? p1 : 1);
                    if (col > RENDER_MAX_COL)
                        col = RENDER_MAX_COL;
                    break;
                case 'D': /* cursor back */
                    col = col > (size_t)p1 ? col - (size_t)p1 : 0;
                    break;
                case 'G': /* cursor to column P */
                    col = (size_t)(p1 > 0 ? p1 : 1);
                    col = col ? col - 1 : 0;
                    break;
                case 'H':
                case 'f':
                { /* cursor position: row 1 is this line */
                    long row = np > 0 ? params[0] : 1;
                    if (row <= 1) {
                        long cc = np > 1 ? params[1] : 1;
                        col = (size_t)(cc > 0 ? cc - 1 : 0);
                    }
                    break;
                }
                default: /* SGR and every other final: drop */
                    break;
                }
                i = fidx + 1;
                continue;
            }
            if (c2 == ']') { /* OSC: ESC ] ... BEL | ESC \ */
                size_t j = i + 2;
                int done = 0;
                while (j < n && !done) {
                    if (line[j] == '\a') {
                        done = 1;
                    } else if (line[j] == 0x1b && j + 1 < n &&
                               line[j + 1] == '\\') {
                        done = 1;
                        j++;
                    }
                    j++;
                }
                i = done ? j : n;
                continue;
            }
            i += 2; /* other two-byte escape: drop */
            continue;
        }
        if (c < 32 || c == 127) { /* remaining C0 controls / DEL: drop */
            i++;
            continue;
        }
        if (col >= RENDER_MAX_COL) { /* pathological cursor: stop */
            i = n;
            break;
        }
        if (col_ensure(&cols, &cols_cap, col + 1) != 0) {
            i = n;
            break;
        }
        cols[col++] = (char)c;
        if (col > len)
            len = col;
        i++;
    }

    /* Trailing spaces written by a tab or erase-to-start are part of
     * the visible line (quoth keeps them). */
    if (len)
        rbuf_puts(out, cols, len);
    free(cols);
}

/* Does this line carry anything the dumb-terminal pass must apply? */
static int line_needs_render(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r' || s[i] == '\b' || s[i] == '\t' || s[i] == 0x1b)
            return 1;
    }
    return 0;
}

/* Render multi-line PTY text: each line collapses to its final visible
 * text; a line free of control bytes passes through untouched and a
 * trailing partial line is preserved as-is. */
static void render_text(RBuf *out, const char *text, size_t n)
{
    size_t start = 0;
    int first = 1;
    for (size_t i = 0; i <= n; i++) {
        if (i != n && text[i] != '\n')
            continue;
        const char *seg = text + start;
        size_t segn = i - start;
        if (!first)
            rbuf_putc(out, '\n');
        first = 0;
        if (segn) {
            if (line_needs_render(seg, segn))
                render_line(out, seg, segn);
            else
                rbuf_puts(out, seg, segn);
        }
        start = i + 1;
    }
}

char *nm_proc_render(const char *text)
{
    RBuf out = { NULL, 0, 0 };
    if (text && *text)
        render_text(&out, text, strlen(text));
    if (!out.p)
        return strdup("");
    return out.p;
}

const char *nm_proc_take_output(NmProc *p)
{
    if (!p)
        return "";
    /* The report buffer is reused in place (memory-reuse); it is handed
     * out borrowed and rewritten on the next take.  The lock covers the
     * raw buffer's state, not the report (which only this thread
     * writes) — so the borrowed pointer stays valid after it is
     * dropped. */
    PROC_LOCK(&p->lock);
    RBuf out = { p->report, 0, p->report_cap };

    if (p->dropped) {
        char notice[64];
        int k = snprintf(notice, sizeof(notice), "\n... %zu bytes omitted ...\n",
                         p->dropped);
        if (k > 0)
            rbuf_puts(&out, notice, (size_t)k);
        p->dropped = 0;
    }
    if (p->len > p->report_pos)
        render_text(&out, p->buf + p->report_pos, p->len - p->report_pos);

    /* Terminate even the empty case: a reused report buffer would
     * otherwise hand back the previous take's content. */
    if (out.p && out.cap > 0)
        out.p[out.len] = '\0';

    p->report = out.p;
    p->report_cap = out.cap;

    /* Delivered: reclaim the raw buffer (its allocation is kept). */
    p->len = 0;
    p->report_pos = 0;
    PROC_UNLOCK(&p->lock);

    return out.p ? out.p : "";
}

/* ---------------------------------------------------------------- */
/* Test seams                                                       */
/* ---------------------------------------------------------------- */

void nm_proc_set_max_jobs(int n)
{
    if (n < 1)
        n = 1;
    if (n > NM_PROC_MAX_JOBS)
        n = NM_PROC_MAX_JOBS;
    g_max_jobs = n;
}

void nm_proc_set_buffer_max(size_t bytes) { g_buf_max = bytes; }

void nm_proc_reset(void)
{
    nm_proc_close_all();
    g_max_jobs = NM_PROC_MAX_JOBS;
    g_buf_max = PROC_BUF_MAX_DEFAULT;
    g_next_id = 1;
}

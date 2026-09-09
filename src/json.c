/* json.c - minimal JSON reader/writer
 *
 * Hand-rolled, zero-dependency, no regex. The reader is a single-pass
 * recursive descent parser producing a document tree; the writer is a
 * small serialization layer with string escaping. Enough for chat
 * completion requests, SSE deltas, tool-call arguments, and model
 * catalogs — nothing more, on purpose.
 *
 * Reader memory model (memory-reuse principle): all strings, keys,
 * and numbers for one parsed document live in a single append-only
 * arena owned by the root node — one malloc at parse time, one free
 * at nm_json_free time. Nodes are blocks carved out of the same
 * arena. Parsing a 2KB SSE delta is exactly two mallocs (nodes +
 * strings), freed once when the caller is done with the event.
 *
 * Writer memory model: built trees use one arena too (grown
 * geometrically); nm_json_dump serializes into one growable buffer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

/* Silences -Wunused-function on helpers kept for upcoming phases. */
#if defined(__GNUC__)
#define NM_UNUSED __attribute__((unused))
#else
#define NM_UNUSED
#endif

/* ---------------------------------------------------------------- */
/* Arena (chunked — pointers carved from it are stable for life)    */
/* ---------------------------------------------------------------- */

/* Chunks are never realloc'd (that would move every previously carved
 * pointer), so growth allocates a fresh chunk, geometrically sized.
 * Whole document: one free of the chunk list at nm_json_free time. */

#define ARENA_MIN_CAP 256

typedef struct ArenaChunk
{
    struct ArenaChunk *next;
    size_t used;
    size_t cap;
    /* data follows the header */
} ArenaChunk;

typedef struct Arena
{
    ArenaChunk *head; /* most recent chunk; list is singly linked */
} Arena;

static void *arena_alloc(Arena *a, size_t n)
{
    n = (n + 15) & ~(size_t)15; /* 16-byte alignment */
    ArenaChunk *c = a->head;
    if (!c || c->used + n > c->cap) {
        size_t cap = ARENA_MIN_CAP;
        while (cap < n)
            cap *= 2;
        c = malloc(sizeof(ArenaChunk) + cap);
        if (!c)
            return NULL;
        c->next = a->head;
        c->used = 0;
        c->cap = cap;
        a->head = c;
    }
    char *p = (char *)(c + 1) + c->used;
    c->used += n;
    return p;
}

static void arena_free(Arena *a)
{
    ArenaChunk *c = a->head;
    while (c) {
        ArenaChunk *next = c->next;
        free(c);
        c = next;
    }
    a->head = NULL;
}

/* Arena-strdup: reserved for object keys that must outlive the
 * parse buffer (phase 4 tool schemas); currently unused. */
static char NM_UNUSED *arena_strdup(Arena *a, const char *s, size_t len)
{
    char *p = arena_alloc(a, len + 1);
    if (!p)
        return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

/* ---------------------------------------------------------------- */
/* Node                                                             */
/* ---------------------------------------------------------------- */

typedef struct NmJsonMember
{
    char *key;
    struct NmJson *val;
} NmJsonMember;

struct NmJson
{
    NmJsonType type;
    int heap_owned; /* built via nm_json_new_*: strings/arrays are
                     * individual mallocs; parsed trees share an arena
                     * and free in one shot. */
    union
    {
        int boolean;
        double number;
        char *string;
        struct
        {
            NmJsonMember *members; /* object: array of key/val pairs */
            size_t len;
            size_t cap;
        } obj;
        struct
        {
            struct NmJson **items; /* array: array of node pointers */
            size_t len;
            size_t cap;
        } arr;
    } u;
    Arena arena; /* root node owns the chunk list for the whole tree */
};

NmJsonType nm_json_type(const NmJson *v)
{
    return v ? v->type : NM_JSON_NULL;
}

/* ---------------------------------------------------------------- */
/* Parser                                                           */
/* ---------------------------------------------------------------- */

typedef struct Parser
{
    const char *s;
    size_t pos;
    size_t len;
    const char **err;
    Arena *arena;
} Parser;

static NmJson *parse_value(Parser *p);

static void skip_ws(Parser *p)
{
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else
            break;
    }
}

static NmJson *new_node(Parser *p, NmJsonType t)
{
    NmJson *v = arena_alloc(p->arena, sizeof(NmJson));
    if (!v)
        return NULL;
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static char *parse_string_raw(Parser *p)
{
    /* Parses a JSON string literal (with quotes) into a heap/arena
     * string. Character-level unescaping, no regex. */
    if (p->pos >= p->len || p->s[p->pos] != '"') {
        if (p->err)
            *p->err = "expected string";
        return NULL;
    }
    p->pos++;
    size_t start = p->pos;
    size_t arena_out_len = 0;
    /* First pass: find the closing quote (unescaped) so the arena
     * slice is one alloc with a known bound. */
    int closed = 0;
    size_t i = start;
    while (i < p->len) {
        char c = p->s[i];
        if (c == '"') {
            closed = 1;
            break;
        }
        if (c == '\\')
            i++; /* skip the escaped character too */
        i++;
    }
    if (!closed) {
        if (p->err)
            *p->err = "unterminated string";
        return NULL;
    }
    /* Sizing pass counts worst-case UTF-8 bytes: a \u escape can emit
     * up to 3 bytes, any other escape exactly 1. */
    {
        size_t i = start;
        size_t out_len = 0;
        while (i < p->len && p->s[i] != '"') {
            if (p->s[i] == '\\') {
                i++;
                if (i >= p->len)
                    break;
                if (p->s[i] == 'u')
                    out_len += 3, i += 4;
                else
                    out_len += 1, i++;
            } else {
                out_len++;
                i++;
            }
        }
        arena_out_len = out_len;
    }
    size_t qend = i; /* index of the closing quote */
    char *out = arena_alloc(p->arena, arena_out_len + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    i = start;
    while (i < qend) {
        char c = p->s[i];
        if (c == '\\') {
            i++;
            if (i >= p->len)
                break;
            char e = p->s[i];
            switch (e) {
            case '"':
                out[o++] = '"';
                break;
            case '\\':
                out[o++] = '\\';
                break;
            case '/':
                out[o++] = '/';
                break;
            case 'b':
                out[o++] = '\b';
                break;
            case 'f':
                out[o++] = '\f';
                break;
            case 'n':
                out[o++] = '\n';
                break;
            case 'r':
                out[o++] = '\r';
                break;
            case 't':
                out[o++] = '\t';
                break;
            case 'u':
            {
                if (i + 4 >= p->len) {
                    if (p->err)
                        *p->err = "bad \\u escape";
                    return NULL;
                }
                int h[4];
                for (int k = 0; k < 4; k++) {
                    h[k] = hexval(p->s[i + 1 + k]);
                    if (h[k] < 0) {
                        if (p->err)
                            *p->err = "bad \\u escape";
                        return NULL;
                    }
                }
                unsigned cp = (unsigned)h[0] << 12 | (unsigned)h[1] << 8 | (unsigned)h[2] << 4 | (unsigned)h[3];
                /* UTF-8 encode (no surrogate pairs — models never emit
                 * them, and unpaired surrogates are an error anyway). */
                if (cp < 0x80) {
                    out[o++] = (char)cp;
                } else if (cp < 0x800) {
                    out[o++] = (char)(0xC0 | (cp >> 6));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[o++] = (char)(0xE0 | (cp >> 12));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                }
                i += 4;
                break;
            }
            default:
                if (p->err)
                    *p->err = "bad escape";
                return NULL;
            }
            i++;
        } else {
            out[o++] = c;
            i++;
        }
    }
    out[o] = '\0';
    p->pos = qend + 1; /* consume closing quote */
    return out;
}

static NmJson *parse_object(Parser *p)
{
    NmJson *v = new_node(p, NM_JSON_OBJECT);
    if (!v)
        return NULL;
    p->pos++; /* { */
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        return v;
    }
    for (;;) {
        skip_ws(p);
        char *key = parse_string_raw(p);
        if (!key)
            return NULL;
        skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') {
            if (p->err)
                *p->err = "expected ':'";
            return NULL;
        }
        p->pos++;
        skip_ws(p);
        NmJson *val = parse_value(p);
        if (!val)
            return NULL;
        if (v->u.obj.len == v->u.obj.cap) {
            size_t nc = v->u.obj.cap ? v->u.obj.cap * 2 : 8;
            NmJsonMember *nm = arena_alloc(p->arena, nc * sizeof(NmJsonMember));
            if (!nm)
                return NULL;
            if (v->u.obj.len)
                memcpy(nm, v->u.obj.members, v->u.obj.len * sizeof(NmJsonMember));
            v->u.obj.members = nm;
            v->u.obj.cap = nc;
        }
        v->u.obj.members[v->u.obj.len].key = key;
        v->u.obj.members[v->u.obj.len].val = val;
        v->u.obj.len++;
        skip_ws(p);
        if (p->pos >= p->len) {
            if (p->err)
                *p->err = "unterminated object";
            return NULL;
        }
        if (p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->s[p->pos] == '}') {
            p->pos++;
            return v;
        }
        if (p->err)
            *p->err = "expected ',' or '}'";
        return NULL;
    }
}

static NmJson *parse_array(Parser *p)
{
    NmJson *v = new_node(p, NM_JSON_ARRAY);
    if (!v)
        return NULL;
    p->pos++; /* [ */
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') {
        p->pos++;
        return v;
    }
    for (;;) {
        skip_ws(p);
        NmJson *val = parse_value(p);
        if (!val)
            return NULL;
        if (v->u.arr.len == v->u.arr.cap) {
            size_t nc = v->u.arr.cap ? v->u.arr.cap * 2 : 8;
            NmJson **ni = arena_alloc(p->arena, nc * sizeof(NmJson *));
            if (!ni)
                return NULL;
            if (v->u.arr.len)
                memcpy(ni, v->u.arr.items, v->u.arr.len * sizeof(NmJson *));
            v->u.arr.items = ni;
            v->u.arr.cap = nc;
        }
        v->u.arr.items[v->u.arr.len++] = val;
        skip_ws(p);
        if (p->pos >= p->len) {
            if (p->err)
                *p->err = "unterminated array";
            return NULL;
        }
        if (p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->s[p->pos] == ']') {
            p->pos++;
            return v;
        }
        if (p->err)
            *p->err = "expected ',' or ']'";
        return NULL;
    }
}

static NmJson *parse_number(Parser *p)
{
    size_t start = p->pos;
    if (p->pos < p->len && (p->s[p->pos] == '-' || p->s[p->pos] == '+'))
        p->pos++;
    while (p->pos < p->len && ((p->s[p->pos] >= '0' && p->s[p->pos] <= '9') || p->s[p->pos] == '.' || p->s[p->pos] == 'e' || p->s[p->pos] == 'E' || p->s[p->pos] == '-' || p->s[p->pos] == '+'))
        p->pos++;
    char tmp[64];
    size_t n = p->pos - start;
    if (n == 0 || n >= sizeof(tmp)) {
        if (p->err)
            *p->err = "bad number";
        return NULL;
    }
    memcpy(tmp, p->s + start, n);
    tmp[n] = '\0';
    char *end;
    double d = strtod(tmp, &end);
    if (end == tmp) {
        if (p->err)
            *p->err = "bad number";
        return NULL;
    }
    NmJson *v = new_node(p, NM_JSON_NUMBER);
    if (!v)
        return NULL;
    v->u.number = d;
    return v;
}

static NmJson *parse_value(Parser *p)
{
    skip_ws(p);
    if (p->pos >= p->len) {
        if (p->err)
            *p->err = "unexpected end of input";
        return NULL;
    }
    switch (p->s[p->pos]) {
    case '{':
        return parse_object(p);
    case '[':
        return parse_array(p);
    case '"':
    {
        NmJson *v = new_node(p, NM_JSON_STRING);
        if (!v)
            return NULL;
        v->u.string = parse_string_raw(p);
        return v->u.string ? v : NULL;
    }
    case 't':
        if (p->len - p->pos < 4 || memcmp(p->s + p->pos, "true", 4) != 0) {
            if (p->err)
                *p->err = "bad literal";
            return NULL;
        }
        p->pos += 4;
        {
            NmJson *v = new_node(p, NM_JSON_BOOL);
            if (v)
                v->u.boolean = 1;
            return v;
        }
    case 'f':
        if (p->len - p->pos < 5 || memcmp(p->s + p->pos, "false", 5) != 0) {
            if (p->err)
                *p->err = "bad literal";
            return NULL;
        }
        p->pos += 5;
        {
            NmJson *v = new_node(p, NM_JSON_BOOL);
            if (v)
                v->u.boolean = 0;
            return v;
        }
    case 'n':
        if (p->len - p->pos < 4 || memcmp(p->s + p->pos, "null", 4) != 0) {
            if (p->err)
                *p->err = "bad literal";
            return NULL;
        }
        p->pos += 4;
        return new_node(p, NM_JSON_NULL);
    default:
        if (p->s[p->pos] == '-' || (p->s[p->pos] >= '0' && p->s[p->pos] <= '9'))
            return parse_number(p);
        if (p->err)
            *p->err = "unexpected character";
        return NULL;
    }
}

NmJson *nm_json_parse(const char *text, size_t len, const char **err)
{
    if (err)
        *err = NULL;
    if (!text || len == 0) {
        if (err)
            *err = "empty input";
        return NULL;
    }
    Arena *arena = calloc(1, sizeof(Arena));
    if (!arena)
        return NULL;
    Parser p = { text, 0, len, err, arena };
    NmJson *root = parse_value(&p);
    if (!root) {
        arena_free(arena);
        free(arena);
        return NULL;
    }
    /* The root node IS the arena owner: fold the arena descriptor in. */
    root->arena = *arena;
    free(arena);
    return root;
}

static void free_built(NmJson *v);

void nm_json_free(NmJson *v)
{
    if (!v)
        return;
    if (v->heap_owned) {
        /* Built tree: recursively free each malloc'd node. */
        free_built(v);
        return;
    }
    /* Parsed tree: whole document shares the root's arena — walk the
     * chunk list once. */
    arena_free(&v->arena);
}

/* ---------------------------------------------------------------- */
/* Reader accessors                                                 */
/* ---------------------------------------------------------------- */

NmJson *nm_json_get(const NmJson *obj, const char *key)
{
    if (!obj || obj->type != NM_JSON_OBJECT || !key)
        return NULL;
    for (size_t i = 0; i < obj->u.obj.len; i++) {
        if (strcmp(obj->u.obj.members[i].key, key) == 0)
            return obj->u.obj.members[i].val;
    }
    return NULL;
}

NmJson *nm_json_at(const NmJson *arr, size_t i)
{
    if (!arr || arr->type != NM_JSON_ARRAY || i >= arr->u.arr.len)
        return NULL;
    return arr->u.arr.items[i];
}

size_t nm_json_len(const NmJson *v)
{
    if (!v)
        return 0;
    if (v->type == NM_JSON_OBJECT)
        return v->u.obj.len;
    if (v->type == NM_JSON_ARRAY)
        return v->u.arr.len;
    return 0;
}

const char *nm_json_key(const NmJson *obj, size_t i)
{
    if (!obj || obj->type != NM_JSON_OBJECT || i >= obj->u.obj.len)
        return NULL;
    return obj->u.obj.members[i].key;
}

const char *nm_json_str(const NmJson *v)
{
    return (v && v->type == NM_JSON_STRING) ? v->u.string : NULL;
}

int nm_json_bool(const NmJson *v)
{
    return (v && v->type == NM_JSON_BOOL) ? v->u.boolean : 0;
}

double nm_json_num(const NmJson *v)
{
    return (v && v->type == NM_JSON_NUMBER) ? v->u.number : 0.0;
}

/* ---------------------------------------------------------------- */
/* Writer: build + serialize                                         */
/* ---------------------------------------------------------------- */

static NmJson *build_new(NmJsonType t)
{
    NmJson *v = calloc(1, sizeof(NmJson));
    if (!v)
        return NULL;
    v->type = t;
    v->heap_owned = 1;
    return v;
}

NmJson *nm_json_new_object(void) { return build_new(NM_JSON_OBJECT); }
NmJson *nm_json_new_array(void) { return build_new(NM_JSON_ARRAY); }
NmJson *nm_json_new_null(void) { return build_new(NM_JSON_NULL); }

NmJson *nm_json_new_string(const char *s)
{
    NmJson *v = build_new(NM_JSON_STRING);
    if (!v)
        return NULL;
    v->u.string = strdup(s ? s : "");
    return v;
}

NmJson *nm_json_new_number(double d)
{
    NmJson *v = build_new(NM_JSON_NUMBER);
    if (v)
        v->u.number = d;
    return v;
}

NmJson *nm_json_new_bool(int b)
{
    NmJson *v = build_new(NM_JSON_BOOL);
    if (v)
        v->u.boolean = b ? 1 : 0;
    return v;
}

/* Writer nodes own individual mallocs (strings, keys, member/item
 * arrays), freed recursively by free_built() via nm_json_free. */

static void free_built(NmJson *v)
{
    if (!v)
        return;
    switch (v->type) {
    case NM_JSON_STRING:
        free(v->u.string);
        break;
    case NM_JSON_OBJECT:
        for (size_t i = 0; i < v->u.obj.len; i++) {
            free(v->u.obj.members[i].key);
            free_built(v->u.obj.members[i].val);
        }
        free(v->u.obj.members);
        break;
    case NM_JSON_ARRAY:
        for (size_t i = 0; i < v->u.arr.len; i++)
            free_built(v->u.arr.items[i]);
        free(v->u.arr.items);
        break;
    default:
        break;
    }
    free(v);
}

/* Deep-convert a parsed (arena-owned) subtree into a heap-owned
 * built tree. Object/array children are copied recursively; string
 * values and keys are strdup'd so nothing borrows arena memory.
 * Used when grafting parsed nodes into a built tree — a built tree
 * must be uniformly heap-owned, or free_built() bad-frees borrowed
 * pointers. Returns the same node if already built. */
static NmJson *clone_built(const NmJson *v)
{
    if (!v)
        return NULL;
    if (v->heap_owned)
        return (NmJson *)v;
    switch (v->type) {
    case NM_JSON_OBJECT:
    {
        NmJson *o = build_new(NM_JSON_OBJECT);
        if (!o)
            return NULL;
        for (size_t i = 0; i < v->u.obj.len; i++) {
            NmJson *cv = clone_built(v->u.obj.members[i].val);
            if (!cv)
                continue; /* OOM: skip member rather than alias arena */
            nm_json_set(o, v->u.obj.members[i].key, cv);
        }
        return o;
    }
    case NM_JSON_ARRAY:
    {
        NmJson *a = build_new(NM_JSON_ARRAY);
        if (!a)
            return NULL;
        for (size_t i = 0; i < v->u.arr.len; i++) {
            NmJson *cv = clone_built(v->u.arr.items[i]);
            if (cv)
                nm_json_push(a, cv);
        }
        return a;
    }
    case NM_JSON_STRING:
        return nm_json_new_string(v->u.string);
    case NM_JSON_NUMBER:
        return nm_json_new_number(v->u.number);
    case NM_JSON_BOOL:
        return nm_json_new_bool(v->u.boolean);
    default:
        return nm_json_new_null();
    }
}

void nm_json_set(NmJson *obj, const char *key, NmJson *v)
{
    if (!obj || obj->type != NM_JSON_OBJECT || !key || !v)
        return;
    v = clone_built(v); /* parsed nodes become heap-owned on graft */
    if (!v)
        return;
    for (size_t i = 0; i < obj->u.obj.len; i++) {
        if (strcmp(obj->u.obj.members[i].key, key) == 0) {
            free_built(obj->u.obj.members[i].val);
            obj->u.obj.members[i].val = v;
            return;
        }
    }
    if (obj->u.obj.len == obj->u.obj.cap) {
        size_t nc = obj->u.obj.cap ? obj->u.obj.cap * 2 : 8;
        NmJsonMember *nm = realloc(obj->u.obj.members, nc * sizeof(NmJsonMember));
        if (!nm)
            return;
        obj->u.obj.members = nm;
        obj->u.obj.cap = nc;
    }
    obj->u.obj.members[obj->u.obj.len].key = strdup(key);
    obj->u.obj.members[obj->u.obj.len].val = v;
    obj->u.obj.len++;
}

void nm_json_push(NmJson *arr, NmJson *v)
{
    if (!arr || arr->type != NM_JSON_ARRAY || !v)
        return;
    v = clone_built(v); /* parsed nodes become heap-owned on graft */
    if (!v)
        return;
    if (arr->u.arr.len == arr->u.arr.cap) {
        size_t nc = arr->u.arr.cap ? arr->u.arr.cap * 2 : 8;
        NmJson **ni = realloc(arr->u.arr.items, nc * sizeof(NmJson *));
        if (!ni)
            return;
        arr->u.arr.items = ni;
        arr->u.arr.cap = nc;
    }
    arr->u.arr.items[arr->u.arr.len++] = v;
}

/* Serialization buffer (memory reuse: one buffer for the whole dump). */
typedef struct DumpBuf
{
    char *s;
    size_t len;
    size_t cap;
    int oom;
} DumpBuf;

static void db_putc(DumpBuf *b, char c)
{
    if (b->oom)
        return;
    if (b->len + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        char *ns = realloc(b->s, nc);
        if (!ns) {
            b->oom = 1;
            return;
        }
        b->s = ns;
        b->cap = nc;
    }
    b->s[b->len++] = c;
}

static void db_puts(DumpBuf *b, const char *s)
{
    while (*s)
        db_putc(b, *s++);
}

static void db_putn(DumpBuf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        db_putc(b, s[i]);
}

static void dump_value(DumpBuf *b, const NmJson *v)
{
    if (!v) {
        db_puts(b, "null");
        return;
    }
    switch (v->type) {
    case NM_JSON_NULL:
        db_puts(b, "null");
        break;
    case NM_JSON_BOOL:
        db_puts(b, v->u.boolean ? "true" : "false");
        break;
    case NM_JSON_NUMBER:
    {
        char tmp[64];
        /* Integral numbers serialize without a trailing .0 — the
         * wire format for counts and indices. */
        if (v->u.number == (long long)v->u.number) {
            snprintf(tmp, sizeof(tmp), "%lld", (long long)v->u.number);
        } else {
            snprintf(tmp, sizeof(tmp), "%g", v->u.number);
        }
        db_puts(b, tmp);
        break;
    }
    case NM_JSON_STRING:
    {
        const char *s = v->u.string ? v->u.string : "";
        db_putc(b, '"');
        for (const char *p = s; *p; p++) {
            unsigned char c = (unsigned char)*p;
            switch (c) {
            case '"':
                db_puts(b, "\\\"");
                break;
            case '\\':
                db_puts(b, "\\\\");
                break;
            case '\b':
                db_puts(b, "\\b");
                break;
            case '\f':
                db_puts(b, "\\f");
                break;
            case '\n':
                db_puts(b, "\\n");
                break;
            case '\r':
                db_puts(b, "\\r");
                break;
            case '\t':
                db_puts(b, "\\t");
                break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    db_puts(b, tmp);
                } else {
                    db_putc(b, (char)c);
                }
            }
        }
        db_putc(b, '"');
        break;
    }
    case NM_JSON_ARRAY:
        db_putc(b, '[');
        for (size_t i = 0; i < v->u.arr.len; i++) {
            if (i)
                db_putc(b, ',');
            dump_value(b, v->u.arr.items[i]);
        }
        db_putc(b, ']');
        break;
    case NM_JSON_OBJECT:
        db_putc(b, '{');
        for (size_t i = 0; i < v->u.obj.len; i++) {
            if (i)
                db_putc(b, ',');
            db_putc(b, '"');
            db_putn(b, v->u.obj.members[i].key,
                    strlen(v->u.obj.members[i].key));
            db_puts(b, "\":");
            dump_value(b, v->u.obj.members[i].val);
        }
        db_putc(b, '}');
        break;
    }
}

char *nm_json_dump(const NmJson *v)
{
    DumpBuf b = { NULL, 0, 0, 0 };
    dump_value(&b, v);
    db_putc(&b, '\0');
    if (b.oom) {
        free(b.s);
        return NULL;
    }
    return b.s;
}

/* json.c - minimal JSON reader/writer
 *
 * Single-pass recursive descent parser; hand-rolled string escaper.
 * No regex, no external deps. Sized for chat-completion payloads and
 * model catalogs, nothing more.
 *
 * TODO(phase 1): real implementation. Currently a stub so the
 * skeleton links; test_json.c drives the real one.
 */

#include <stdlib.h>
#include <string.h>

#include "nevermore/json.h"

/* Internal representation — the public header only exposes pointers
 * and accessors, so the struct is opaque and fully internal. */
struct NmJson
{
    NmJsonType type;
    union
    {
        char *str;
        double num;
        int b;
    };
};

NmJsonType nm_json_type(const NmJson *v)
{
    return v ? v->type : NM_JSON_NULL;
}

NmJson *nm_json_parse(const char *text, size_t len, const char **err)
{
    /* TODO(phase 1). */
    (void)text;
    (void)len;
    if (err)
        *err = "parser not yet implemented";
    return NULL;
}

void nm_json_free(NmJson *v)
{
    /* TODO(phase 1): recursive free of children. */
    free(v);
}

NmJson *nm_json_get(const NmJson *obj, const char *key)
{
    (void)obj;
    (void)key;
    return NULL; /* TODO(phase 1) */
}

NmJson *nm_json_at(const NmJson *arr, size_t i)
{
    (void)arr;
    (void)i;
    return NULL; /* TODO(phase 1) */
}

size_t nm_json_len(const NmJson *arr_or_obj)
{
    (void)arr_or_obj;
    return 0; /* TODO(phase 1) */
}

const char *nm_json_key(const NmJson *obj, size_t i)
{
    (void)obj;
    (void)i;
    return NULL; /* TODO(phase 1) */
}

const char *nm_json_str(const NmJson *v)
{
    return (v && v->type == NM_JSON_STRING) ? v->str : NULL;
}

int nm_json_bool(const NmJson *v)
{
    return (v && v->type == NM_JSON_BOOL) ? v->b : 0;
}

double nm_json_num(const NmJson *v)
{
    return (v && v->type == NM_JSON_NUMBER) ? v->num : 0;
}

NmJson *nm_json_new_object(void) { return NULL; } /* TODO(phase 1) */
NmJson *nm_json_new_array(void) { return NULL; }  /* TODO(phase 1) */
NmJson *nm_json_new_string(const char *s)
{
    (void)s;
    return NULL;
} /* TODO */
NmJson *nm_json_new_number(double d)
{
    (void)d;
    return NULL;
} /* TODO */
NmJson *nm_json_new_bool(int b)
{
    (void)b;
    return NULL;
}                                               /* TODO */
NmJson *nm_json_new_null(void) { return NULL; } /* TODO */

void nm_json_set(NmJson *obj, const char *key, NmJson *v)
{
    (void)obj;
    (void)key;
    (void)v; /* TODO(phase 1) */
}

void nm_json_push(NmJson *arr, NmJson *v)
{
    (void)arr;
    (void)v; /* TODO(phase 1) */
}

char *nm_json_dump(const NmJson *v)
{
    (void)v;
    return NULL; /* TODO(phase 1) */
}

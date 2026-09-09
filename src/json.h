/* json.h - minimal JSON reader/writer
 *
 * Hand-rolled, zero-dependency, no regex. The reader is a single-pass
 * recursive descent parser producing a document tree; the writer is a
 * small serialization layer with string escaping. Enough for chat
 * completion requests, SSE deltas, tool-call arguments, and model
 * catalogs — nothing more, on purpose.
 */

#ifndef NM_JSON_H
#define NM_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    NM_JSON_NULL,
    NM_JSON_BOOL,
    NM_JSON_NUMBER,
    NM_JSON_STRING,
    NM_JSON_ARRAY,
    NM_JSON_OBJECT
} NmJsonType;

typedef struct NmJson NmJson;

NmJsonType nm_json_type(const NmJson *v);

/* Reader */
NmJson *nm_json_parse(const char *text, size_t len, const char **err);
void nm_json_free(NmJson *v);

/* Object/array access; NULL when absent. */
NmJson *nm_json_get(const NmJson *obj, const char *key);
NmJson *nm_json_at(const NmJson *arr, size_t i);
size_t nm_json_len(const NmJson *arr_or_obj);
const char *nm_json_key(const NmJson *obj, size_t i); /* iterate keys */

/* Scalar access */
const char *nm_json_str(const NmJson *v); /* NULL unless NM_JSON_STRING */
int nm_json_bool(const NmJson *v);         /* 0 unless NM_JSON_BOOL */
double nm_json_num(const NmJson *v);       /* 0 unless NM_JSON_NUMBER */

/* Writer: build values, serialize. The returned string is heap-owned. */
NmJson *nm_json_new_object(void);
NmJson *nm_json_new_array(void);
NmJson *nm_json_new_string(const char *s);
NmJson *nm_json_new_number(double d);
NmJson *nm_json_new_bool(int b);
NmJson *nm_json_new_null(void);
void nm_json_set(NmJson *obj, const char *key, NmJson *v);
void nm_json_push(NmJson *arr, NmJson *v);
char *nm_json_dump(const NmJson *v);

#ifdef __cplusplus
}
#endif

#endif // NM_JSON_H

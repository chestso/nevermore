/* wire_recorder.h - wire debug recorder (docs/WIRE-DEBUG.md)
 *
 * NEVERMORE_DEBUG_WIRE=1 turns nevermore into its own packet capture:
 * every HTTP request/response and each SSE event is appended to one
 * NDJSON log file (banner first line), with secrets redacted at
 * serialization time via the NmRequestHeader.secret marker.
 *
 * One recorder per process, installed as the transport wire tap at
 * startup (main.c): nm_wire_recorder_init parses $NEVERMORE_DEBUG_WIRE,
 * opens the file, writes the banner, and installs the tap. Off by
 * default — unset/empty means zero overhead (the tap stays NULL).
 *
 * Format: line 1 is a plain-text banner starting with '#'; every
 * following line is one JSON object (HAR-shaped request/response
 * keys). See the design doc for the event kinds and their fields.
 */

#ifndef NM_WIRE_RECORDER_H
#define NM_WIRE_RECORDER_H

#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse $NEVERMORE_DEBUG_WIRE and arm the recorder:
 *   "1"            -> default path (state dir /nevermore/wire/)
 *   "<path>"       -> explicit path (overwritten; forward slashes on
 *                     Windows keep JSON strings escape-free)
 *   unset / empty  -> recorder off, no tap, zero overhead
 *
 * provider/model label the banner's run line (NULL-ok). Returns 1
 * when recording is armed, 0 when off, -1 when the file could not be
 * opened (a one-line stderr notice was printed; the run proceeds
 * with the recorder off). */
int nm_wire_recorder_init(const char *provider, const char *model);

/* Tear the recorder down (flush + close). Idempotent; safe before
 * init. Called at process exit — the recorder is also fflushed per
 * event, so a crash loses at most the current line. */
void nm_wire_recorder_shutdown(void);

/* Monotonic seconds since the recorder's first line (t field). The
 * clock starts when the banner is written. 0.0 before init. */
double nm_wire_recorder_now(void);

/* The configured key names for the banner ("keys configured:" line).
 * names-only: values are NEVER logged (WIRE-DEBUG §4). */
void nm_wire_recorder_set_env_keys(const char *const *names, size_t n);

#ifdef __cplusplus
}
#endif

#endif // NM_WIRE_RECORDER_H
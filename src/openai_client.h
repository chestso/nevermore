/* openai_client.h - shared OpenAI-compatible wire client (internal)
 *
 * One HTTP+SSE chat-completions client shared by the openai, ollama,
 * and openrouter providers. Only base URL, auth headers, and model
 * catalogs differ between them. Port of quoth-openai-client.el; wire
 * behavior is specified in quoth/OLLAMA-CLOUD-API.md (Ollama) and
 * docs/OPENROUTER-API.md (OpenRouter).
 *
 * Note: quoth sends "User-Agent: Charm-Fantasy/<ver>" on the OpenAI
 * chat path (it emulates the Crush CLI when talking to Hyper). We do
 * not need that emulation for direct OpenAI/Ollama/OpenRouter calls;
 * nevermore identifies as itself.
 */

#ifndef NM_OPENAI_CLIENT_H
#define NM_OPENAI_CLIENT_H

#include "json.h"
#include "provider.h"
#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmOpenaiEndpoint
{
    const char *base_url;    /* e.g. "https://api.openai.com/v1" */
    const char *auth_header; /* e.g. "Authorization: Bearer %s", or NULL */
    const char *api_key;
    const char *user_agent; /* never NULL */
} NmOpenaiEndpoint;

/* POST {base_url}/chat/completions, stream: true. Parses SSE deltas
 * (choices[0].delta.content / .tool_calls) and drives req->on_delta.
 * Blocking; returns the final status. Implemented as chat_begin +
 * a step pump over the event-driven seam below — one implementation,
 * two drive styles. */
NmChatResult nm_openai_chat(const NmOpenaiEndpoint *ep,
                            const NmChatRequest *req);

/* Event-driven split (phase 4; see NmProvider.chat_begin in
 * provider.h for the drive contract). begin connects and sends the
 * request (blocking connect/send — documented transport.h
 * deferral), then the caller steps from its event loop:
 *
 *   fd = nm_openai_stream_fd(h)      // -1 while none open
 *   readable -> nm_openai_chat_step  // -> NM_CHAT_PENDING (keep
 *                                     //    stepping later), OK
 *                                     //    (complete), ERR_* (fatal)
 *   nm_openai_chat_end(h)            // free; cancel any time
 *
 * on_delta fires from inside chat_step exactly as from chat. */
NmChatStream *nm_openai_chat_begin(const NmOpenaiEndpoint *ep,
                                   const NmChatRequest *req,
                                   NmChatResult *err);
NmChatStatus nm_openai_chat_step(NmChatStream *h, NmChatResult *result);
int nm_openai_stream_fd(NmChatStream *h);
void nm_openai_chat_end(NmChatStream *h);

/* Split "http(s)://host[:port]" into host/port/mode (shared with the
 * providers' catalog fetches). Writes into caller buffers. 0 on
 * success. */
int nm_openai_split_base_url(const char *url, char *host, size_t host_cap,
                             int *port, NmTransportMode *mode);

/* Fetch GET {base_url}/models (OpenAI + OpenRouter catalogs). */
NmJson *nm_openai_models(const NmOpenaiEndpoint *ep, const char **err);

#ifdef __cplusplus
}
#endif

#endif // NM_OPENAI_CLIENT_H

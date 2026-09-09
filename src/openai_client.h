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

#include "nevermore/json.h"
#include "nevermore/provider.h"

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
 * Blocking; returns the final status. */
NmChatResult nm_openai_chat(const NmOpenaiEndpoint *ep,
                            const NmChatRequest *req);

/* Fetch GET {base_url}/models (OpenAI + OpenRouter catalogs). */
NmJson *nm_openai_models(const NmOpenaiEndpoint *ep, const char **err);

#ifdef __cplusplus
}
#endif

#endif // NM_OPENAI_CLIENT_H

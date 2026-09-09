/* openai_client.c - shared OpenAI-compatible wire client
 *
 * POST {base_url}/chat/completions with "stream": true, parse SSE
 * deltas, drive on_delta. Shared by the openai, ollama, and openrouter
 * providers — only the endpoint struct differs.
 *
 * TODO(phase 1): real implementation. Currently a stub so the
 * skeleton links.
 */

#include <string.h>

#include "openai_client.h"

NmChatResult nm_openai_chat(const NmOpenaiEndpoint *ep,
                            const NmChatRequest *req)
{
    NmChatResult r = { NM_CHAT_OK, 0, NULL };
    (void)ep;
    (void)req;
    /* TODO(phase 1): serialize messages, connect, stream, deparse SSE. */
    r.status = NM_CHAT_ERR_PARSE;
    return r;
}

NmJson *nm_openai_models(const NmOpenaiEndpoint *ep, const char **err)
{
    (void)ep;
    if (err)
        *err = "not yet implemented";
    return NULL; /* TODO(phase 1/5) */
}

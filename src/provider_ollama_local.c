/* provider_ollama_local.c - the local Ollama daemon
 *
 * http://localhost:11434, keyless by construction. One wire and one
 * implementation with Ollama Cloud (provider_ollama.c) — this file is
 * only the vtable that pins the endpoint default and the keyless key
 * story: no authinfo machine AND no env key, so `OLLAMA_API_KEY` (the
 * cloud's credential) can never resolve into a local round and ride to
 * whatever answers on the daemon's port. One-sided by design: the
 * daemon needs no key, and a token is never sent to earn one.
 * The chat half is the shared OpenAI-compatible surface; the catalog
 * slots are the native /api machine the cloud file owns.
 */

#include "provider_internal.h"

#include "openai_client.h"
#include "provider_openaicompat.h"

static const char *ollama_local_env_key(const NmProvider *p)
{
    (void)p;
    /* Keyless by CONSTRUCTION, not by convention: the local tier names
     * NO key source, so `nm_provider_api_key` can only answer NULL and
     * a round through it can never carry Authorization. This is the
     * leak that names a shared variable for: OLLAMA_API_KEY is the
     * CLOUD's credential, and "the daemon ignores it" was only true
     * while nothing was listening on 11434 — point it at a replay
     * server, a stray local process, or a colleague's machine, and the
     * cloud token rides there. A local daemon that genuinely sits
     * behind an authenticating proxy is not this provider. */
    return NULL;
}

const struct NmProvider nm_ollama_local_provider = {
    NM_PROVIDER_OLLAMA_LOCAL,
    "ollama:local",
    "http://localhost:11434/v1",
    NULL, /* no authinfo machine: the daemon is keyless */
    nm_openaicompat_chat,
    nm_openaicompat_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_stream_wait_ms,
    nm_openai_stream_finishing,
    nm_openai_chat_end,
    nm_ollama_models,
    nm_ollama_models_cached,
    nm_ollama_models_begin,
    nm_ollama_models_step,
    nm_ollama_models_source,
    nm_ollama_models_end,
    nm_ollama_needs_auth,
    ollama_local_env_key,
    NULL, /* knobs: no pricing/routing vocabulary */
};

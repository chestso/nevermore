/* provider_ollama_local.c - the local Ollama daemon
 *
 * http://localhost:11434, keyless by construction. One wire and one
 * implementation with Ollama Cloud (provider_ollama.c) — this file
 * is only the vtable that pins the endpoint default and the
 * authinfo-less, env-key-less key story (the cloud's OLLAMA_API_KEY
 * is still honored if set, but absent here the daemon needs none).
 */

#include "provider_internal.h"

#include "openai_client.h"

static const char *ollama_local_env_key(const NmProvider *p)
{
    (void)p;
    return "OLLAMA_API_KEY";
}

const struct NmProvider nm_ollama_local_provider = {
    NM_PROVIDER_OLLAMA_LOCAL,
    "ollama-local",
    "http://localhost:11434/v1",
    NULL, /* no authinfo machine: the daemon is keyless */
    nm_ollama_chat,
    nm_ollama_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_chat_end,
    nm_ollama_models,
    nm_ollama_needs_auth,
    ollama_local_env_key,
};

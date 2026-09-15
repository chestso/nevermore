/* provider_opencode_zen.c - OpenCode Zen
 *
 * https://opencode.ai/zen/v1 (pay-per-use credits). One wire and one
 * implementation with OpenCode Go (provider_opencode.c) — this file
 * is only the vtable that pins the tier's base URL and catalog. The
 * "opencode" noun is Go (the box's working tier); this is the Zen
 * tier under an explicit name.
 *
 * Zen's full catalog (data/nm-opencode-zen-models.json) and free ids
 * live in provider_opencode.c; the x-opencode-session rule is shared
 * (required for Zen's free models, harmless for paid ones). See
 * docs/OPENCODE-API.md.
 */

#include "provider_internal.h"

#include "openai_client.h"

static const char *opencode_zen_env_key(const NmProvider *p)
{
    (void)p;
    return "OPENCODE_API_KEY"; /* one key for both tiers */
}

const struct NmProvider nm_opencode_zen_provider = {
    NM_PROVIDER_OPENCODE_ZEN,
    "opencode-zen",
    "https://opencode.ai/zen/v1",
    "opencode.ai",
    nm_opencode_chat,
    nm_opencode_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_chat_end,
    nm_opencode_models,
    nm_opencode_needs_auth,
    opencode_zen_env_key,
};

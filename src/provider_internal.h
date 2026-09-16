/* provider_internal.h - internal declarations shared by provider .c files
 *
 * Each provider defines a global vtable named
 *   nm_<name>_provider
 * and registers it in nevermore.c's g_providers table. Split out so
 * the registry is a plain table of pointers with no extern noise.
 */

#ifndef NM_PROVIDER_INTERNAL_H
#define NM_PROVIDER_INTERNAL_H

#include "provider.h"

extern const struct NmProvider nm_hyper_provider;
extern const struct NmProvider nm_ollama_provider;
extern const struct NmProvider nm_ollama_local_provider;
extern const struct NmProvider nm_openai_provider;
extern const struct NmProvider nm_openrouter_provider;
extern const struct NmProvider nm_opencode_provider;     /* :go */
extern const struct NmProvider nm_opencode_zen_provider; /* :zen */

/* Shared OpenCode wire surface (provider_opencode.c): the Go vtable
 * lives there and provider_opencode_zen.c holds only the second
 * vtable — same one-implementation-two-vtables shape as the ollama
 * pair. The two tiers differ only in base URL and catalog data. */
NmChatResult nm_opencode_chat(const NmProvider *p, const NmChatRequest *req,
                              const char *base_url, const char *api_key);
NmChatStream *nm_opencode_chat_begin(const NmProvider *p,
                                     const NmChatRequest *req,
                                     const char *base_url, const char *api_key,
                                     NmChatResult *err);
const NmModel *nm_opencode_models(const NmProvider *p, const char *base_url,
                                  const char *api_key, size_t *n_out);
int nm_opencode_needs_auth(const NmProvider *p, const char *base_url);

/* Shared Ollama wire surface (provider_ollama.c): the cloud vtable
 * and the local daemon vtable (provider_ollama_local.c) sit on the
 * same OpenAI-compatible client + native /api catalog; only the
 * endpoint default differs, so the implementation lives in one file
 * and is parameterized by the provider's id. */
NmChatResult nm_ollama_chat(const NmProvider *p, const NmChatRequest *req,
                            const char *base_url, const char *api_key);
NmChatStream *nm_ollama_chat_begin(const NmProvider *p,
                                   const NmChatRequest *req,
                                   const char *base_url, const char *api_key,
                                   NmChatResult *err);
const NmModel *nm_ollama_models(const NmProvider *p, const char *base_url,
                                const char *api_key, size_t *n_out);
int nm_ollama_needs_auth(const NmProvider *p, const char *base_url);

/* Conversation id (one per agent/conversation; design §3).
 *
 * A provider-scoped routing hint: providers that care read
 * NmChatRequest.conversation_id and point an extra header at it
 * (x-opencode-session). Stable per agent, non-empty, non-colliding
 * across clients; explicitly not a secret and not authenticated. One
 * declaration, one implementation (agent.c) so the array sizes
 * cannot drift. */
#define NM_CONVERSATION_ID_LEN 40
void nm_conversation_id_new(char out[NM_CONVERSATION_ID_LEN]);

/* Live-catalog fetch gate (shared by all providers): 0 when
 * NM_NO_LIVE_CATALOG is set in the environment. `make check` runs
 * with it set (TESTS_ENVIRONMENT) so default-base catalog probes are
 * no-ops (static fallback) — a default-base probe is a real network
 * call (local-daemon 127.0.0.1:11434, api.openai.com, ...) whose
 * latency or refusal is CI-environment-dependent (a blackholed
 * loopback connect stalled Windows CI past the 10s watchdog).
 * Providers apply the gate ONLY when the caller passed no explicit
 * base_url: scripted-server tests (explicit base) keep exercising
 * the live fetch path. */
int nm_live_catalog_enabled(void);

#endif // NM_PROVIDER_INTERNAL_H

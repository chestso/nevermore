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
extern const struct NmProvider nm_openrouter_provider;
extern const struct NmProvider nm_opencode_provider;     /* :go */
extern const struct NmProvider nm_opencode_zen_provider; /* :zen */

/* Provider credits descriptors (credit.c / provider_*.c) */
extern const struct NmProviderCredits nm_hyper_credits;
extern const struct NmProviderCredits nm_ollama_credits;
extern const struct NmProviderCredits nm_openrouter_credits;
extern const struct NmProviderCredits nm_opencode_credits;
extern const struct NmProviderCredits nm_opencode_zen_credits;

/* Shared OpenCode wire surface (provider_opencode.c): the two tiers
 * differ only in base URL and shipped table, so they share one vtable
 * surface — the descriptors live there (provider_openaicompat.h) and
 * this is the auth rule they both point at. */
int nm_opencode_needs_auth(const NmProvider *p, const char *base_url);

/* The OpenCode CATALOG: the tier's live list (the membership) and,
 * when the generated table cannot cover it, the catalog document (the
 * coverage). The two-stage machine is the tiers' own (the chat half
 * stays on the shared OpenAI-compatible surface), so the vtable slots
 * below are the OpenCode ones. */
const NmModel *nm_opencode_models(const NmProvider *p, const char *base_url,
                                  const char *api_key, size_t *n_out);
const NmModel *nm_opencode_models_cached(const NmProvider *p, size_t *n_out);
int nm_opencode_models_begin(const NmProvider *p, const char *base_url,
                             const char *api_key);
NmCatalogStatus nm_opencode_models_step(const NmProvider *p);
NmSource nm_opencode_models_source(const NmProvider *p);
int nm_opencode_models_wait_ms(const NmProvider *p);
void nm_opencode_models_end(const NmProvider *p);

/* Shared Ollama wire surface (provider_ollama.c): the cloud vtable
 * and the local daemon vtable (provider_ollama_local.c) sit on the
 * same OpenAI-compatible client + native /api catalog; only the
 * endpoint default differs, so the implementation lives in one file
 * and is parameterized by the provider's id. (The chat half itself is
 * the shared OpenAI-compatible surface — provider_openaicompat.h.) */
const NmModel *nm_ollama_models(const NmProvider *p, const char *base_url,
                                const char *api_key, size_t *n_out);
const NmModel *nm_ollama_models_cached(const NmProvider *p, size_t *n_out);
/* The async catalog seam, shared by both tiers (the vtable slots). */
int nm_ollama_models_begin(const NmProvider *p, const char *base_url,
                           const char *api_key);
NmCatalogStatus nm_ollama_models_step(const NmProvider *p);
NmSource nm_ollama_models_source(const NmProvider *p);
int nm_ollama_models_wait_ms(const NmProvider *p);
void nm_ollama_models_end(const NmProvider *p);
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

/* The unbidden-fetch gate (shared by all providers): 0 when
 * NM_NO_UNBIDDEN_FETCH is set in the environment. "Unbidden" is the
 * rule, not a mood: a fetch nobody at the terminal asked for, or one
 * to a host nobody named. It covers
 *   - the app's own warms and refreshes — the model catalog
 *     (nm_chat_app_warm_catalog) and the credit balance
 *     (nm_chat_app_warm_credits, the idle tick's refresh) — which run
 *     with no user action at all;
 *   - a CATALOG fetch at a provider's DEFAULT base (no explicit
 *     base_url), where the app would have to pick the host itself.
 * It does NOT cover what the user bade: chat turns, /credits, and any
 * fetch to an explicitly given base.
 *
 * `make check` runs with it set (TESTS_ENVIRONMENT) so default-base
 * catalog probes are no-ops (static fallback) — a default-base probe
 * is a real network call (local-daemon 127.0.0.1:11434,
 * hyper.charm.land, ...) whose latency or refusal is
 * CI-environment-dependent (a blackholed loopback connect stalled
 * Windows CI past the 10s watchdog). Providers apply the gate ONLY
 * when the caller passed no explicit base_url: scripted-server tests
 * (explicit base) keep exercising the live fetch path. The app's own
 * warms apply it unconditionally, explicit base or not — that is
 * exactly the traffic nobody asked for (the wire-replay tool sets the
 * knob because its server holds only chat completions). */
int nm_unbidden_fetch_enabled(void);

#endif // NM_PROVIDER_INTERNAL_H

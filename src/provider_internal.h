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
extern const struct NmProvider nm_openai_provider;
extern const struct NmProvider nm_openrouter_provider;

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

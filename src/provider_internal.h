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

#endif // NM_PROVIDER_INTERNAL_H

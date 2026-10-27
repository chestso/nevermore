/* identity.h - the build's one identity string.
 *
 * Three surfaces render it, and none may disagree on content:
 *
 *   nevermore --version   NM_IDENTITY_LINE  ("nevermore <ver> (...)")
 *   TUI banner            NM_USER_AGENT     (the wire form, verbatim)
 *   User-Agent            NM_USER_AGENT     (chat + catalog fetches)
 *
 * The UA is the --version line as an HTTP product token: the only
 * difference is the separator between product name and version (a
 * space becomes the UA's `/`). Composing both from one tail macro makes
 * "the UA matches --version" structural rather than a convention.
 *
 * The APP's identity is the other half of the same idea, and it is a
 * PRODUCT fact (not a build fact): the public address a provider
 * attributes traffic by — OpenRouter's `HTTP-Referer`, where the URL
 * IS the app's identity (OPENROUTER-API.md §9) — and the display name
 * that goes with it. It lives here so the wire, the docs and the
 * provider's dashboard cannot disagree about which app this is.
 *
 * Every component is a build fact already reported elsewhere:
 * NEVERMORE_VERSION from the generated header (git describe base +
 * distance + sha, `-dirty` included), and BOBA_VERSION plus the target
 * OS/CPU from configure. The OS label is the canonical triple's word,
 * so a GNU system on the Linux kernel says GNU/Linux — see the mapping
 * in configure.ac.
 *
 * Identifies, does not impersonate: quoth's "Charm-Fantasy/<ver>"
 * Crush emulation is still deliberately not ported.
 */

#ifndef NM_IDENTITY_H
#define NM_IDENTITY_H

#include "config.h"            /* BOBA_VERSION, NM_BUILD_OS/ARCH */
#include "nevermore_version.h" /* NEVERMORE_VERSION */

/* Standalone/IDE include paths only: configure always defines these,
 * and the version header is BUILT_SOURCES in src plus an explicit
 * dependency for the test objects. */
#ifndef BOBA_VERSION
#define BOBA_VERSION "unknown"
#endif
#ifndef NM_BUILD_OS
#define NM_BUILD_OS "unknown"
#endif
#ifndef NM_BUILD_ARCH
#define NM_BUILD_ARCH "unknown"
#endif

#define NM_IDENTITY_TAIL \
    " (boba " BOBA_VERSION "; " NM_BUILD_OS " " NM_BUILD_ARCH ")"

/* Exactly what `nevermore --version` prints. */
#define NM_IDENTITY_LINE "nevermore " NEVERMORE_VERSION NM_IDENTITY_TAIL

/* The same identity as a User-Agent product token — and the TUI banner. */
#define NM_USER_AGENT "nevermore/" NEVERMORE_VERSION NM_IDENTITY_TAIL

/* ---------------------------------------------------------------- */
/* The APP's identity (a product fact, not a build fact)             */
/* ---------------------------------------------------------------- */

/* The app's public address: the URL a provider attributes this
 * product's traffic BY. On OpenRouter the URL IS the app's identity:
 * the app is created by the first request that carries it, and the
 * whole URL (not the origin) is the key — `https://host/path` and
 * `https://host/other` are two apps (OPENROUTER-API.md §9). So the
 * spelling here is the FULL page, in its CANONICAL form: no trailing
 * slash (the provider normalizes it away, and the app's page answers
 * at the slashless spelling), no query, no fragment. */
#define NM_APP_URL "https://chest.so/nevermore"

/* The app's display name, as a provider shows it. */
#define NM_APP_NAME "nevermore"

/* The marketplace category a provider files the app under. OpenRouter
 * recognizes a fixed vocabulary (cli-agent, ide-extension,
 * cloud-agent, …); an unrecognized value is dropped without an error,
 * so the value must be one of the documented ones. nevermore is a
 * terminal coding agent: cli-agent. */
#define NM_APP_CATEGORY "cli-agent"

/* The environment knob that REPLACES the app's URL for one run. A
 * fork, a private deployment or a CI rig that must not appear in the
 * public rankings points it somewhere else — and a wrong URL is not a
 * rename, it is a SECOND app (the referer is the app's key). Unset =
 * the shipped URL above. */
#define NM_APP_URL_ENV "NEVERMORE_APP_URL"

#endif /* NM_IDENTITY_H */

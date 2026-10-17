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

#endif /* NM_IDENTITY_H */

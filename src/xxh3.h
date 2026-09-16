/* xxh3.h - XXH3-64 (seed 0), the hash behind the Hyper affinity headers
 *
 * Implemented from the algorithm specification, Cyan4973/xxHash
 * doc/xxhash_spec.md (v0.2.0) — NOT ported from a third-party
 * implementation. The digest must equal the reference for the
 * affinity headers to be honored: hyper's gateway (and the Crush CLI
 * before it) hashes with zeebo/xxh3 (seed 0), itself a port of the C
 * reference. The tests pin the official known-answer values plus
 * every algorithm-boundary length (small/medium/large divisions at
 * 16/17, 128/129, 240/241, and the 1024-byte block edge).
 *
 * Seed 0 only, by design — this is a non-goal, not a deferral: the
 * only consumers are the Hyper x-session-id / x-session-affinity /
 * x-crush-id headers, and neither the Hyper wire spec nor Crush seeds
 * them. A seeded variant would mean threading a uint64_t parameter,
 * adding deriveSecret() for the >240-byte path, and folding
 * +seed/-seed into the short-input and mixStep combinations — a
 * non-breaking internal change (pre-alpha: no backwards-compat
 * constraint), and the shape of the change is recorded here so a
 * future reader sees the choice was made, not overlooked. Until a
 * seeded consumer exists, an unseeded branch would be dead code, and
 * an untested branch is where correctness bugs hide.
 */

#ifndef NM_XXH3_H
#define NM_XXH3_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* XXH3-64 of `len` bytes at `data`, seed 0. */
uint64_t nm_xxh3_64(const void *data, size_t len);

/* The same digest in canonical form: 16 lowercase hex characters,
 * big-endian (most significant byte first), NUL-terminated. `out`
 * must have room for 17 bytes. This is the string the wire headers
 * carry. */
void nm_xxh3_64_hex(const void *data, size_t len, char out[17]);

#ifdef __cplusplus
}
#endif

#endif // NM_XXH3_H
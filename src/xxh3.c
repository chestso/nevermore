/* xxh3.c - XXH3-64 (seed 0), implemented from the algorithm spec
 *
 * Source of truth: Cyan4973/xxHash doc/xxhash_spec.md (v0.2.0). The
 * structure below follows the spec's sections in order — small
 * (0-16), medium (17-240), large (241+) — so it can be read
 * side-by-side with the document. The default 192-byte secret is the
 * spec's literal table; every read is little-endian per the spec's
 * "secret values are always read using little-endian convention".
 *
 * Deliberately not a port of zeebo/xxh3 or any other implementation:
 * a port inherits that code's bugs. (The quoth Elisp port, read as a
 * cross-check, is wrong for 129-240-byte inputs — its tests stopped
 * at 48 bytes — so its structure was a trap, not a guide.) The
 * digest must equal the reference for hyper's affinity headers to be
 * honored; tests/test_xxh3.c pins the official known-answer values
 * plus every boundary length.
 *
 * Seed 0 only by design — see xxh3.h for the non-goal note and the
 * exact shape a seeded variant would take.
 *
 * Integer discipline: all arithmetic is mod 2^64. There is no
 * __int128 and no 128-bit type: the two places the spec multiplies
 * to 128 bits (mixStep's mulFold and the final merge) go through
 * mul128_fold() below, built from 32-bit limbs.
 */

#include "xxh3.h"

#include <string.h>

/* Default secret, byte-for-byte from xxhash_spec.md (v0.2.0). Not to
 * be confused with the reference's key64_* constants, which are just
 * little-endian reads of this table. */
static const uint8_t k_secret[192] = {
    0xb8,
    0xfe,
    0x6c,
    0x39,
    0x23,
    0xa4,
    0x4b,
    0xbe,
    0x7c,
    0x01,
    0x81,
    0x2c,
    0xf7,
    0x21,
    0xad,
    0x1c,
    0xde,
    0xd4,
    0x6d,
    0xe9,
    0x83,
    0x90,
    0x97,
    0xdb,
    0x72,
    0x40,
    0xa4,
    0xa4,
    0xb7,
    0xb3,
    0x67,
    0x1f,
    0xcb,
    0x79,
    0xe6,
    0x4e,
    0xcc,
    0xc0,
    0xe5,
    0x78,
    0x82,
    0x5a,
    0xd0,
    0x7d,
    0xcc,
    0xff,
    0x72,
    0x21,
    0xb8,
    0x08,
    0x46,
    0x74,
    0xf7,
    0x43,
    0x24,
    0x8e,
    0xe0,
    0x35,
    0x90,
    0xe6,
    0x81,
    0x3a,
    0x26,
    0x4c,
    0x3c,
    0x28,
    0x52,
    0xbb,
    0x91,
    0xc3,
    0x00,
    0xcb,
    0x88,
    0xd0,
    0x65,
    0x8b,
    0x1b,
    0x53,
    0x2e,
    0xa3,
    0x71,
    0x64,
    0x48,
    0x97,
    0xa2,
    0x0d,
    0xf9,
    0x4e,
    0x38,
    0x19,
    0xef,
    0x46,
    0xa9,
    0xde,
    0xac,
    0xd8,
    0xa8,
    0xfa,
    0x76,
    0x3f,
    0xe3,
    0x9c,
    0x34,
    0x3f,
    0xf9,
    0xdc,
    0xbb,
    0xc7,
    0xc7,
    0x0b,
    0x4f,
    0x1d,
    0x8a,
    0x51,
    0xe0,
    0x4b,
    0xcd,
    0xb4,
    0x59,
    0x31,
    0xc8,
    0x9f,
    0x7e,
    0xc9,
    0xd9,
    0x78,
    0x73,
    0x64,
    0xea,
    0xc5,
    0xac,
    0x83,
    0x34,
    0xd3,
    0xeb,
    0xc3,
    0xc5,
    0x81,
    0xa0,
    0xff,
    0xfa,
    0x13,
    0x63,
    0xeb,
    0x17,
    0x0d,
    0xdd,
    0x51,
    0xb7,
    0xf0,
    0xda,
    0x49,
    0xd3,
    0x16,
    0x55,
    0x26,
    0x29,
    0xd4,
    0x68,
    0x9e,
    0x2b,
    0x16,
    0xbe,
    0x58,
    0x7d,
    0x47,
    0xa1,
    0xfc,
    0x8f,
    0xf8,
    0xb8,
    0xd1,
    0x7a,
    0xd0,
    0x31,
    0xce,
    0x45,
    0xcb,
    0x3a,
    0x8f,
    0x95,
    0x16,
    0x04,
    0x28,
    0xaf,
    0xd7,
    0xfb,
    0xca,
    0xbb,
    0x4b,
    0x40,
    0x7e,
};

/* The spec's prime constants (mod 2^64). */
#define PRIME32_1 0x9E3779B1ULL
#define PRIME32_2 0x85EBCA77ULL
#define PRIME32_3 0xC2B2AE3DULL
#define PRIME64_1 0x9E3779B185EBCA87ULL
#define PRIME64_2 0xC2B2AE3D27D4EB4FULL
#define PRIME64_3 0x165667B19E3779F9ULL
#define PRIME64_4 0x85EBCA77C2B2AE63ULL
#define PRIME64_5 0x27D4EB2F165667C5ULL
#define PRIME_MX1 0x165667919E3779F9ULL
#define PRIME_MX2 0x9FB21C651E98DF25ULL

/* ---------------------------------------------------------------- */
/* Little-endian reads + bit helpers                                 */
/* ---------------------------------------------------------------- */

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read64(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static uint64_t rotl64(uint64_t x, unsigned n)
{
    return n == 0 ? x : (x << n) | (x >> (64 - n));
}

/* Byte swap (the spec's bswap64). */
static uint64_t bswap64(uint64_t x)
{
    return ((x & 0x00000000000000ffULL) << 56) |
           ((x & 0x000000000000ff00ULL) << 40) |
           ((x & 0x0000000000ff0000ULL) << 24) |
           ((x & 0x00000000ff000000ULL) << 8) |
           ((x & 0x000000ff00000000ULL) >> 8) |
           ((x & 0x0000ff0000000000ULL) >> 24) |
           ((x & 0x00ff000000000000ULL) >> 40) |
           ((x & 0xff00000000000000ULL) >> 56);
}

/* (lowerHalf ^ higherHalf) of the full 128-bit product x*y — the
 * spec's mulFold64. Built from 32-bit limbs so no compiler needs a
 * 128-bit integer type. */
static uint64_t mul128_fold(uint64_t x, uint64_t y)
{
    uint64_t xl = x & 0xffffffffULL, xh = x >> 32;
    uint64_t yl = y & 0xffffffffULL, yh = y >> 32;
    uint64_t ll = xl * yl;
    uint64_t lh = xl * yh;
    uint64_t hl = xh * yl;
    uint64_t hh = xh * yh;
    /* Sum the three middle columns; `cross` is at most ~3*2^32, so it
     * fits in 64 bits (the standard schoolbook mul128). */
    uint64_t cross = (ll >> 32) + (lh & 0xffffffffULL) + (hl & 0xffffffffULL);
    uint64_t lo = (cross << 32) | (ll & 0xffffffffULL);
    uint64_t hi = hh + (lh >> 32) + (hl >> 32) + (cross >> 32);
    return lo ^ hi;
}

/* The spec's two final-mix operations. */
static uint64_t avalanche(uint64_t x)
{
    x ^= x >> 37;
    x *= PRIME_MX1;
    x ^= x >> 32;
    return x;
}

static uint64_t avalanche_xxh64(uint64_t x)
{
    x ^= x >> 33;
    x *= PRIME64_2;
    x ^= x >> 29;
    x *= PRIME64_3;
    x ^= x >> 32;
    return x;
}

/* mixStep: one 16-byte data chunk against a 16-byte secret segment.
 * Seed 0, so the spec's (secretWord + seed) / (secretWord - seed) are
 * the secret words unchanged. */
static uint64_t mix_step(const uint8_t *data, size_t secret_off)
{
    const uint8_t *s = k_secret + secret_off;
    return mul128_fold(read64(data) ^ read64(s), read64(data + 8) ^ read64(s + 8));
}

/* ---------------------------------------------------------------- */
/* Small inputs (0-16 bytes)                                         */
/* ---------------------------------------------------------------- */

static uint64_t hash_small(const uint8_t *p, size_t len)
{
    if (len > 8) { /* 9-16 */
        uint64_t input_first = read64(p);
        uint64_t input_last = read64(p + len - 8);
        /* secret[24:56] as two secret-word pairs */
        uint64_t low = (read64(k_secret + 24) ^ read64(k_secret + 32)) ^
                       input_first;
        uint64_t high = (read64(k_secret + 40) ^ read64(k_secret + 48)) ^
                        input_last;
        uint64_t folded = mul128_fold(low, high);
        return avalanche((uint64_t)len + bswap64(low) + high + folded);
    }
    if (len > 3) { /* 4-8 */
        uint32_t input_first = read32(p);
        uint32_t input_last = read32(p + len - 4);
        uint64_t combined = (uint64_t)input_last |
                            ((uint64_t)input_first << 32);
        uint64_t value = combined ^ (read64(k_secret + 8) ^
                                     read64(k_secret + 16));
        value ^= rotl64(value, 49) ^ rotl64(value, 24);
        value *= PRIME_MX2;
        value ^= (value >> 35) + (uint64_t)len;
        value *= PRIME_MX2;
        value ^= value >> 28;
        return value;
    }
    if (len > 0) { /* 1-3 */
        /* combined = last byte | len<<8 | first byte<<16 | mid byte<<24 */
        uint32_t combined = (uint32_t)p[len - 1] | ((uint32_t)len << 8) |
                            ((uint32_t)p[0] << 16) |
                            ((uint32_t)p[len >> 1] << 24);
        uint64_t value = (uint64_t)(read32(k_secret) ^ read32(k_secret + 4)) ^
                         (uint64_t)combined;
        return avalanche_xxh64(value);
    }
    /* Empty: avalanche_xxh64(seed ^ secret[56:64] ^ secret[64:72]). */
    return avalanche_xxh64(read64(k_secret + 56) ^ read64(k_secret + 64));
}

/* ---------------------------------------------------------------- */
/* Medium inputs (17-240 bytes)                                      */
/* ---------------------------------------------------------------- */

static uint64_t hash_medium(const uint8_t *p, size_t len)
{
    uint64_t acc = (uint64_t)len * PRIME64_1;

    if (len <= 128) {
        /* 17-128: pair chunks from both ends, widest first. The loop
         * variable is signed so the final iteration is i == 0. */
        size_t rounds = ((len - 1) >> 5) + 1;
        for (long i = (long)rounds - 1; i >= 0; i--) {
            size_t start = (size_t)i * 16;
            size_t end = len - (size_t)i * 16 - 16;
            acc += mix_step(p + start, (size_t)i * 32);
            acc += mix_step(p + end, (size_t)i * 32 + 16);
        }
        return avalanche(acc);
    }

    /* 129-240: first 128 bytes chunk by chunk, an intermediate
     * avalanche, the remaining full chunks at offset 3+16k, then the
     * final 16 bytes at secret offset 119. */
    size_t chunks = len >> 4;
    for (size_t i = 0; i < 8; i++)
        acc += mix_step(p + i * 16, i * 16);
    acc = avalanche(acc);
    for (size_t i = 8; i < chunks; i++)
        acc += mix_step(p + i * 16, (i - 8) * 16 + 3);
    acc += mix_step(p + len - 16, 119);
    return avalanche(acc);
}

/* ---------------------------------------------------------------- */
/* Large inputs (241+ bytes)                                         */
/* ---------------------------------------------------------------- */

/* One stripe (8 lanes) against a 64-byte secret segment. */
static void accumulate(uint64_t acc[8], const uint8_t *stripe,
                       size_t secret_off)
{
    const uint8_t *s = k_secret + secret_off;
    for (int i = 0; i < 8; i++) {
        uint64_t data = read64(stripe + i * 8);
        uint64_t value = data ^ read64(s + i * 8);
        acc[i ^ 1] += data;
        acc[i] += (uint64_t)(uint32_t)value * (value >> 32);
    }
}

static void scramble(uint64_t acc[8])
{
    const uint8_t *s = k_secret + 128; /* last 64 bytes of the secret */
    for (int i = 0; i < 8; i++) {
        acc[i] ^= acc[i] >> 47;
        acc[i] ^= read64(s + i * 8);
        acc[i] *= PRIME32_1;
    }
}

static uint64_t hash_large(const uint8_t *p, size_t len)
{
    const size_t total = len; /* the final merge seeds with the TOTAL
                               * length, not what the block loop left */
    uint64_t acc[8] = { PRIME32_3, PRIME64_1, PRIME64_2, PRIME64_3,
                        PRIME64_4, PRIME32_2, PRIME64_5, PRIME32_1 };

    /* Full blocks: 16 stripes + a scramble, until <= 1024 bytes remain
     * (the last block is always left for the tail handling, even when
     * full). */
    while (len > 1024) {
        for (size_t n = 0; n < 16; n++)
            accumulate(acc, p + n * 64, n * 8);
        scramble(acc);
        p += 1024;
        len -= 1024;
    }

    /* Last block: full stripes except the last, then the last 64
     * bytes (which may overlap the previous stripes) at secret offset
     * 192-71 == 121. */
    size_t full_stripes = (len - 1) / 64;
    for (size_t n = 0; n < full_stripes; n++)
        accumulate(acc, p + n * 64, n * 8);
    accumulate(acc, p + len - 64, 121);

    /* Final merge: four 128-bit products folded into the
     * length-seeded accumulator, at secret offset 11. */
    uint64_t result = (uint64_t)total * PRIME64_1;
    for (int i = 0; i < 4; i++)
        result += mul128_fold(acc[i * 2] ^ read64(k_secret + 11 + i * 16),
                              acc[i * 2 + 1] ^
                                  read64(k_secret + 19 + i * 16));
    return avalanche(result);
}

/* ---------------------------------------------------------------- */
/* Entry points                                                      */
/* ---------------------------------------------------------------- */

uint64_t nm_xxh3_64(const void *data, size_t len)
{
    const uint8_t *p = data;
    if (len <= 16)
        return hash_small(p, len);
    if (len <= 240)
        return hash_medium(p, len);
    return hash_large(p, len);
}

void nm_xxh3_64_hex(const void *data, size_t len, char out[17])
{
    static const char hex[] = "0123456789abcdef";
    uint64_t h = nm_xxh3_64(data, len);
    for (int i = 0; i < 16; i++) {
        /* Canonical form is big-endian: most significant nibble first
         * (bit 63 down to bit 0). */
        out[i] = hex[(h >> (60 - i * 4)) & 0xf];
    }
    out[16] = '\0';
}
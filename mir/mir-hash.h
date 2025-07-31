/* This file is a part of MIR project.

   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* Simple high-quality multiplicative hash passing demerphq-smhasher,
   faster than spooky, city, or xxhash for strings less 100 bytes.
   Hash for the same key can be different on different architectures.
   To get machine-independent hash, use mir_hash_strict which is about
   1.5 times slower than mir_hash.  */
#ifndef __MIR_HASH__
#define __MIR_HASH__

#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(__i386__) || defined(__PPC64__) || defined(__s390__) \
  || defined(__m32c__) || defined(cris) || defined(__CR16__) || defined(__vax__)        \
  || defined(__m68k__) || defined(__aarch64__) || defined(_M_AMD64) || defined(_M_IX86)
#define MIR_HASH_UNALIGNED_ACCESS 1
#else
#define MIR_HASH_UNALIGNED_ACCESS 0
#endif

#if (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)) || defined(_MSC_VER)
#define MIR_LITTLE_ENDIAN 1
#else
#define MIR_LITTLE_ENDIAN 0
#endif

static inline uint64_t mir_get_key_part (const uint8_t *v, size_t len, int relax_p) {
  size_t i, start = 0;
  uint64_t tail = 0;

  if (relax_p || MIR_LITTLE_ENDIAN) {
#if MIR_HASH_UNALIGNED_ACCESS
    if (len == sizeof (uint64_t)) return *(uint64_t *) v;
    if (len >= sizeof (uint32_t)) {
      tail = (uint64_t) * (uint32_t *) v << 32;
      start = 4;
    }
#endif
  }
  for (i = start; i < len; i++) tail = (tail >> 8) | ((uint64_t) v[i] << 56);
  return tail;
}

static const uint64_t mir_hash_p1 = 0X65862b62bdf5ef4d, mir_hash_p2 = 0X288eea216831e6a7;
static inline uint64_t mir_mum (uint64_t v, uint64_t c, int relax_p) {
  if (relax_p) {
#if defined(__SIZEOF_INT128__)
    __uint128_t r = (__uint128_t) v * (__uint128_t) c;
    return (uint64_t) (r >> 64) + (uint64_t) r;
#endif
  }
  uint64_t v1 = v >> 32, v2 = (uint32_t) v, c1 = c >> 32, c2 = (uint32_t) c, rm = v2 * c1 + v1 * c2;
  return v1 * c1 + (rm >> 32) + v2 * c2 + (rm << 32);
}

static inline uint64_t mir_round (uint64_t state, uint64_t v, int relax_p) {
  state ^= mir_mum (v, mir_hash_p1, relax_p);
  return state ^ mir_mum (state, mir_hash_p2, relax_p);
}

static inline uint64_t mir_hash_1 (const void *key, size_t len, uint64_t seed, int relax_p) {
  const uint8_t *v = (const uint8_t *) key;
  uint64_t r = seed + len;

  for (; len >= 16; len -= 16, v += 16) {
    r ^= mir_mum (mir_get_key_part (v, 8, relax_p), mir_hash_p1, relax_p);
    r ^= mir_mum (mir_get_key_part (v + 8, 8, relax_p), mir_hash_p2, relax_p);
    r ^= mir_mum (r, mir_hash_p1, relax_p);
  }
  if (len >= 8) {
    r ^= mir_mum (mir_get_key_part (v, 8, relax_p), mir_hash_p1, relax_p);
    len -= 8, v += 8;
  }
  if (len != 0) r ^= mir_mum (mir_get_key_part (v, len, relax_p), mir_hash_p2, relax_p);
  return mir_round (r, r, relax_p);
}

static inline uint64_t mir_hash (const void *key, size_t len, uint64_t seed) {
  return mir_hash_1 (key, len, seed, 1);
}

static inline uint64_t mir_hash_strict (const void *key, size_t len, uint64_t seed) {
  return mir_hash_1 (key, len, seed, 0);
}

static inline uint64_t mir_hash_init (uint64_t seed) { return seed; }
static inline uint64_t mir_hash_step (uint64_t h, uint64_t key) { return mir_round (h, key, 1); }
static inline uint64_t mir_hash_finish (uint64_t h) { return mir_round (h, h, 1); }

static inline uint64_t mir_hash64 (uint64_t key, uint64_t seed) {
  return mir_hash_finish (mir_hash_step (mir_hash_init (seed), key));
}

#endif

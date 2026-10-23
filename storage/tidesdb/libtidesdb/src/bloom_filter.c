/**
 *
 * Copyright (C) TidesDB
 *
 * Original Author: Alex Gaetano Padula
 *
 * Licensed under the Mozilla Public License, v. 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.mozilla.org/en-US/MPL/2.0/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bloom_filter.h"

#include <string.h>
#include <tgmath.h>

#define BF_UNLIKELY(x) TDB_UNLIKELY(x)
#define BF_LIKELY(x)   TDB_LIKELY(x)

/* bit manipulation macros for packed bitset */
#define BF_BITS_PER_WORD        64
#define BF_WORD_INDEX(bit)      ((bit) / BF_BITS_PER_WORD)
#define BF_BIT_INDEX(bit)       ((bit) % BF_BITS_PER_WORD)
#define BF_SET_BIT(bitset, bit) ((bitset)[BF_WORD_INDEX(bit)] |= (1ULL << BF_BIT_INDEX(bit)))
#define BF_GET_BIT(bitset, bit) (((bitset)[BF_WORD_INDEX(bit)] >> BF_BIT_INDEX(bit)) & 1ULL)

/* hash mixing prime (murmur-family). chosen for good avalanche behavior in
 * the multiplicative mix below. */
#define BF_HASH_PRIME 0xc6a4a793u

/* index-derivation hash versions. version 1 is the original hash. version 2
 * appends a murmur3 fmix32 finalizer so short keys fully avalanche, which
 * decorrelates h1/h2 and lowers the false-positive rate on small structured
 * keys. the version is stored per filter; a filter is always queried with the
 * same hash that built it, so existing on-disk (v1) filters stay correct. */
#define BF_HASH_VERSION_LEGACY  1u
#define BF_HASH_VERSION_CURRENT 2u
/* serialized v2 filters carry a 0x00 sentinel + version byte. a v1 filter can
 * never start with 0x00 because its first field is varint32(m) and m >= 1. */
#define BF_SERIALIZE_VERSION_SENTINEL 0x00u
#define BF_SERIALIZE_VERSION_BYTES    2

/* upper bound on the number of hash functions accepted by bloom_filter_new.
 * derived h grows logarithmically with target false-positive rate; even at
 * p = 1e-30 the formula yields h ~ 100, so this is a generous sanity ceiling
 * to reject pathological configs (negative or absurdly large values from
 * floating-point edge cases). typical real-world h is 7-15. */
#define BF_MAX_HASH_FUNCTIONS 100

/* varint worst-case sizes for serialization buffer math */
#define BF_VARINT32_MAX_BYTES 5
#define BF_VARINT64_MAX_BYTES 10
/* serialized header is 3 varint32s -- m, h, non_zero_count */
#define BF_SERIALIZE_HEADER_MAX_BYTES (3 * BF_VARINT32_MAX_BYTES)
/* each non-zero word is encoded as varint32 index + varint64 value */
#define BF_SERIALIZE_WORD_MAX_BYTES (BF_VARINT32_MAX_BYTES + BF_VARINT64_MAX_BYTES)

/* lemire's fast range reduction maps a uniform uint32_t hash into [0, range)
 * without integer division. it uses a single 64-bit multiply + shift.
 * not a true modulo but produces a uniform distribution, which is all
 * a bloom filter needs. */
static inline uint32_t bf_fast_range(const uint32_t hash, const uint32_t range)
{
    return (uint32_t)(((uint64_t)hash * (uint64_t)range) >> 32);
}

/**
 * bf_hash_inline
 * static inline version of bloom_filter_hash for internal use
 * allows compiler to inline in hot paths (add/contains)
 */
static inline uint32_t bf_hash_inline(const uint8_t *entry, const size_t size, const uint32_t seed)
{
    const uint32_t prime = BF_HASH_PRIME;
    const uint8_t *limit = entry + size;
    uint32_t h = seed ^ ((uint32_t)size * prime);

#if UINTPTR_MAX == UINT64_MAX
    while (entry + 8 <= limit)
    {
        uint32_t w1, w2;
        memcpy(&w1, entry, sizeof(w1));
        memcpy(&w2, entry + 4, sizeof(w2));
        entry += 8;
        h += w1;
        h *= prime;
        h ^= (h >> 16);
        h += w2;
        h *= prime;
        h ^= (h >> 16);
    }
    if (entry + 4 <= limit)
    {
        uint32_t w;
        memcpy(&w, entry, sizeof(w));
        entry += 4;
        h += w;
        h *= prime;
        h ^= (h >> 16);
    }
#else
    while (entry + 4 <= limit)
    {
        uint32_t w;
        memcpy(&w, entry, sizeof(w));
        entry += 4;
        h += w;
        h *= prime;
        h ^= (h >> 16);
    }
#endif

    switch (limit - entry)
    {
        case 3:
            h += (uint32_t)entry[2] << 16;
            /* fall through */
        case 2:
            h += (uint32_t)entry[1] << 8;
            /* fall through */
        case 1:
            h += entry[0];
            h *= prime;
            h ^= (h >> 24);
            break;
        default:
            break;
    }
    return h;
}

/* murmur3 fmix32 -- full-avalanche finalizer. applied by the v2 hash so even a
 * short key whose base hash had weak mixing produces well-spread index bits. */
static inline uint32_t bf_fmix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

/* v2 index hash base hash plus the fmix32 finalizer */
static inline uint32_t bf_hash_v2_inline(const uint8_t *entry, const size_t size,
                                         const uint32_t seed)
{
    return bf_fmix32(bf_hash_inline(entry, size, seed));
}

/* derive the two base hashes for a filter using the hash version it was built with,
 * so a filter is always queried with the same scheme that set its bits */
static inline void bf_derive_hashes(const bloom_filter_t *bf, const uint8_t *entry,
                                    const size_t size, uint32_t *h1, uint32_t *h2)
{
    if (bf->hash_version >= BF_HASH_VERSION_CURRENT)
    {
        *h1 = bf_hash_v2_inline(entry, size, 0);
        *h2 = bf_hash_v2_inline(entry, size, 1);
    }
    else
    {
        *h1 = bf_hash_inline(entry, size, 0);
        *h2 = bf_hash_inline(entry, size, 1);
    }
}

int bloom_filter_new(bloom_filter_t **bf, double p, const int n)
{
    /* reject non-finite p explicitly -- a NaN slips past the range comparisons
     * (all false for NaN) and would reach an undefined (unsigned)NaN cast below */
    if (!isfinite(p) || p <= 0.0 || p >= 1.0 || n <= 0)
    {
        return -1;
    }

    *bf = malloc(sizeof(bloom_filter_t));
    if (*bf == NULL)
    {
        return -1;
    }

    /**** we calculate the size of the bitset (m) using the formula
     ***  m = -n * ln(p) / (ln(2)^2)
     **
     */
    const double m_double = ceil(-((double)n) * log(p) / (M_LN2 * M_LN2));

    /* we validate m is within valid range */
    if (m_double <= 0.0 || m_double > (double)UINT32_MAX)
    {
        free(*bf);
        *bf = NULL;
        return -1;
    }

    (*bf)->m = (unsigned int)m_double;

    /* we calculate the number of hash functions (h) using the formula
     * h = (m / n) * ln(2)
     *
     */
    const double h_double = ceil(((double)(*bf)->m) / n * M_LN2);

    /* we validate h is reasonable -- typical real-world values are 7-15;
     * BF_MAX_HASH_FUNCTIONS rejects pathological configs from FP edge cases */
    if (h_double <= 0.0 || h_double > (double)BF_MAX_HASH_FUNCTIONS)
    {
        free(*bf);
        *bf = NULL;
        return -1;
    }

    (*bf)->h = (unsigned int)h_double;

    /* we calculate number of 64-bit words needed for packed bitset */
    (*bf)->size_in_words = ((*bf)->m + BF_BITS_PER_WORD - 1) / BF_BITS_PER_WORD;

    /* we validate size_in_words to prevent overflow */
    if ((*bf)->size_in_words == 0 || (*bf)->size_in_words > UINT32_MAX / sizeof(uint64_t))
    {
        free(*bf);
        *bf = NULL;
        return -1;
    }

    /* we alloc memory for the packed bitset and initialize it to 0 */
    (*bf)->bitset = calloc((size_t)(*bf)->size_in_words, sizeof(uint64_t));
    if ((*bf)->bitset == NULL)
    {
        free(*bf);
        *bf = NULL;
        return -1;
    }

    /* freshly built filters use the current (best) index hash */
    (*bf)->hash_version = BF_HASH_VERSION_CURRENT;

    return 0;
}

void bloom_filter_add(const bloom_filter_t *bf, const uint8_t *entry, const size_t size)
{
    if (BF_UNLIKELY(bf == NULL)) return;
    if (BF_UNLIKELY(entry == NULL || size == 0)) return;

    /* we cache struct fields to avoid repeated memory access */
    const unsigned int h = bf->h;
    const unsigned int m = bf->m;
    uint64_t *const bitset = bf->bitset;

    uint32_t h1, h2;
    bf_derive_hashes(bf, entry, size, &h1, &h2);

    for (unsigned int i = 0; i < h; i++)
    {
        const uint32_t hash = h1 + i * h2;
        const uint32_t index = bf_fast_range(hash, m);
        BF_SET_BIT(bitset, index);
    }
}

int bloom_filter_contains(const bloom_filter_t *bf, const uint8_t *entry, const size_t size)
{
    if (BF_UNLIKELY(bf == NULL)) return -1;
    if (BF_UNLIKELY(entry == NULL || size == 0)) return -1;

    /* we cache struct fields to avoid repeated memory access */
    const unsigned int h = bf->h;
    const unsigned int m = bf->m;
    const uint64_t *const bitset = bf->bitset;

    /* k-mitzenmacher + fast range reduction
     * 2 hashes + h cheap probes instead of h full hashes + h divisions */
    uint32_t h1, h2;
    bf_derive_hashes(bf, entry, size, &h1, &h2);

    for (unsigned int i = 0; i < h; i++)
    {
        const uint32_t hash = h1 + i * h2;
        const uint32_t index = bf_fast_range(hash, m);
        if (BF_LIKELY(!BF_GET_BIT(bitset, index)))
        {
            return 0; /* definitely not in set */
        }
    }
    return 1; /* probably in set */
}

int bloom_filter_is_full(const bloom_filter_t *bf)
{
    if (BF_UNLIKELY(bf == NULL)) return -1;
    if (BF_UNLIKELY(bf->bitset == NULL)) return -1;

    const uint64_t *const bitset = bf->bitset;
    const unsigned int size_in_words = bf->size_in_words;

    /*** prevents `size_in_words - 1` from underflowing as unsigned.
     **  the constructor rejects size_in_words == 0, but a future refactor or a
     *   deserialized filter that bypasses the constructor could produce one. */
    if (BF_UNLIKELY(size_in_words == 0)) return -1;

    /* we check if all words are fully set */
    for (unsigned int i = 0; i < size_in_words - 1; i++)
    {
        if (bitset[i] != UINT64_MAX)
        {
            return 0;
        }
    }

    /* we check last word (may be partial) */
    const unsigned int remaining_bits = bf->m % BF_BITS_PER_WORD;
    if (remaining_bits == 0)
    {
        return (bitset[size_in_words - 1] == UINT64_MAX);
    }
    const uint64_t mask = (1ULL << remaining_bits) - 1;
    return ((bitset[size_in_words - 1] & mask) == mask);
}

unsigned int bloom_filter_hash(const uint8_t *entry, const size_t size, const int seed)
{
    if (BF_UNLIKELY(entry == NULL || size == 0)) return 0;

    return bf_hash_inline(entry, size, (uint32_t)seed);
}

uint8_t *bloom_filter_serialize(const bloom_filter_t *bf, size_t *out_size)
{
    if (bf == NULL)
    {
        return NULL;
    }

    /* we count non-zero words for sparse encoding */
    unsigned int non_zero_count = 0;
    for (unsigned int i = 0; i < bf->size_in_words; i++)
    {
        if (bf->bitset[i] != 0) non_zero_count++;
    }

    /* we allocate worst-case size
     * -- header            3 varint32s (m, h, non_zero_count)
     * -- sparse data       each non-zero word = varint32 index + varint64 value
     */
    const size_t max_size = BF_SERIALIZE_VERSION_BYTES + BF_SERIALIZE_HEADER_MAX_BYTES +
                            (size_t)non_zero_count * BF_SERIALIZE_WORD_MAX_BYTES;
    uint8_t *buffer = malloc(max_size);
    if (buffer == NULL)
    {
        return NULL;
    }

    uint8_t *ptr = buffer;

    /* any non-legacy filter leads with a 0x00 sentinel (impossible for a v1 filter,
     * whose first byte is varint32(m) with m >= 1) followed by the hash version
     * byte, so deserialize routes the filter back to the hash that built it. keyed
     * off "> LEGACY" rather than a specific version so a future bump stays recorded. */
    if (bf->hash_version > BF_HASH_VERSION_LEGACY)
    {
        *ptr++ = BF_SERIALIZE_VERSION_SENTINEL;
        *ptr++ = (uint8_t)bf->hash_version;
    }

    /* we write header with varint encoding */
    ptr = encode_varint32(ptr, (uint32_t)bf->m);
    ptr = encode_varint32(ptr, (uint32_t)bf->h);
    ptr = encode_varint32(ptr, (uint32_t)non_zero_count);

    /* we write sparse bitset -- only non-zero words with their indices */
    for (unsigned int i = 0; i < bf->size_in_words; i++)
    {
        if (bf->bitset[i] != 0)
        {
            ptr = encode_varint32(ptr, (uint32_t)i);   /* word index */
            ptr = encode_varint64(ptr, bf->bitset[i]); /* word value */
        }
    }

    /* we return actual size used, no realloc shrink since the overallocation
     * is at most 15 bytes per non-zero word and glibc typically won't release it anyway */
    *out_size = ptr - buffer;
    return buffer;
}

/* bounded varint decoders -- read at most the bytes a 32/64-bit value can occupy
 * and never past `end`. return 0 and advance *pp on success, -1 on truncation or a
 * malformed (unterminated) varint. these replace the unbounded compat decoders on
 * the parse-untrusted-bytes path so a corrupt buffer cannot drive an over-read. */
static int bf_get_varint32(const uint8_t **pp, const uint8_t *end, uint32_t *out)
{
    uint32_t result = 0;
    int shift = 0;
    const uint8_t *p = *pp;
    for (int i = 0; i < BF_VARINT32_MAX_BYTES; i++)
    {
        if (p >= end) return -1;
        const uint8_t b = *p++;
        result |= (uint32_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u))
        {
            *pp = p;
            *out = result;
            return 0;
        }
        shift += 7;
    }
    return -1; /* no terminator within the max byte budget */
}

static int bf_get_varint64(const uint8_t **pp, const uint8_t *end, uint64_t *out)
{
    uint64_t result = 0;
    int shift = 0;
    const uint8_t *p = *pp;
    for (int i = 0; i < BF_VARINT64_MAX_BYTES; i++)
    {
        if (p >= end) return -1;
        const uint8_t b = *p++;
        result |= (uint64_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u))
        {
            *pp = p;
            *out = result;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

bloom_filter_t *bloom_filter_deserialize(const uint8_t *data, const size_t len)
{
    if (data == NULL || len == 0)
    {
        return NULL;
    }

    const uint8_t *ptr = data;
    const uint8_t *const end = data + len;

    /* a leading 0x00 marks the versioned format (v1 can never start with 0x00,
     * its first field is varint32(m) with m >= 1). absent it, this is a legacy
     * v1 filter that must keep being queried with the v1 hash. */
    unsigned int hash_version = BF_HASH_VERSION_LEGACY;
    if (ptr[0] == BF_SERIALIZE_VERSION_SENTINEL)
    {
        if (end - ptr < BF_SERIALIZE_VERSION_BYTES) return NULL; /* sentinel + version */
        ptr++;                                                   /* skip sentinel */
        hash_version = (unsigned int)*ptr++;                     /* read hash version */
        /* reject an unknown version -- querying with an undefined scheme would
         * silently produce false negatives on an otherwise valid filter */
        if (hash_version < BF_HASH_VERSION_LEGACY || hash_version > BF_HASH_VERSION_CURRENT)
        {
            return NULL;
        }
    }

    /* we read header with bounded varint decoding */
    uint32_t m_u32, h_u32, non_zero_count;
    if (bf_get_varint32(&ptr, end, &m_u32) != 0) return NULL;
    if (bf_get_varint32(&ptr, end, &h_u32) != 0) return NULL;
    if (bf_get_varint32(&ptr, end, &non_zero_count) != 0) return NULL;

    const unsigned int m = m_u32;
    const unsigned int h = h_u32;

    /* we validate deserialized values */
    if (m == 0 || h == 0)
    {
        return NULL;
    }

    /* we check for potential integer overflow in size calculation */
    if (m > UINT32_MAX - BF_BITS_PER_WORD)
    {
        return NULL;
    }

    const unsigned int size_in_words = (m + BF_BITS_PER_WORD - 1) / BF_BITS_PER_WORD;

    /* a valid filter never has more non-zero words than total words; reject a
     * corrupt count up front so the loop below can't be driven past the buffer */
    if (non_zero_count > size_in_words)
    {
        return NULL;
    }

    /* we allocate and zero-initialize bitset */
    uint64_t *bitset = calloc((size_t)size_in_words, sizeof(uint64_t));
    if (bitset == NULL)
    {
        return NULL;
    }

    /* we read sparse bitset -- only non-zero words */
    for (uint32_t i = 0; i < non_zero_count; i++)
    {
        uint32_t index;
        uint64_t value;
        if (bf_get_varint32(&ptr, end, &index) != 0 || bf_get_varint64(&ptr, end, &value) != 0)
        {
            free(bitset);
            return NULL;
        }

        /* we validate index is within bounds */
        if (index >= (uint32_t)size_in_words)
        {
            free(bitset);
            return NULL;
        }

        bitset[index] = value;
    }

    bloom_filter_t *bf = malloc(sizeof(bloom_filter_t));
    if (bf == NULL)
    {
        free(bitset);
        return NULL;
    }

    bf->m = m;
    bf->h = h;
    bf->bitset = bitset;
    bf->size_in_words = size_in_words;
    bf->hash_version = hash_version;

    return bf;
}

void bloom_filter_free(bloom_filter_t *bf)
{
    if (bf == NULL)
    {
        return;
    }

    free(bf->bitset);
    free(bf);
}

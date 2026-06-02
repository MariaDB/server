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
#ifndef __BLOOM_FILTER_H__
#define __BLOOM_FILTER_H__
#include "compat.h"

/**
 * bloom_filter_t
 * bloom filter struct (optimized with packed bits)
 * @param bitset the bloom filter bitset (packed in uint64_t words)
 * @param m the size of the bloom filter in bits
 * @param h the number of hash functions
 * @param size_in_words number of uint64_t words in bitset
 * @param hash_version index-derivation hash version 1 = legacy, 2 = fmix-finalized
 *                     (better avalanche / lower FPR on short keys). carried with the
 *                     filter and honored by add/contains so on-disk filters built with
 *                     an older hash keep querying with that same hash (no false negatives).
 *
 * a filter is single-writer during build (add) and immutable after.
 * once frozen it may be queried (contains) concurrently by any number of threads --
 * the query path is pure-read. add() concurrent with add()/contains() is a data race
 * (the bitset words are non-atomic read-modify-write) and is not supported.
 */
typedef struct
{
    uint64_t *bitset;
    unsigned int m;
    unsigned int h;
    unsigned int size_in_words;
    unsigned int hash_version;
} bloom_filter_t;

/**
 * bloom_filter_new
 * creates a new bloom filter
 * @param bf the bloom filter to create
 * @param p the false positive rate
 * @param n the number of elements
 * @return 0 if successful, -1 if not
 */
int bloom_filter_new(bloom_filter_t **bf, double p, int n);

/**
 * bloom_filter_add
 * adds an entry to the bloom filter
 * @param bf the bloom filter to add to
 * @param entry the entry to add
 * @param size the size of the entry
 */
void bloom_filter_add(const bloom_filter_t *bf, const uint8_t *entry, size_t size);

/**
 * bloom_filter_contains
 * checks if an entry is in the bloom filter
 * @param bf the bloom filter to check
 * @param entry the entry to check
 * @param size the size of the entry
 * @return 1 if the entry is in the bloom filter, 0 if not
 */
int bloom_filter_contains(const bloom_filter_t *bf, const uint8_t *entry, size_t size);

/**
 * bloom_filter_is_full
 * checks if the bloom filter is full
 * @param bf the bloom filter to check
 * @return 1 if the bloom filter is full, 0 if not
 */
int bloom_filter_is_full(const bloom_filter_t *bf);

/**
 * bloom_filter_hash
 * hashes an entry
 * @param entry the entry to hash
 * @param size the size of the entry
 * @param seed the seed for the hash
 * @return the hash
 */
unsigned int bloom_filter_hash(const uint8_t *entry, size_t size, int seed);

/**
 * bloom_filter_serialize
 * serializes a bloom filter to compact binary format using:
 * -- varint encoding for header fields (m, h, non_zero_count)
 * -- sparse encoding     -- only stores non-zero words with their indices
 * typical space savings  -- 70-90% for low fill rates (< 50%)
 * @param bf the bloom filter to serialize
 * @param out_size the size of the serialized bloom filter
 * @return the serialized bloom filter
 */
uint8_t *bloom_filter_serialize(const bloom_filter_t *bf, size_t *out_size);

/**
 * bloom_filter_deserialize
 * deserializes a bloom filter. every field read is bounded by len, so a
 * truncated or corrupt buffer is rejected (NULL) rather than over-read.
 * @param data the serialized bloom filter
 * @param len the length in bytes of the serialized buffer
 * @return the deserialized bloom filter, or NULL on malformed/truncated input
 */
bloom_filter_t *bloom_filter_deserialize(const uint8_t *data, size_t len);

/**
 * bloom_filter_free
 * frees a bloom filter
 * @param bf the bloom filter to free
 */
void bloom_filter_free(bloom_filter_t *bf);

#endif /* __BLOOM_FILTER_H__ */
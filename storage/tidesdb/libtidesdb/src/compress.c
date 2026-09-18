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

#include "compress.h"

/* third-party backend headers, included only when the backend is compiled in. the TIDESDB_HAVE_*
 * macros are PRIVATE compile definitions set by CMake from the -DTIDESDB_WITH_* options, so a build
 * can drop any subset (or all) of them and still produce a working library that supports the
 * remaining algorithms plus TDB_COMPRESS_NONE. */
#ifdef TIDESDB_HAVE_LZ4
#include <lz4.h>
#endif
#ifdef TIDESDB_HAVE_SNAPPY
#include <snappy-c.h>
#endif
#ifdef TIDESDB_HAVE_ZSTD
#include <zstd.h>
#endif

/* the compression_algorithm enum values are an on-disk + ABI contract, they are written into
 * sstable/vlog metadata, so they must never change, and the duplicate enum in db.h (the
 * standalone FFI header, which cannot include this header) MUST hold identical values. pin them
 * at compile time so any drift in compress.h fails the build; db.h carries the matching contract
 * comment. the asserts are unconditional -- the enumerators exist regardless of which backends are
 * compiled in. guarded on C11 so older/non-conforming C front-ends still compile. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(TDB_COMPRESS_NONE == 0, "compression_algorithm wire drift: NONE must be 0");
_Static_assert(TDB_COMPRESS_SNAPPY == 1, "compression_algorithm wire drift: SNAPPY must be 1");
_Static_assert(TDB_COMPRESS_LZ4 == 2, "compression_algorithm wire drift: LZ4 must be 2");
_Static_assert(TDB_COMPRESS_ZSTD == 3, "compression_algorithm wire drift: ZSTD must be 3");
_Static_assert(TDB_COMPRESS_LZ4_FAST == 4, "compression_algorithm wire drift: LZ4_FAST must be 4");
#endif

int tidesdb_compression_available(const compression_algorithm type)
{
    switch (type)
    {
        case TDB_COMPRESS_NONE:
            return 1;
#ifdef TIDESDB_HAVE_SNAPPY
        case TDB_COMPRESS_SNAPPY:
            return 1;
#endif
#ifdef TIDESDB_HAVE_LZ4
        case TDB_COMPRESS_LZ4:
        case TDB_COMPRESS_LZ4_FAST:
            return 1;
#endif
#ifdef TIDESDB_HAVE_ZSTD
        case TDB_COMPRESS_ZSTD:
            return 1;
#endif
        default:
            return 0;
    }
}

uint8_t *compress_data(const uint8_t *data, const size_t data_size, size_t *compressed_size,
                       const compression_algorithm type)
{
    uint8_t *compressed_data = NULL;

    if (TDB_UNLIKELY(!data))
    {
        return NULL;
    }

    switch (type)
    {
#ifdef TIDESDB_HAVE_SNAPPY
        case TDB_COMPRESS_SNAPPY:
        {
            *compressed_size = snappy_max_compressed_length(data_size);
            const size_t total_size = *compressed_size + sizeof(uint64_t);
            compressed_data = malloc(total_size);
            if (TDB_UNLIKELY(!compressed_data)) return NULL;

            encode_uint64_le_compat(compressed_data, data_size);

            size_t actual_size = *compressed_size;
            if (TDB_UNLIKELY(snappy_compress((const char *)data, data_size,
                                             (char *)(compressed_data + sizeof(uint64_t)),
                                             &actual_size) != SNAPPY_OK))
            {
                free(compressed_data);
                return NULL;
            }

            *compressed_size = actual_size + sizeof(uint64_t);
            break;
        }
#endif

#ifdef TIDESDB_HAVE_LZ4
        case TDB_COMPRESS_LZ4:
        case TDB_COMPRESS_LZ4_FAST:
        {
            *compressed_size = (size_t)LZ4_compressBound((int)data_size);
            const size_t total_size = *compressed_size + sizeof(uint64_t);
            compressed_data = malloc(total_size);
            if (TDB_UNLIKELY(!compressed_data)) return NULL;

            encode_uint64_le_compat(compressed_data, data_size);

            /* unified LZ4 path-- acceleration=1 for default, acceleration=2 for fast */
            const int acceleration = (type == TDB_COMPRESS_LZ4_FAST) ? 2 : 1;
            const int lz4_result =
                LZ4_compress_fast((const char *)data, (char *)(compressed_data + sizeof(uint64_t)),
                                  (int)data_size, (int)*compressed_size, acceleration);
            if (TDB_UNLIKELY(lz4_result <= 0))
            {
                free(compressed_data);
                return NULL;
            }

            *compressed_size = (size_t)lz4_result + sizeof(uint64_t);
            break;
        }
#endif

#ifdef TIDESDB_HAVE_ZSTD
        case TDB_COMPRESS_ZSTD:
        {
            *compressed_size = ZSTD_compressBound(data_size);
            const size_t total_size = *compressed_size + sizeof(uint64_t);
            compressed_data = malloc(total_size);
            if (TDB_UNLIKELY(!compressed_data)) return NULL;

            encode_uint64_le_compat(compressed_data, data_size);

            const size_t actual_size = ZSTD_compress(compressed_data + sizeof(uint64_t),
                                                     *compressed_size, data, data_size, 1);
            if (TDB_UNLIKELY(ZSTD_isError(actual_size)))
            {
                free(compressed_data);
                return NULL;
            }

            *compressed_size = actual_size + sizeof(uint64_t);
            break;
        }
#endif

        default:
            return NULL;
    }

    /* shrink buffer to actual compressed size to save memory and improve cache
     * when the compressed data is stored or transmitted */
    if (TDB_LIKELY(compressed_data != NULL))
    {
        uint8_t *shrunk = realloc(compressed_data, *compressed_size);
        if (TDB_LIKELY(shrunk != NULL))
        {
            compressed_data = shrunk;
        }
    }

    return compressed_data;
}

uint8_t *decompress_data(const uint8_t *data, const size_t data_size, size_t *decompressed_size,
                         const compression_algorithm type)
{
    uint8_t *decompressed_data = NULL;

    if (TDB_UNLIKELY(!data)) return NULL;

    switch (type)
    {
#ifdef TIDESDB_HAVE_SNAPPY
        case TDB_COMPRESS_SNAPPY:
        {
            if (TDB_UNLIKELY(data_size < sizeof(uint64_t)))
            {
                return NULL;
            }

            const uint64_t original_size = decode_uint64_le_compat(data);

            if (TDB_UNLIKELY(original_size > UINT32_MAX))
            {
                return NULL;
            }

            *decompressed_size = (size_t)original_size;

            decompressed_data = malloc(*decompressed_size);
            if (TDB_UNLIKELY(!decompressed_data)) return NULL;

            if (TDB_UNLIKELY(snappy_uncompress((const char *)(data + sizeof(uint64_t)),
                                               data_size - sizeof(uint64_t),
                                               (char *)decompressed_data,
                                               decompressed_size) != SNAPPY_OK))
            {
                free(decompressed_data);
                return NULL;
            }
            /* verify produced length matches the size prefix, mirroring the LZ4/ZSTD branches.
             * snappy_uncompress can succeed with a shorter output that still fits the buffer,
             * which would otherwise pass silently. */
            if (TDB_UNLIKELY(*decompressed_size != (size_t)original_size))
            {
                free(decompressed_data);
                return NULL;
            }
            break;
        }
#endif

#ifdef TIDESDB_HAVE_LZ4
        case TDB_COMPRESS_LZ4:
        case TDB_COMPRESS_LZ4_FAST:
        {
            if (TDB_UNLIKELY(data_size < sizeof(uint64_t)))
            {
                return NULL;
            }

            const uint64_t original_size = decode_uint64_le_compat(data);

            if (TDB_UNLIKELY(original_size > UINT32_MAX))
            {
                return NULL;
            }

            *decompressed_size = (size_t)original_size;

            decompressed_data = malloc(*decompressed_size);
            if (TDB_UNLIKELY(!decompressed_data)) return NULL;

            const int lz4_result = LZ4_decompress_safe(
                (const char *)(data + sizeof(uint64_t)), (char *)decompressed_data,
                (int)(data_size - sizeof(uint64_t)), (int)*decompressed_size);
            if (TDB_UNLIKELY(lz4_result < 0 || lz4_result != (int)*decompressed_size))
            {
                free(decompressed_data);
                return NULL;
            }
            break;
        }
#endif

#ifdef TIDESDB_HAVE_ZSTD
        case TDB_COMPRESS_ZSTD:
        {
            if (TDB_UNLIKELY(data_size < sizeof(uint64_t)))
            {
                return NULL;
            }

            const uint64_t original_size = decode_uint64_le_compat(data);

            if (TDB_UNLIKELY(original_size > UINT32_MAX))
            {
                return NULL;
            }

            *decompressed_size = (size_t)original_size;

            decompressed_data = malloc(*decompressed_size);
            if (TDB_UNLIKELY(!decompressed_data)) return NULL;

            const size_t zstd_result =
                ZSTD_decompress(decompressed_data, *decompressed_size, data + sizeof(uint64_t),
                                data_size - sizeof(uint64_t));
            if (TDB_UNLIKELY(ZSTD_isError(zstd_result) || zstd_result != *decompressed_size))
            {
                free(decompressed_data);
                return NULL;
            }
            break;
        }
#endif

        default:
            return NULL;
    }

    return decompressed_data;
}
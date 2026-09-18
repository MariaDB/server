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
#ifndef __COMPRESS_H__
#define __COMPRESS_H__

#include "compat.h"

/* snappy, lz4 and zstd are the supported compression backends. each is optional at build time --
 * the -DTIDESDB_WITH_{SNAPPY,LZ4,ZSTD} CMake options (default ON) select which are compiled in, and
 * a build with all three off has no compression dependencies at all (TDB_COMPRESS_NONE only). the
 * third-party headers are included only in compress.c, guarded by the TIDESDB_HAVE_* build macros,
 * so a consumer of the installed library does not need the compression dev headers just to include
 * this file.
 * ABI/on-disk contract -- these numeric values are persisted in sstable/vlog metadata and are
 * duplicated in db.h (the standalone FFI header). the two copies MUST stay identical; compress.c
 * pins them with _Static_assert to catch drift at build time. every enumerator is defined
 * regardless of which backends are compiled in, so an sstable's algorithm id is always
 * recognizable -- an unavailable backend yields a clean runtime error, not an unknown algorithm. */
typedef enum
{
    TDB_COMPRESS_NONE = 0,
    TDB_COMPRESS_SNAPPY = 1,
    TDB_COMPRESS_LZ4 = 2,
    TDB_COMPRESS_ZSTD = 3,
    TDB_COMPRESS_LZ4_FAST = 4,
} compression_algorithm;

/**
 * compress_data
 * compresses data using the specified compression algorithm
 * @param data the data to compress
 * @param data_size the size of the data
 * @param compressed_size the size of the compressed data
 * @param type the compression algorithm to use
 * @return newly allocated compressed data (caller frees), or NULL on failure (bad args,
 *         allocation failure, unsupported type, or a codec error)
 */
uint8_t *compress_data(const uint8_t *data, size_t data_size, size_t *compressed_size,
                       compression_algorithm type);

/**
 * decompress_data
 * decompresses data using the specified compression algorithm
 * @param data the data to decompress
 * @param data_size the size of the data
 * @param decompressed_size the size of the decompressed data
 * @param type the compression algorithm to use
 * @return newly allocated decompressed data (caller frees), or NULL on failure (bad args,
 *         allocation failure, or a codec/corruption error)
 */
uint8_t *decompress_data(const uint8_t *data, size_t data_size, size_t *decompressed_size,
                         compression_algorithm type);

/**
 * tidesdb_compression_available
 * report whether a compression backend is compiled into this build. TDB_COMPRESS_NONE is always
 * available; the rest depend on the TIDESDB_HAVE_* build flags (which mirror the -DTIDESDB_WITH_*
 * CMake options). lets callers reject an unsupported algorithm up front instead of failing at
 * compress/flush time.
 * @param type the compression algorithm to query
 * @return 1 if the algorithm can be used in this build, 0 otherwise
 */
int tidesdb_compression_available(compression_algorithm type);

#endif /* __COMPRESS_H__ */

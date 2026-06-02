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
#include <lz4.h>
#ifndef __sun
#include <snappy-c.h>
#endif
#include <zstd.h>

#include "compat.h"

/* snappy, lz4, zstd supported to use for compression purposes */
/* snappy is not available on SunOS/OmniOS/Illumos */
/* ABI/on-disk contract, these numeric values are persisted in sstable/vlog metadata and are
 * duplicated in db.h (the standalone FFI header). the two copies MUST stay identical; compress.c
 * pins these values with _Static_assert to catch drift at build time. */
typedef enum
{
    TDB_COMPRESS_NONE = 0,
#ifndef __sun
    TDB_COMPRESS_SNAPPY = 1,
#endif
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
 * @return the compressed data
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
 * @return the decompressed data
 */
uint8_t *decompress_data(const uint8_t *data, size_t data_size, size_t *decompressed_size,
                         compression_algorithm type);

#endif /* __COMPRESS_H__ */

/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include <my_global.h>
#include <toku_portability.h>
#include <util/scoped_malloc.h>

#include <zlib.h>
#include <lzma.h>
#include <snappy.h>

#include "compress.h"
#include "memory.h"
#include "quicklz.h"
#include "toku_assert.h"

static inline enum toku_compression_method
normalize_compression_method(enum toku_compression_method method)
// Effect: resolve "friendly" names like "fast" and "small" into their real values.
{
    switch (method) {
    case TOKU_DEFAULT_COMPRESSION_METHOD:
    case TOKU_FAST_COMPRESSION_METHOD:
        return TOKU_QUICKLZ_METHOD;
    case TOKU_SMALL_COMPRESSION_METHOD:
        return TOKU_LZMA_METHOD;
    default:
        return method; // everything else is fine
    }
}

size_t toku_compress_bound (enum toku_compression_method a, size_t size)
// See compress.h for the specification of this function.
{
    a = normalize_compression_method(a);
    switch (a) {
    case TOKU_NO_COMPRESSION:
        return size + 1;
    case TOKU_LZMA_METHOD:
	return 1+lzma_stream_buffer_bound(size); // We need one extra for the rfc1950-style header byte (bits -03 are TOKU_LZMA_METHOD (1), bits 4-7 are the compression level)
    case TOKU_QUICKLZ_METHOD:
        return size+400 + 1;  // quicklz manual says 400 bytes is enough.  We need one more byte for the rfc1950-style header byte.  bits 0-3 are 9, bits 4-7 are the QLZ_COMPRESSION_LEVEL.
    case TOKU_ZLIB_METHOD:
        return compressBound (size);
    case TOKU_ZLIB_WITHOUT_CHECKSUM_METHOD:
        return 2+deflateBound(nullptr, size); // We need one extra for the rfc1950-style header byte, and one extra to store windowBits (a bit over cautious about future upgrades maybe).
    case TOKU_SNAPPY_METHOD:
        return (1 + snappy::MaxCompressedLength(size));
    default:
        break;
    }
    // fall through for bad enum (thus compiler can warn us if we didn't use all the enums
    assert(0); return 0;
}

void toku_compress (enum toku_compression_method a,
                    // the following types and naming conventions come from zlib.h
                    Bytef       *dest,   uLongf *destLen,
                    const Bytef *source, uLong   sourceLen)
// See compress.h for the specification of this function.
{
    static const int zlib_compression_level = 5;
    static const int zlib_without_checksum_windowbits = -15;

    a = normalize_compression_method(a);
    switch (a) {
    case TOKU_NO_COMPRESSION:
        dest[0] = TOKU_NO_COMPRESSION;
        memcpy(dest + 1, source, sourceLen);
        *destLen = sourceLen + 1;
        return;
    case TOKU_ZLIB_METHOD: {
        int r = compress2(dest, destLen, source, sourceLen, zlib_compression_level);
        assert(r == Z_OK);
        assert((dest[0]&0xF) == TOKU_ZLIB_METHOD);
        return;
    }
    case  TOKU_QUICKLZ_METHOD: {
        if (sourceLen==0) {
            // quicklz requires at least one byte, so we handle this ourselves
            assert(1 <= *destLen);
            *destLen = 1;
        } else {
            toku::scoped_calloc qsc_buf(sizeof(qlz_state_compress));
            qlz_state_compress *qsc = reinterpret_cast<qlz_state_compress *>(qsc_buf.get());
            size_t actual_destlen = qlz_compress(source, (char*)(dest+1), sourceLen, qsc);
            assert(actual_destlen + 1 <= *destLen);
            // add one for the rfc1950-style header byte.
            *destLen = actual_destlen + 1;
        }
        // Fill in that first byte
        dest[0] = TOKU_QUICKLZ_METHOD + (QLZ_COMPRESSION_LEVEL << 4);
        return;
    }
    case TOKU_LZMA_METHOD: {
        const int lzma_compression_level = 2;
        if (sourceLen==0) {
            // lzma version 4.999 requires at least one byte, so we'll do it ourselves.
            assert(1<=*destLen);
            *destLen = 1;
        } else {
            size_t out_pos = 1;
            lzma_ret r = lzma_easy_buffer_encode(lzma_compression_level,
                                                 LZMA_CHECK_NONE, NULL,
                                                 source, sourceLen,
                                                 dest, &out_pos, *destLen);
            assert(out_pos < *destLen);
            if (r != LZMA_OK) {
                fprintf(stderr, "lzma_easy_buffer_encode() returned %d\n", (int) r);
            }
            assert(r==LZMA_OK);
            *destLen = out_pos;
        }
        dest[0] = TOKU_LZMA_METHOD + (lzma_compression_level << 4);
        return;
    }
    case TOKU_ZLIB_WITHOUT_CHECKSUM_METHOD: {
        z_stream strm;
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.next_in = const_cast<Bytef *>(source);
        strm.avail_in = sourceLen;
        int r = deflateInit2(&strm, zlib_compression_level, Z_DEFLATED,
                             zlib_without_checksum_windowbits, 8, Z_DEFAULT_STRATEGY);
        lazy_assert(r == Z_OK);
        strm.next_out = dest + 2;
        strm.avail_out = *destLen - 2;
        r = deflate(&strm, Z_FINISH);
        lazy_assert(r == Z_STREAM_END);
        r = deflateEnd(&strm);
        lazy_assert(r == Z_OK);
        *destLen = strm.total_out + 2;
        dest[0] = TOKU_ZLIB_WITHOUT_CHECKSUM_METHOD + (zlib_compression_level << 4);
        dest[1] = zlib_without_checksum_windowbits;
        return;
    }
    case TOKU_SNAPPY_METHOD: {
        size_t tmp_dest= *destLen;
        snappy::RawCompress((char*)source, sourceLen, (char*)dest + 1,
                            &tmp_dest);
        *destLen= tmp_dest + 1;
        dest[0] = TOKU_SNAPPY_METHOD;
        return;
    }
    default:
        break;
    }
    // default fall through to error.
    assert(0);
}

void toku_decompress (Bytef       *dest,   uLongf destLen,
                      const Bytef *source, uLongf sourceLen)
// See compress.h for the specification of this function.
{
    assert(sourceLen>=1); // need at least one byte for the RFC header.
    switch (source[0] & 0xF) {
    case TOKU_NO_COMPRESSION:
        memcpy(dest, source + 1, sourceLen - 1);
        return;
    case TOKU_ZLIB_METHOD: {
        uLongf actual_destlen = destLen;
        int r = uncompress(dest, &actual_destlen, source, sourceLen);
        assert(r == Z_OK);
        assert(actual_destlen == destLen);
        return;
    }
    case TOKU_QUICKLZ_METHOD:
        if (sourceLen>1) {
            toku::scoped_calloc state_buf(sizeof(qlz_state_decompress));
            qlz_state_decompress *qsd = reinterpret_cast<qlz_state_decompress *>(state_buf.get());
            uLongf actual_destlen = qlz_decompress((char*)source+1, dest, qsd);
            assert(actual_destlen == destLen);
        } else {
            // length 1 means there is no data, so do nothing.
            assert(destLen==0);
        }
        return;
    case TOKU_LZMA_METHOD: {
        if (sourceLen>1) {
            uint64_t memlimit = UINT64_MAX;
            size_t out_pos = 0;
            size_t in_pos  = 1;
            lzma_ret r = lzma_stream_buffer_decode(&memlimit,  // memlimit, use UINT64_MAX to disable this check
                                                   0,          // flags
                                                   NULL,       // allocator
                                                   source, &in_pos, sourceLen,
                                                   dest, &out_pos, destLen);
            assert(r==LZMA_OK);
            assert(out_pos == destLen);
        } else {
            // length 1 means there is no data, so do nothing.
            assert(destLen==0);
        }
        return;
    }
    case TOKU_ZLIB_WITHOUT_CHECKSUM_METHOD: {
        z_stream strm;
        strm.next_in = const_cast<Bytef *>(source + 2);
        strm.avail_in = sourceLen - 2;
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        int8_t windowBits = source[1];
        int r = inflateInit2(&strm, windowBits);
        lazy_assert(r == Z_OK);
        strm.next_out = dest;
        strm.avail_out = destLen;
        r = inflate(&strm, Z_FINISH);
        lazy_assert(r == Z_STREAM_END);
        r = inflateEnd(&strm);
        lazy_assert(r == Z_OK);
        return;
    }
    case TOKU_SNAPPY_METHOD: {
        bool r = snappy::RawUncompress((char*)source + 1, sourceLen - 1, (char*)dest);
        assert(r);
        return;
    }
    }
    // default fall through to error.
    assert(0);
}

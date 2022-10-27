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

#pragma once

#include <string.h>

#include "portability/memory.h"
#include "portability/toku_assert.h"
#include "portability/toku_htonl.h"
#include "portability/toku_portability.h"
#include "util/memarena.h"

struct rbuf {
    unsigned char *buf;
    unsigned int  size;
    unsigned int  ndone;
};
#define RBUF_INITIALIZER ((struct rbuf){.buf = NULL, .size=0, .ndone=0})

static inline void rbuf_init(struct rbuf *r, unsigned char *buf, unsigned int size) {
    r->buf = buf;
    r->size = size;
    r->ndone = 0;
}

static inline unsigned int rbuf_get_roffset(struct rbuf *r) {
    return r->ndone;
}

static inline unsigned char rbuf_char (struct rbuf *r) {
    assert(r->ndone<r->size);
    return r->buf[r->ndone++];
}

static inline void rbuf_ma_uint8_t (struct rbuf *r, memarena *ma __attribute__((__unused__)), uint8_t *num) {
    *num = rbuf_char(r);
}

static inline void rbuf_ma_bool (struct rbuf *r, memarena *ma __attribute__((__unused__)), bool *b) {
    uint8_t n = rbuf_char(r);
    *b = (n!=0);
}

//Read an int that MUST be in network order regardless of disk order
static unsigned int rbuf_network_int (struct rbuf *r) __attribute__((__unused__));
static unsigned int rbuf_network_int (struct rbuf *r) {
    assert(r->ndone+4 <= r->size);
    uint32_t result = toku_ntohl(*(uint32_t*)(r->buf+r->ndone)); // This only works on machines where unaligned loads are OK.
    r->ndone+=4;
    return result;
}

static unsigned int rbuf_int (struct rbuf *r) {
#if 1
    assert(r->ndone+4 <= r->size);
    uint32_t result = toku_dtoh32(*(uint32_t*)(r->buf+r->ndone)); // This only works on machines where unaligned loads are OK.
    r->ndone+=4;
    return result;
#else
    unsigned char c0 = rbuf_char(r);
    unsigned char c1 = rbuf_char(r);
    unsigned char c2 = rbuf_char(r);
    unsigned char c3 = rbuf_char(r);
    return ((c0<<24)|
	    (c1<<16)|
	    (c2<<8)|
	    (c3<<0));
#endif
}

static inline void rbuf_literal_bytes (struct rbuf *r, const void **bytes, unsigned int n_bytes) {
    *bytes =   &r->buf[r->ndone];
    r->ndone+=n_bytes;
    assert(r->ndone<=r->size);
}

/* Return a pointer into the middle of the buffer. */
static inline void rbuf_bytes (struct rbuf *r, const void **bytes, unsigned int *n_bytes)
{
    *n_bytes = rbuf_int(r);
    rbuf_literal_bytes(r, bytes, *n_bytes);
}

static inline unsigned long long rbuf_ulonglong (struct rbuf *r) {
    unsigned i0 = rbuf_int(r);
    unsigned i1 = rbuf_int(r);
    return ((unsigned long long)(i0)<<32) | ((unsigned long long)(i1));
}

static inline signed long long rbuf_longlong (struct rbuf *r) {
    return (signed long long)rbuf_ulonglong(r);
}

static inline void rbuf_ma_uint32_t (struct rbuf *r, memarena *ma __attribute__((__unused__)), uint32_t *num) {
    *num = rbuf_int(r);
}

static inline void rbuf_ma_uint64_t (struct rbuf *r, memarena *ma __attribute__((__unused__)), uint64_t *num) {
    *num = rbuf_ulonglong(r);
}

// Don't try to use the same space, malloc it
static inline void rbuf_BYTESTRING (struct rbuf *r, BYTESTRING *bs) {
    bs->len  = rbuf_int(r);
    uint32_t newndone = r->ndone + bs->len;
    assert(newndone <= r->size);
    bs->data = (char *) toku_memdup(&r->buf[r->ndone], (size_t)bs->len);
    assert(bs->data);
    r->ndone = newndone;
}

static inline void rbuf_ma_BYTESTRING  (struct rbuf *r, memarena *ma, BYTESTRING *bs) {
    bs->len  = rbuf_int(r);
    uint32_t newndone = r->ndone + bs->len;
    assert(newndone <= r->size);
    bs->data = (char *) ma->malloc_from_arena(bs->len);
    assert(bs->data);
    memcpy(bs->data, &r->buf[r->ndone], bs->len);
    r->ndone = newndone;
}

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

#include <memory.h>
#include <string.h>

#include "portability/toku_htonl.h"

#include "util/bytestring.h"
#include "util/x1764.h"

/* When serializing a value, write it into a buffer. */
/* This code requires that the buffer be big enough to hold whatever you put into it. */
/* This abstraction doesn't do a good job of hiding its internals.
 * Why?  The performance of this code is important, and we want to inline stuff */
//Why is size here an int instead of DISKOFF like in the initializer?
struct wbuf {
    unsigned char *buf;
    unsigned int  size;
    unsigned int  ndone;
    struct x1764  checksum;    // The checksum state
};

static inline void wbuf_nocrc_init (struct wbuf *w, void *buf, unsigned int size) {
    w->buf = (unsigned char *) buf;
    w->size = size;
    w->ndone = 0;
}

static inline void wbuf_init (struct wbuf *w, void *buf, unsigned int size) {
    wbuf_nocrc_init(w, buf, size);
    toku_x1764_init(&w->checksum);
}

static inline size_t wbuf_get_woffset(struct wbuf *w) {
    return w->ndone;
}

/* Write a character. */
static inline void wbuf_nocrc_char (struct wbuf *w, unsigned char ch) {
    assert(w->ndone<w->size);
    w->buf[w->ndone++]=ch;
}

/* Write a character. */
static inline void wbuf_nocrc_uint8_t (struct wbuf *w, uint8_t ch) {
    assert(w->ndone<w->size);
    w->buf[w->ndone++]=ch;
}

static inline void wbuf_char (struct wbuf *w, unsigned char ch) {
    wbuf_nocrc_char (w, ch);
    toku_x1764_add(&w->checksum, &w->buf[w->ndone-1], 1);
}

//Write an int that MUST be in network order regardless of disk order
static void wbuf_network_int (struct wbuf *w, int32_t i) __attribute__((__unused__));
static void wbuf_network_int (struct wbuf *w, int32_t i) {
    assert(w->ndone + 4 <= w->size);
    *(uint32_t*)(&w->buf[w->ndone]) = toku_htonl(i);
    toku_x1764_add(&w->checksum, &w->buf[w->ndone], 4);
    w->ndone += 4;
}

static inline void wbuf_nocrc_int (struct wbuf *w, int32_t i) {
#if 0
    wbuf_nocrc_char(w, i>>24);
    wbuf_nocrc_char(w, i>>16);
    wbuf_nocrc_char(w, i>>8);
    wbuf_nocrc_char(w, i>>0);
#else
    assert(w->ndone + 4 <= w->size);
 #if 0
    w->buf[w->ndone+0] = i>>24;
    w->buf[w->ndone+1] = i>>16;
    w->buf[w->ndone+2] = i>>8;
    w->buf[w->ndone+3] = i>>0;
 #else
    *(uint32_t*)(&w->buf[w->ndone]) = toku_htod32(i);
 #endif
    w->ndone += 4;
#endif
}

static inline void wbuf_int (struct wbuf *w, int32_t i) {
    wbuf_nocrc_int(w, i);
    toku_x1764_add(&w->checksum, &w->buf[w->ndone-4], 4);
}

static inline void wbuf_nocrc_uint (struct wbuf *w, uint32_t i) {
    wbuf_nocrc_int(w, (int32_t)i);
}

static inline void wbuf_uint (struct wbuf *w, uint32_t i) {
    wbuf_int(w, (int32_t)i);
}

static inline uint8_t* wbuf_nocrc_reserve_literal_bytes(struct wbuf *w, uint32_t nbytes) {
    assert(w->ndone + nbytes <= w->size);
    uint8_t * dest = w->buf + w->ndone;
    w->ndone += nbytes;
    return dest;
}

static inline void wbuf_nocrc_literal_bytes(struct wbuf *w, const void *bytes_bv, uint32_t nbytes) {
    const unsigned char *bytes = (const unsigned char *) bytes_bv;
#if 0
    { int i; for (i=0; i<nbytes; i++) wbuf_nocrc_char(w, bytes[i]); }
#else
    assert(w->ndone + nbytes <= w->size);
    memcpy(w->buf + w->ndone, bytes, (size_t)nbytes);
    w->ndone += nbytes;
#endif
}

static inline void wbuf_literal_bytes(struct wbuf *w, const void *bytes_bv, uint32_t nbytes) {
    wbuf_nocrc_literal_bytes(w, bytes_bv, nbytes);
    toku_x1764_add(&w->checksum, &w->buf[w->ndone-nbytes], nbytes);
}

static void wbuf_nocrc_bytes (struct wbuf *w, const void *bytes_bv, uint32_t nbytes) {
    wbuf_nocrc_uint(w, nbytes);
    wbuf_nocrc_literal_bytes(w, bytes_bv, nbytes);
}

static void wbuf_bytes (struct wbuf *w, const void *bytes_bv, uint32_t nbytes) {
    wbuf_uint(w, nbytes);
    wbuf_literal_bytes(w, bytes_bv, nbytes);
}

static void wbuf_nocrc_ulonglong (struct wbuf *w, uint64_t ull) {
    wbuf_nocrc_uint(w, (uint32_t)(ull>>32));
    wbuf_nocrc_uint(w, (uint32_t)(ull&0xFFFFFFFF));
}

static void wbuf_ulonglong (struct wbuf *w, uint64_t ull) {
    wbuf_uint(w, (uint32_t)(ull>>32));
    wbuf_uint(w, (uint32_t)(ull&0xFFFFFFFF));
}

static inline void wbuf_nocrc_uint64_t(struct wbuf *w, uint64_t ull) {
    wbuf_nocrc_ulonglong(w, ull);
}


static inline void wbuf_uint64_t(struct wbuf *w, uint64_t ull) {
    wbuf_ulonglong(w, ull);
}

static inline void wbuf_nocrc_bool (struct wbuf *w, bool b) {
    wbuf_nocrc_uint8_t(w, (uint8_t)(b ? 1 : 0));
}

static inline void wbuf_nocrc_BYTESTRING (struct wbuf *w, BYTESTRING v) {
    wbuf_nocrc_bytes(w, v.data, v.len);
}

static inline void wbuf_BYTESTRING (struct wbuf *w, BYTESTRING v) {
    wbuf_bytes(w, v.data, v.len);
}

static inline void wbuf_uint8_t (struct wbuf *w, uint8_t v) {
    wbuf_char(w, v);
}

static inline void wbuf_nocrc_uint32_t (struct wbuf *w, uint32_t v) {
    wbuf_nocrc_uint(w, v);
}

static inline void wbuf_uint32_t (struct wbuf *w, uint32_t v) {
    wbuf_uint(w, v);
}

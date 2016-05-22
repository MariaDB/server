/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#ifndef _TOKUDB_VLQ_H
#define _TOKUDB_VLQ_H

namespace tokudb {

    // Variable length encode an unsigned integer into a buffer with limit s.
    // Returns the number of bytes used to encode n in the buffer.
    // Returns 0 if the buffer is too small.
    template <class T> size_t vlq_encode_ui(T n, void *p, size_t s) {
        unsigned char *pp = (unsigned char *)p;
        size_t i = 0;
        while (n >= 128) {
            if (i >= s)
                return 0; // not enough space
            pp[i++] = n%128;
            n = n/128;
        }
        if (i >= s) 
            return 0; // not enough space
        pp[i++] = 128+n;
        return i;
    }

    // Variable length decode an unsigned integer from a buffer with limit s.
    // Returns the number of bytes used to decode the buffer.
    // Returns 0 if the buffer is too small.
    template <class T> size_t vlq_decode_ui(T *np, void *p, size_t s) {
        unsigned char *pp = (unsigned char *)p;
        T n = 0;
        size_t i = 0;
        while (1) {
            if (i >= s)
                return 0; // not a full decode
            T m = pp[i];
            n |= (m & 127) << (7*i);
            i++;
            if ((m & 128) != 0)
                break;
        }
        *np = n;
        return i;
    }
}

#endif

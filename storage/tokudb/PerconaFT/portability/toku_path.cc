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

#include "toku_path.h"
#include <toku_assert.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>

const char *toku_test_filename(const char *default_filename) {
    const char *filename = getenv("TOKU_TEST_FILENAME");
    if (filename == nullptr) {
        filename = basename((char *) default_filename);
        assert(filename != nullptr);
    }
    return filename;
}

// Guarantees NUL termination (unless siz == 0)
// siz is full size of dst (including NUL terminator)
// Appends src to end of dst, (truncating if necessary) to use no more than siz bytes (including NUL terminator)
// Returns strnlen(dst, siz) (size (excluding NUL) of string we tried to create)
size_t toku_strlcat(char *dst, const char *src, size_t siz)
{
    if (siz == 0) {
        return 0;
    }
    dst[siz-1] = '\0'; //Guarantee NUL termination.

    const size_t old_dst_len = strnlen(dst, siz - 1);
    paranoid_invariant(old_dst_len <= siz - 1);
    if (old_dst_len == siz - 1) {
        // No room for anything more.
        return old_dst_len;
    }
    char *d = &dst[old_dst_len];  //Points to null ptr at end of old string
    const size_t remaining_space = siz-old_dst_len-1;
    const size_t allowed_src_len = strnlen(src, remaining_space);  // Limit to remaining space (leave space for NUL)
    paranoid_invariant(allowed_src_len <= remaining_space);
    paranoid_invariant(old_dst_len + allowed_src_len < siz);
    memcpy(d, src, allowed_src_len);
    d[allowed_src_len] = '\0';  // NUL terminate (may be redundant with previous NUL termination)

    return old_dst_len + allowed_src_len;
}

// Guarantees NUL termination (unless siz == 0)
// siz is full size of dst (including NUL terminator)
// Appends src to end of dst, (truncating if necessary) to use no more than siz bytes (including NUL terminator)
// Returns strnlen(dst, siz) (size (excluding NUL) of string we tried to create)
//
// Implementation note: implemented for simplicity as oppsed to performance
size_t toku_strlcpy(char *dst, const char *src, size_t siz)
{
    if (siz == 0) {
        return 0;
    }
    *dst = '\0';
    return toku_strlcat(dst, src, siz);
}

char *toku_path_join(char *dest, int n, const char *base, ...) {
    static const char PATHSEP = '/';
    size_t written;
    written = toku_strlcpy(dest, base, TOKU_PATH_MAX);
    paranoid_invariant(written < TOKU_PATH_MAX);
    paranoid_invariant(dest[written] == '\0');

    va_list ap;
    va_start(ap, base);
    for (int i = 1; written < TOKU_PATH_MAX && i < n; ++i) {
        if (dest[written - 1] != PATHSEP) {
            if (written+2 >= TOKU_PATH_MAX) {
                // No room.
                break;
            }
            dest[written++] = PATHSEP;
            dest[written] = '\0';
        }
        const char *next = va_arg(ap, const char *);
        written = toku_strlcat(dest, next, TOKU_PATH_MAX);
        paranoid_invariant(written < TOKU_PATH_MAX);
        paranoid_invariant(dest[written] == '\0');
    }
    va_end(ap);

    // Zero out rest of buffer for security
    memset(&dest[written], 0, TOKU_PATH_MAX - written);
    return dest;
}

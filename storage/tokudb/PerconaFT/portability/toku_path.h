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

#include <stdarg.h>
#include <limits.h>
#include <sys/types.h>

__attribute__((nonnull))
const char *toku_test_filename(const char *default_filename);

#define TOKU_TEST_FILENAME toku_test_filename(__FILE__)

#define TOKU_PATH_MAX PATH_MAX

// Guarantees NUL termination (unless siz == 0)
// siz is full size of dst (including NUL terminator)
// Appends src to end of dst, (truncating if necessary) to use no more than siz bytes (including NUL terminator)
// Returns strnlen(dst, siz)
size_t toku_strlcat(char *dst, const char *src, size_t siz);

// Guarantees NUL termination (unless siz == 0)
// siz is full size of dst (including NUL terminator)
// Appends src to end of dst, (truncating if necessary) to use no more than siz bytes (including NUL terminator)
// Returns strnlen(dst, siz)
size_t toku_strlcpy(char *dst, const char *src, size_t siz);

char *toku_path_join(char *dest, int n, const char *base, ...);
// Effect:
//  Concatenate all the parts into a filename, using portable path separators.
//  Store the result in dest.
// Requires:
//  dest is a buffer of size at least TOKU_PATH_MAX + 1.
//  There are n path components, including base.
// Returns:
//  dest (useful for chaining function calls)

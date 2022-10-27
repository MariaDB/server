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

#include <portability/toku_config.h>
#include <toku_portability.h>
#include <toku_assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

#if defined(HAVE_RANDOM_R)
// Definition of randu62 and randu64 assume myrandom_r generates 31 low-order bits
static_assert(RAND_MAX == INT32_MAX, "Unexpected RAND_MAX");
static inline int
myinitstate_r(unsigned int seed, char *statebuf, size_t statelen, struct random_data *buf)
{
    return initstate_r(seed, statebuf, statelen, buf);
}
static inline int32_t
myrandom_r(struct random_data *buf)
{
    int32_t x;
    int r = random_r(buf, &x);
    lazy_assert_zero(r);
    return x;
}
#elif defined(HAVE_NRAND48)
struct random_data {
    unsigned short xsubi[3];
};
static inline int
myinitstate_r(unsigned int seed, char *UU(statebuf), size_t UU(statelen), struct random_data *buf)
{
    buf->xsubi[0] = (seed & 0xffff0000) >> 16;
    buf->xsubi[0] = (seed & 0x0000ffff);
    buf->xsubi[2] = (seed & 0x00ffff00) >> 8;
    return 0;
}
static inline int32_t
myrandom_r(struct random_data *buf)
{
    int32_t x = nrand48(buf->xsubi);
    return x;
}
#else
# error "no suitable reentrant random function available (checked random_r and nrand48)"
#endif

static inline uint64_t
randu62(struct random_data *buf)
{
    uint64_t a = myrandom_r(buf);
    uint64_t b = myrandom_r(buf);
    return (a | (b << 31));
}

static inline uint64_t
randu64(struct random_data *buf)
{
    uint64_t r62 = randu62(buf);
    uint64_t c = myrandom_r(buf);
    return (r62 | ((c & 0x3) << 62));
}

static inline uint32_t
rand_choices(struct random_data *buf, uint32_t choices) {
    invariant(choices >= 2);
    invariant(choices < INT32_MAX);
    uint32_t bits = 2;
    while (bits < choices) {
        bits *= 2;
    }
    --bits;

    uint32_t result;
    do {
        result = myrandom_r(buf) & bits;
    } while (result >= choices);

    return result;
}

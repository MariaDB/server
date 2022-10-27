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

#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>

typedef int toku_os_handle_t;

struct fileid {
    dev_t st_dev; /* device and inode are enough to uniquely identify a file in unix. */
    ino_t st_ino;
};

static inline int toku_fileid_cmp(const struct fileid &a, const struct fileid &b) {
    if (a.st_dev < b.st_dev) {
        return -1;
    } else if (a.st_dev > b.st_dev) {
        return +1;
    } else {
        if (a.st_ino < b.st_ino) {
            return -1;
        } else if (a.st_ino > b.st_ino) {
            return +1;
        } else {
            return 0;
        }
    }
}

__attribute__((const, nonnull, warn_unused_result))
static inline bool toku_fileids_are_equal(struct fileid *a, struct fileid *b) {
    return toku_fileid_cmp(*a, *b) == 0;
}

typedef struct stat toku_struct_stat;

#if !defined(O_BINARY)
#define O_BINARY 0
#endif

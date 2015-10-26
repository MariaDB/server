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

#include <toku_portability.h>
#include <toku_pthread.h>

/* Maintain a set of files for reading, with double buffering for the reads. */

/* A DBUFIO_FILESET is a set of files.  The files are indexed from 0 to N-1, where N is specified when the set is created (and the files are also provided when the set is creaed). */
/* An implementation would typically use a separate thread or asynchronous I/O to fetch ahead data for each file.  The system will typically fill two buffers of size M for each file.  One buffer is being read out of using dbuf_read(), and the other buffer is either empty (waiting on the asynchronous I/O to start), being filled in by the asynchronous I/O mechanism, or is waiting for the caller to read data from it. */
typedef struct dbufio_fileset *DBUFIO_FILESET; 

int create_dbufio_fileset (DBUFIO_FILESET *bfsp, int N, int fds[/*N*/], size_t bufsize, bool compressed);

int destroy_dbufio_fileset(DBUFIO_FILESET);

int dbufio_fileset_read (DBUFIO_FILESET bfs, int filenum, void *buf_v, size_t count, size_t *n_read);

int panic_dbufio_fileset(DBUFIO_FILESET, int error);

void dbufio_print(DBUFIO_FILESET);

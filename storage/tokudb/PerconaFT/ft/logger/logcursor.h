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

#include <ft/log_header.h>

struct toku_logcursor;
typedef struct toku_logcursor *TOKULOGCURSOR;

// All routines return 0 on success

// toku_logcursor_create()
//   - creates a logcursor (lc)
//   - following toku_logcursor_create()
//         if toku_logcursor_next() is called, it returns the first entry in the log
//         if toku_logcursor_prev() is called, it returns the last entry in the log
int toku_logcursor_create(TOKULOGCURSOR *lc, const char *log_dir);
// toku_logcursor_create_for_file()
//   - creates a logcusor (lc) that only knows about the file log_file
int toku_logcursor_create_for_file(TOKULOGCURSOR *lc, const char *log_dir, const char *log_file);
// toku_logcursor_destroy()
//    - frees all resources associated with the logcursor, including the log_entry 
//       associated with the latest cursor action
int toku_logcursor_destroy(TOKULOGCURSOR *lc);

// toku_logcursor_[next,prev,first,last] take care of malloc'ing and free'ing log_entrys.
//    - routines NULL out the **le pointers on entry, then set the **le pointers to 
//        the malloc'ed entries when successful, 
int toku_logcursor_next(TOKULOGCURSOR lc, struct log_entry **le);
int toku_logcursor_prev(TOKULOGCURSOR lc, struct log_entry **le);

int toku_logcursor_first(const TOKULOGCURSOR lc, struct log_entry **le);
int toku_logcursor_last(const TOKULOGCURSOR lc, struct log_entry **le);

// return 0 if log exists, ENOENT if no log
int toku_logcursor_log_exists(const TOKULOGCURSOR lc);

void toku_logcursor_print(TOKULOGCURSOR lc);

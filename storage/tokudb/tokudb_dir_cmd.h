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

#ifndef _TOKUDB_DIR_CMD_H
#define _TOKUDB_DIR_CMD_H

#include <sql_class.h>

namespace tokudb {

struct  dir_cmd_callbacks {
    void (*set_error)(THD *thd, int error, const char *error_fmt, ...)
        MY_ATTRIBUTE((format(printf, 3, 4)));
};

void process_dir_cmd(THD *thd,
                     const char *cmd_str,
                     const dir_cmd_callbacks &cb);

};

#endif // _TOKUDB_DIR_CMD_H

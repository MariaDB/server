/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
/* -*- mode: C; c-basic-offset: 4 -*- */
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

#ifndef _TOKUDB_INFORMATION_SCHEMA_H
#define _TOKUDB_INFORMATION_SCHEMA_H

#include "hatoku_defines.h"

namespace tokudb {
namespace information_schema {

#ifdef MARIA_PLUGIN_INTERFACE_VERSION
#define st_mysql_plugin st_maria_plugin
#endif

extern st_mysql_plugin trx;
extern st_mysql_plugin lock_waits;
extern st_mysql_plugin locks;
extern st_mysql_plugin file_map;
extern st_mysql_plugin fractal_tree_info;
extern st_mysql_plugin fractal_tree_block_map;
extern st_mysql_plugin background_job_status;

} // namespace information_schema
} // namespace tokudb

#endif // _TOKUDB_INFORMATION_SCHEMA_H

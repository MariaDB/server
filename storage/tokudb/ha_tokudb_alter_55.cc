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

#if TOKU_INCLUDE_ALTER_55

#include "ha_tokudb_alter_common.cc"

bool ha_tokudb::try_hot_alter_table() {
    TOKUDB_DBUG_ENTER("try_hot_alter_table");
    THD *thd = ha_thd();
    bool disable_hot_alter = get_disable_hot_alter(thd);
    DBUG_RETURN(!disable_hot_alter);
}

int ha_tokudb::new_alter_table_frm_data(const uchar *frm_data, size_t frm_len) {
    return write_frm_data(frm_data, frm_len);
}

#endif

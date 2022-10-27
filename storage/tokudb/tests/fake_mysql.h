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

// Provide some mimimal MySQL classes just to compile the tokudb cardinality functions
class KEY_INFO {
public:
    uint flags;
    uint key_parts;
    uint64_t *rec_per_key;
    char *name;
};
#define HA_NOSAME 1
class TABLE_SHARE {
public:
    uint primary_key;
    uint keys, key_parts;
    KEY_INFO *key_info;
};
class TABLE {
public:
    TABLE_SHARE *s;
    KEY_INFO *key_info;
};
uint get_key_parts(KEY_INFO *key_info) {
    assert(key_info);
    return key_info->key_parts;
}
#define MAX_KEY (1U << 30)

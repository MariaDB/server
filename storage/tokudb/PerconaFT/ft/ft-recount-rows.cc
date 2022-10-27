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

#include "ft/serialize/block_table.h"
#include "ft/ft.h"
#include "ft/ft-internal.h"
#include "ft/cursor.h"

struct recount_rows_extra_t {
    int (*_progress_callback)(
        uint64_t count,
        uint64_t deleted,
        void* progress_extra);
    void* _progress_extra;
    uint64_t _keys;
    bool _cancelled;
};

static int recount_rows_found(
    uint32_t UU(keylen),
    const void* key,
    uint32_t UU(vallen),
    const void* UU(val),
    void* extra,
    bool UU(lock_only)) {

    recount_rows_extra_t* rre = (recount_rows_extra_t*)extra;

    if (FT_LIKELY(key != nullptr)) {
        rre->_keys++;
    }
    return rre->_cancelled
        = rre->_progress_callback(rre->_keys, 0, rre->_progress_extra);
}
static bool recount_rows_interrupt(void* extra, uint64_t deleted_rows) {
    recount_rows_extra_t* rre = (recount_rows_extra_t*)extra;

    return rre->_cancelled =
        rre->_progress_callback(rre->_keys, deleted_rows, rre->_progress_extra);
}
int toku_ft_recount_rows(FT_HANDLE ft,
                         int (*progress_callback)(uint64_t count,
                                                  uint64_t deleted,
                                                  void* progress_extra),
                         void* progress_extra) {
    int ret = 0;
    recount_rows_extra_t rre = {progress_callback, progress_extra, 0, false};

    ft_cursor c;
    ret = toku_ft_cursor_create(ft, &c, nullptr, C_READ_ANY, false, false);
    if (ret)
        return ret;

    toku_ft_cursor_set_check_interrupt_cb(&c, recount_rows_interrupt, &rre);

    ret = toku_ft_cursor_first(&c, recount_rows_found, &rre);
    while (FT_LIKELY(ret == 0)) {
        ret = toku_ft_cursor_next(&c, recount_rows_found, &rre);
    }

    toku_ft_cursor_destroy(&c);

    if (rre._cancelled == false) {
        // update ft count
        toku_unsafe_set(&ft->ft->in_memory_logical_rows, rre._keys);
        ft->ft->h->set_dirty();
        ret = 0;
    }

    return ret;
}

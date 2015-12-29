/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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

#include <db.h>

#include "db.hpp"
#include "slice.hpp"

namespace ftcxx {

    typedef int (*slice_compare_func)(const Slice &desc, const Slice &key, const Slice &val);

    template<slice_compare_func slice_cmp>
    int wrapped_comparator(::DB *db, const DBT *a, const DBT *b) {
        return slice_cmp(DB(db).descriptor(), Slice(*a), Slice(*b));
    }

    class SetvalFunc {
        void (*_setval)(const DBT *, void *);
        void *_extra;
    public:
        SetvalFunc(void (*setval)(const DBT *, void *), void *extra)
            : _setval(setval),
              _extra(extra)
        {}
        void operator()(const Slice &new_val) {
            DBT vdbt = new_val.dbt();
            _setval(&vdbt, _extra);
        }
    };

    typedef int (*slice_update_func)(const Slice &desc, const Slice &key, const Slice &old_val, const Slice &extra, SetvalFunc callback);

    template<slice_update_func slice_update>
    int wrapped_updater(::DB *db, const DBT *key, const DBT *old_val, const DBT *extra, void (*setval)(const DBT *, void *), void *setval_extra) {
        return slice_update(DB(db).descriptor(), Slice(*key), Slice(*old_val), Slice(*extra), SetvalFunc(setval, setval_extra));
    }

} // namespace ftcxx

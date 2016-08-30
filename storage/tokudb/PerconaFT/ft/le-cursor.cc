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

#include <my_global.h>
#include "ft/ft.h"
#include "ft/ft-internal.h"
#include "ft/le-cursor.h"
#include "ft/cursor.h"

// A LE_CURSOR is a special purpose FT_CURSOR that:
//  - enables prefetching
//  - does not perform snapshot reads. it reads everything, including uncommitted.
//
// A LE_CURSOR is good for scanning a FT from beginning to end. Useful for hot indexing.

struct le_cursor {
    FT_CURSOR ft_cursor;
    bool neg_infinity; // true when the le cursor is positioned at -infinity (initial setting)
    bool pos_infinity; // true when the le cursor is positioned at +infinity (when _next returns DB_NOTFOUND)
};

int 
toku_le_cursor_create(LE_CURSOR *le_cursor_result, FT_HANDLE ft_handle, TOKUTXN txn) {
    int result = 0;
    LE_CURSOR MALLOC(le_cursor);
    if (le_cursor == NULL) {
        result = get_error_errno();
    }
    else {
        result = toku_ft_cursor(ft_handle, &le_cursor->ft_cursor, txn, false, false);
        if (result == 0) {
            // TODO move the leaf mode to the ft cursor constructor
            toku_ft_cursor_set_leaf_mode(le_cursor->ft_cursor);
            le_cursor->neg_infinity = false;
            le_cursor->pos_infinity = true;
        }
    }

    if (result == 0) {
        *le_cursor_result = le_cursor;
    } else {
        toku_free(le_cursor);
    }

    return result;
}

void toku_le_cursor_close(LE_CURSOR le_cursor) {
    toku_ft_cursor_close(le_cursor->ft_cursor);
    toku_free(le_cursor);
}

// Move to the next leaf entry under the LE_CURSOR
// Success: returns zero, calls the getf callback with the getf_v parameter
// Failure: returns a non-zero error number
int 
toku_le_cursor_next(LE_CURSOR le_cursor, FT_GET_CALLBACK_FUNCTION getf, void *getf_v) {
    int result;
    if (le_cursor->neg_infinity) {
        result = DB_NOTFOUND;
    } else {
        le_cursor->pos_infinity = false;
        // TODO replace this with a non deprecated function. Which?
        result = toku_ft_cursor_get(le_cursor->ft_cursor, NULL, getf, getf_v, DB_PREV);
        if (result == DB_NOTFOUND) {
            le_cursor->neg_infinity = true;
        }
    }
    return result;
}

bool
toku_le_cursor_is_key_greater_or_equal(LE_CURSOR le_cursor, const DBT *key) {
    bool result;
    if (le_cursor->neg_infinity) {
        result = true;      // all keys are greater than -infinity
    } else if (le_cursor->pos_infinity) {
        result = false;     // all keys are less than +infinity
    } else {
        FT ft = le_cursor->ft_cursor->ft_handle->ft;
        // get the current position from the cursor and compare it to the given key.
        int r = ft->cmp(&le_cursor->ft_cursor->key, key);
        if (r <= 0) {
            result = true;  // key is right of the cursor key
        } else {
            result = false; // key is at or left of the cursor key
        }
    }
    return result;
}

void
toku_le_cursor_update_estimate(LE_CURSOR le_cursor, DBT* estimate) {
    // don't handle these edge cases, not worth it.
    // estimate stays same
    if (le_cursor->pos_infinity || le_cursor->neg_infinity) {
        return;
    }
    DBT *cursor_key = &le_cursor->ft_cursor->key;
    estimate->data = toku_xrealloc(estimate->data, cursor_key->size);
    memcpy(estimate->data, cursor_key->data, cursor_key->size);
    estimate->size = cursor_key->size;
    estimate->flags = DB_DBT_REALLOC;
}

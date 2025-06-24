/*****************************************************************************

Copyright (c) 2023, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

#include "db0err.h"

/* The purpose of the change buffer was to reduce random disk access.
When we wished to
(1) insert a record into a non-unique secondary index,
(2) delete-mark a secondary index record,
(3) delete a secondary index record as part of purge (but not ROLLBACK),
and the B-tree leaf page where the record belongs to is not in the buffer
pool, we inserted a record into the change buffer B-tree, indexed by
the page identifier. When the page was eventually read into the buffer
pool, we looked up the change buffer B-tree for any modifications to the
page, applied these upon the completion of the read operation. This
was called the insert buffer merge.

There was a hash index of the change buffer B-tree, implemented as the
"change buffer bitmap". Bits in these bitmap pages indicated how full
the page roughly was, and whether any records for the page identifier
exist in the change buffer. The "free" bits had to be updated as part of
operations that modified secondary index leaf pages.

Because the change buffer has been removed, we will no longer update
any change buffer bitmap pages. Instead, on database startup, we will
check if an upgrade needs to be performed, and apply any buffered
changes if that is the case. Finally, the change buffer will be
transformed to a format that will not be recognized by earlier
versions of MariaDB Server, to prevent downgrades from causing
corruption (due to the removed updates of the bitmap pages) when the
change buffer might be enabled. */

/** Check if ibuf_upgrade() is needed as part of server startup.
@return error code
@retval DB_SUCCESS if no upgrade is needed
@retval DB_FAIL    if the change buffer is not empty (need ibuf_upgrade()) */
dberr_t ibuf_upgrade_needed();

/** Upgrade the change buffer after all redo log has been applied. */
dberr_t ibuf_upgrade();

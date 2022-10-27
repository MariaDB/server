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

//Must be defined before other recursive headers could include logger/recover.h
enum ft_layout_version_e {
    FT_LAYOUT_VERSION_5 = 5,
    FT_LAYOUT_VERSION_6 = 6,   // Diff from 5 to 6:  Add leafentry_estimate
    FT_LAYOUT_VERSION_7 = 7,   // Diff from 6 to 7:  Add exact-bit to leafentry_estimate #818, add magic to header #22, add per-subdatase flags #333
    FT_LAYOUT_VERSION_8 = 8,   // Diff from 7 to 8:  Use murmur instead of crc32.  We are going to make a simplification and stop supporting version 7 and before.  Current As of Beta 1.0.6
    FT_LAYOUT_VERSION_9 = 9,   // Diff from 8 to 9:  Variable-sized blocks and compression.
    FT_LAYOUT_VERSION_10 = 10, // Diff from 9 to 10: Variable number of compressed sub-blocks per block, disk byte order == intel byte order, Subtree estimates instead of just leafentry estimates, translation table, dictionary descriptors, checksum in header, subdb support removed from ft layer
    FT_LAYOUT_VERSION_11 = 11, // Diff from 10 to 11: Nested transaction leafentries (completely redesigned).  FT_CMDs on disk now support XIDS (multiple txnids) instead of exactly one.
    FT_LAYOUT_VERSION_12 = 12, // Diff from 11 to 12: Added FT_CMD 'FT_INSERT_NO_OVERWRITE', compressed block format, num old blocks
    FT_LAYOUT_VERSION_13 = 13, // Diff from 12 to 13: Fixed loader pivot bug, added build_id to every node, timestamps to ft 
    FT_LAYOUT_VERSION_14 = 14, // Diff from 13 to 14: Added MVCC; deprecated TOKU_DB_VALCMP_BUILTIN(_13); Remove fingerprints; Support QUICKLZ; add end-to-end checksum on uncompressed data.
    FT_LAYOUT_VERSION_15 = 15, // Diff from 14 to 15: basement nodes, last verification time
    FT_LAYOUT_VERSION_16 = 16, // Dr. No:  No subtree estimates, partition layout information represented more transparently. 
                                // ALERT ALERT ALERT: version 16 never released to customers, internal and beta use only
    FT_LAYOUT_VERSION_17 = 17, // Dr. No:  Add STAT64INFO_S to ft header
    FT_LAYOUT_VERSION_18 = 18, // Dr. No:  Add HOT info to ft header
    FT_LAYOUT_VERSION_19 = 19, // Doofenshmirtz: Add compression method, highest_unused_msn_for_upgrade
    FT_LAYOUT_VERSION_20 = 20, // Deadshot: Add compression method to log_fcreate,
                               // mgr_last_xid after begin checkpoint,
                               // last_xid to shutdown
    FT_LAYOUT_VERSION_21 = 21, // Ming: Add max_msn_in_ft to header,
                               //       Removed log suppression logentry
    FT_LAYOUT_VERSION_22 = 22, // Ming: Add oldest known referenced xid to each ftnode, for better garbage collection
    FT_LAYOUT_VERSION_23 = 23, // Ming: Fix upgrade path #5902
    FT_LAYOUT_VERSION_24 = 24, // Riddler: change logentries that log transactions to store TXNID_PAIRs instead of TXNIDs
    FT_LAYOUT_VERSION_25 = 25, // SecretSquirrel: ROLLBACK_LOG_NODES (on disk and in memory) now just use blocknum (instead of blocknum + hash) to point to other log nodes.  same for xstillopen log entry
    FT_LAYOUT_VERSION_26 = 26, // Hojo: basements store key/vals separately on disk for fixed klpair length BNs
    FT_LAYOUT_VERSION_27 = 27, // serialize message trees with nonleaf buffers to avoid key, msn sort on deserialize
    FT_LAYOUT_VERSION_28 = 28, // Add fanout to ft_header
    FT_LAYOUT_VERSION_29 = 29, // Add logrows to ft_header
    FT_NEXT_VERSION,           // the version after the current version
    FT_LAYOUT_VERSION   = FT_NEXT_VERSION-1, // A hack so I don't have to change this line.
    FT_LAYOUT_MIN_SUPPORTED_VERSION = FT_LAYOUT_VERSION_13, // Minimum version supported

    // Define these symbolically so the knowledge of exactly which layout version got rid of fingerprints isn't spread all over the code.
    FT_LAST_LAYOUT_VERSION_WITH_FINGERPRINT = FT_LAYOUT_VERSION_13,
    FT_FIRST_LAYOUT_VERSION_WITH_END_TO_END_CHECKSUM = FT_LAYOUT_VERSION_14,
    FT_FIRST_LAYOUT_VERSION_WITH_BASEMENT_NODES = FT_LAYOUT_VERSION_15,
};

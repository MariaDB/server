/*****************************************************************************

Copyright (c) 1995, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

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

/**************************************************//**
@file include/mtr0types.h
Mini-transaction buffer global types

Created 11/26/1995 Heikki Tuuri
*******************************************************/

#pragma once

#include "buf0types.h"

#include "ut0byte.h"

struct mtr_t;

/** Logging modes for a mini-transaction */
enum mtr_log_t {
	/** Default mode: log all operations modifying disk-based data */
	MTR_LOG_ALL = 0,

	/** Log no operations and dirty pages are not added to the flush list.
	Set for attempting modification of a ROW_FORMAT=COMPRESSED page. */
	MTR_LOG_NONE,

	/** Don't generate REDO log but add dirty pages to flush list */
	MTR_LOG_NO_REDO
};

/*
A mini-transaction is a stream of records that is always terminated by
a byte 0x00 or 0x01. The first byte of a mini-transaction record is
never one of these bytes, but these bytes can occur within mini-transaction
records.

The first byte of the record would contain a record type, flags, and a
part of length. The optional second byte of the record will contain
more length. (Not needed for short records.)

For example, because the length of an INIT_PAGE record is 3 to 11 bytes,
the first byte will be 0x02 to 0x0a, indicating the number of subsequent bytes.

Bit 7 of the first byte of a redo log record is the same_page flag.
If same_page=1, the record is referring to the same page as the
previous record. Records that do not refer to data pages but to file
operations are identified by setting the same_page=1 in the very first
record(s) of the mini-transaction. A mini-transaction record that
carries same_page=0 must only be followed by page-oriented records.

Bits 6..4 of the first byte of a redo log record identify the redo log
type. The following record types refer to data pages:

    FREE_PAGE (0): corresponds to MLOG_INIT_FREE_PAGE
    INIT_PAGE (1): corresponds to MLOG_INIT_FILE_PAGE2
    EXTENDED (2): extended record; followed by subtype code @see mrec_ext_t
    WRITE (3): replaces MLOG_nBYTES, MLOG_WRITE_STRING, MLOG_ZIP_*
    MEMSET (4): extends the 10.4 MLOG_MEMSET record
    MEMMOVE (5): copy data within the page (avoids logging redundant data)
    RESERVED (6): reserved for future use; a subtype code
    (encoded immediately after the length) would be written
    to reserve code space for further extensions
    OPTION (7): optional record that may be ignored; a subtype code
    (encoded immediately after the length) would distinguish actual
    usage, such as:
     * MDEV-18976 page checksum record
     * binlog record
     * SQL statement (at the start of statement)

Bits 3..0 indicate the redo log record length, excluding the first
byte, but including additional length bytes and any other bytes,
such as the optional tablespace identifier and page number.
Values 1..15 represent lengths of 1 to 15 bytes. The special value 0
indicates that 1 to 3 length bytes will follow to encode the remaining
length that exceeds 16 bytes.

Additional length bytes if length>16: 0 to 3 bytes
0xxxxxxx                   for 0 to 127 (total: 16 to 143 bytes)
10xxxxxx xxxxxxxx          for 128 to 16511 (total: 144 to 16527)
110xxxxx xxxxxxxx xxxxxxxx for 16512 to 2113663 (total: 16528 to 2113679)
111xxxxx                   reserved (corrupted record, and file!)

If same_page=0, the tablespace identifier and page number will use
similar 1-to-5-byte variable-length encoding:
0xxxxxxx                                     for 0 to 127
10xxxxxx xxxxxxxx                            for 128 to 16,511
110xxxxx xxxxxxxx xxxxxxxx                   for 16,512 to 2,113,663
1110xxxx xxxxxxxx xxxxxxxx xxxxxxxx          for 2,113,664 to 270,549,119
11110xxx xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx for 270,549,120 to 34,630,287,487
11111xxx                                     reserved (corrupted record)
Note: Some 5-byte values are reserved, because the tablespace identifier
and page number can only be up to 4,294,967,295.

If same_page=1 is set in a record that follows a same_page=0 record
in a mini-transaction, the tablespace identifier and page number
fields will be omitted.

For FILE_ records (if same_page=1 for the first record
of a mini-transaction), we will write a tablespace identifier and
a page number (always 0) using the same 1-to-5-byte encoding.

For FREE_PAGE or INIT_PAGE, if same_page=1, the record will be treated
as corrupted (or reserved for future extension).  The type code must
be followed by 1+1 to 5+5 bytes (to encode the tablespace identifier
and page number). If the record length does not match the encoded
lengths of the tablespace identifier and page number, the record will
be treated as corrupted. This allows future expansion of the format.

If there is a FREE_PAGE record in a mini-transaction, it must be the
only record for that page in the mini-transaction. If there is an
INIT_PAGE record for a page in a mini-transaction, it must be the
first record for that page in the mini-transaction.

An EXTENDED record must be followed by 1+1 to 5+5 bytes for the page
identifier (unless the same_page flag is set) and a subtype; @see mrec_ext_t

For WRITE, MEMSET, MEMMOVE, the next 1 to 3 bytes are the byte offset
on the page, relative from the previous offset. If same_page=0, the
"previous offset" is 0. If same_page=1, the "previous offset" is where
the previous operation ended (FIL_PAGE_TYPE for INIT_PAGE).
0xxxxxxx                                     for 0 to 127
10xxxxxx xxxxxxxx                            for 128 to 16,511
110xxxxx xxxxxxxx xxxxxxxx                   for 16,512 to 2,113,663
111xxxxx                                     reserved (corrupted record)
If the sum of the "previous offset" and the current offset exceeds the
page size, the record is treated as corrupted. Negative relative offsets
cannot be written. Instead, a record with same_page=0 can be written.

For MEMSET and MEMMOVE, the target length will follow, encoded in 1 to
3 bytes.  If the length+offset exceeds the page size, the record will
be treated as corrupted.

For MEMMOVE, the source offset will follow, encoded in 1 to 3 bytes,
relative to the current offset. The offset 0 is not possible, and
the sign bit is the least significant bit. That is,
+x is encoded as (x-1)<<1 (+1,+2,+3,... is 0,2,4,...) and
-x is encoded as (x-1)<<1|1 (-1,-2,-3,... is 1,3,5,...).
The source offset must be within the page size, or else the record
will be treated as corrupted.

For MEMSET or WRITE, the byte(s) to be written will follow. For
MEMSET, it usually is a single byte, but it could also be a multi-byte
string, which would be copied over and over until the target length is
reached. The length of the remaining bytes is implied by the length
bytes at the start of the record.

For MEMMOVE, if any bytes follow, the record is treated as corrupted
(future expansion).

As mentioned at the start of this comment, the type byte 0 would be
special, marking the end of a mini-transaction. We could use the
corresponding value 0x80 (with same_page=1) for something special,
such as a future extension when more type codes are needed, or for
encoding rarely needed redo log records.

Examples:

INIT could be logged as 0x12 0x34 0x56, meaning "type code 1 (INIT), 2
bytes to follow" and "tablespace ID 0x34", "page number 0x56".
The first byte must be between 0x12 and 0x1a, and the total length of
the record must match the lengths of the encoded tablespace ID and
page number.

WRITE could be logged as 0x36 0x40 0x57 0x60 0x12 0x34 0x56, meaning
"type code 3 (WRITE), 6 bytes to follow" and "tablespace ID 0x40",
"page number 0x57", "byte offset 0x60", data 0x34,0x56.

A subsequent WRITE to the same page could be logged 0xb5 0x7f 0x23
0x34 0x56 0x78, meaning "same page, type code 3 (WRITE), 5 bytes to
follow", "byte offset 0x7f"+0x60+2, bytes 0x23,0x34,0x56,0x78.

The end of the mini-transaction would be indicated by the end byte
0x00 or 0x01; @see log_sys.get_sequence_bit().
If log_sys.is_encrypted(), that is followed by 8 bytes of nonce
(part of initialization vector). That will be followed by 4 bytes
of CRC-32C of the entire mini-tranasction, excluding the end byte. */

/** Redo log record types. These bit patterns (3 bits) will be written
to the redo log file, so the existing codes or their interpretation on
crash recovery must not be changed. */
enum mrec_type_t
{
  /** Free a page. On recovery, it is unnecessary to read the page.
  The next record for the page (if any) must be INIT_PAGE.
  After this record has been written, the page may be
  overwritten with zeros, or discarded or trimmed. */
  FREE_PAGE= 0,
  /** Zero-initialize a page. The current byte offset (for subsequent
  records) will be reset to FIL_PAGE_TYPE. */
  INIT_PAGE= 0x10,
  /** Extended record; @see mrec_ext_t */
  EXTENDED= 0x20,
  /** Write a string of bytes. Followed by the byte offset (unsigned,
  relative to the current byte offset, encoded in 1 to 3 bytes) and
  the bytes to write (at least one). The current byte offset will be
  set after the last byte written. */
  WRITE= 0x30,
  /** Like WRITE, but before the bytes to write, the data_length-1
  (encoded in 1 to 3 bytes) will be encoded, and it must be more
  than the length of the following data bytes to write.
  The data byte(s) will be repeatedly copied to the output until
  the data_length is reached. */
  MEMSET= 0x40,
  /** Like MEMSET, but instead of the bytes to write, a source byte
  offset (signed, nonzero, relative to the target byte offset, encoded
  in 1 to 3 bytes, with the sign bit in the least significant bit)
  will be written.
  That is, +x is encoded as (x-1)<<1 (+1,+2,+3,... is 0,2,4,...)
  and -x is encoded as (x-1)<<1|1 (-1,-2,-3,... is 1,3,5,...).
  The source offset and data_length must be within the page size, or
  else the record will be treated as corrupted. The data will be
  copied from the page as it was at the start of the
  mini-transaction. */
  MEMMOVE= 0x50,
  /** Reserved for future use. */
  RESERVED= 0x60,
  /** Optional record that may be ignored in crash recovery.
  A subtype code will be encoded immediately after the length.
  Possible subtypes would include a MDEV-18976 page checksum record,
  a binlog record, or an SQL statement. */
  OPTION= 0x70
};


/** Supported EXTENDED record subtypes. */
enum mrec_ext_t
{
  /** Partly initialize a ROW_FORMAT=REDUNDANT B-tree or R-tree index page,
  including writing the "infimum" and "supremum" pseudo-records.
  The current byte offset will be reset to FIL_PAGE_TYPE. */
  INIT_ROW_FORMAT_REDUNDANT= 0,
  /** Partly initialize a ROW_FORMAT=COMPACT or DYNAMIC index page,
  including writing the "infimum" and "supremum" pseudo-records.
  The current byte offset will be reset to FIL_PAGE_TYPE. */
  INIT_ROW_FORMAT_DYNAMIC= 1,
  /** Initialize an undo log page.
  This is roughly (not exactly) equivalent to the old MLOG_UNDO_INIT record.
  The current byte offset will be reset to FIL_PAGE_TYPE. */
  UNDO_INIT= 2,
  /** Append a record to an undo log page.
  This is equivalent to the old MLOG_UNDO_INSERT record.
  The current byte offset will be reset to FIL_PAGE_TYPE. */
  UNDO_APPEND= 3,
  /** Insert a ROW_FORMAT=REDUNDANT record, extending PAGE_HEAP_TOP.
  The current byte offset will be reset to FIL_PAGE_TYPE. */
  INSERT_HEAP_REDUNDANT= 4,
  /** Insert a ROW_FORMAT=REDUNDANT record, reusing PAGE_FREE.
  The current byte offset will be reset to FIL_PAGE_TYPE. */
  INSERT_REUSE_REDUNDANT= 5,
  /** Insert a ROW_FORMAT=COMPACT or DYNAMIC record, extending PAGE_HEAP_TOP.
  The current byte offset will be reset to FIL_PAGE_TYPE. */
  INSERT_HEAP_DYNAMIC= 6,
  /** Insert a ROW_FORMAT=COMPACT or DYNAMIC record, reusing PAGE_FREE.
  The current byte offset will be reset to FIL_PAGE_TYPE. */
  INSERT_REUSE_DYNAMIC= 7,
  /** Delete a record on a ROW_FORMAT=REDUNDANT page.
  We point to the precedessor of the record to be deleted.
  The current byte offset will be reset to FIL_PAGE_TYPE.
  This is similar to the old MLOG_REC_DELETE record. */
  DELETE_ROW_FORMAT_REDUNDANT= 8,
  /** Delete a record on a ROW_FORMAT=COMPACT or DYNAMIC page.
  We point to the precedessor of the record to be deleted
  and include the total size of the record being deleted.
  The current byte offset will be reset to FIL_PAGE_TYPE.
  This is similar to the old MLOG_COMP_REC_DELETE record. */
  DELETE_ROW_FORMAT_DYNAMIC= 9,
  /** Truncate a data file. */
  TRIM_PAGES= 10
};


/** Redo log record types for file-level operations. These bit
patterns will be written to redo log files, so the existing codes or
their interpretation on crash recovery must not be changed. */
enum mfile_type_t
{
  /** Create a file. Followed by tablespace ID and the file name. */
  FILE_CREATE = 0x80,
  /** Delete a file. Followed by tablespace ID and the file name.  */
  FILE_DELETE = 0x90,
  /** Rename a file. Followed by tablespace ID and the old file name,
  NUL, and the new file name.  */
  FILE_RENAME = 0xa0,
  /** Modify a file. Followed by tablespace ID and the file name. */
  FILE_MODIFY = 0xb0,
  /** End-of-checkpoint marker, at the end of a mini-transaction.
  Followed by 2 NUL bytes of page identifier and 8 bytes of LSN;
  @see SIZE_OF_FILE_CHECKPOINT.
  When all bytes are NUL, this is a dummy padding record. */
  FILE_CHECKPOINT = 0xf0
};

/** Size of a FILE_CHECKPOINT record, including the trailing byte to
terminate the mini-transaction and the CRC-32C. */
constexpr byte SIZE_OF_FILE_CHECKPOINT= 3/*type,page_id*/ + 8/*LSN*/ + 1 + 4;

#ifndef UNIV_INNOCHECKSUM
/** Types for the mlock objects to store in the mtr_t::m_memo */
enum mtr_memo_type_t {
	MTR_MEMO_PAGE_S_FIX = RW_S_LATCH,

	MTR_MEMO_PAGE_X_FIX = RW_X_LATCH,

	MTR_MEMO_PAGE_SX_FIX = RW_SX_LATCH,

	MTR_MEMO_BUF_FIX = RW_NO_LATCH,

	MTR_MEMO_MODIFY = 16,

	MTR_MEMO_PAGE_X_MODIFY = MTR_MEMO_PAGE_X_FIX | MTR_MEMO_MODIFY,
	MTR_MEMO_PAGE_SX_MODIFY = MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_MODIFY,

	MTR_MEMO_S_LOCK = RW_S_LATCH << 5,

	MTR_MEMO_X_LOCK = RW_X_LATCH << 5,

	MTR_MEMO_SX_LOCK = RW_SX_LATCH << 5,

	/** wr_lock() on fil_space_t::latch */
	MTR_MEMO_SPACE_X_LOCK = MTR_MEMO_SX_LOCK << 1,
	/** rd_lock() on fil_space_t::latch */
	MTR_MEMO_SPACE_S_LOCK = MTR_MEMO_SX_LOCK << 2
};
#endif /* !UNIV_INNOCHECKSUM */

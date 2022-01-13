/*****************************************************************************

Copyright (c) 2015, 2018, MariaDB Corporation.

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
@file include/fil0fil.ic
The low-level file system support functions

Created 31/03/2015 Jan Lindström
*******************************************************/

#ifndef fil0fil_ic
#define fil0fil_ic

/*******************************************************************//**
Return page type name */
UNIV_INLINE
const char*
fil_get_page_type_name(
/*===================*/
	ulint	page_type)	/*!< in: FIL_PAGE_TYPE */
{
	switch(page_type) {
	case FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED:
		return "PAGE_COMPRESSED_ENRYPTED";
	case FIL_PAGE_PAGE_COMPRESSED:
		return "PAGE_COMPRESSED";
	case FIL_PAGE_INDEX:
		return "INDEX";
	case FIL_PAGE_RTREE:
		return "RTREE";
	case FIL_PAGE_UNDO_LOG:
		return "UNDO LOG";
	case FIL_PAGE_INODE:
		return "INODE";
	case FIL_PAGE_IBUF_FREE_LIST:
		return "IBUF_FREE_LIST";
	case FIL_PAGE_TYPE_ALLOCATED:
		return "ALLOCATED";
	case FIL_PAGE_IBUF_BITMAP:
		return "IBUF_BITMAP";
	case FIL_PAGE_TYPE_SYS:
		return "SYS";
	case FIL_PAGE_TYPE_TRX_SYS:
		return "TRX_SYS";
	case FIL_PAGE_TYPE_FSP_HDR:
		return "FSP_HDR";
	case FIL_PAGE_TYPE_XDES:
		return "XDES";
	case FIL_PAGE_TYPE_BLOB:
		return "BLOB";
	case FIL_PAGE_TYPE_ZBLOB:
		return "ZBLOB";
	case FIL_PAGE_TYPE_ZBLOB2:
		return "ZBLOB2";
	case FIL_PAGE_TYPE_UNKNOWN:
		return "OLD UNKOWN PAGE TYPE";
	default:
		return "PAGE TYPE CORRUPTED";
	}
}

/****************************************************************//**
Validate page type.
@return true if valid, false if not */
UNIV_INLINE
bool
fil_page_type_validate(
	const byte*	page)	/*!< in: page */
{
#ifdef UNIV_DEBUG
	ulint page_type = mach_read_from_2(page + FIL_PAGE_TYPE);

	/* Validate page type */
	if (!((page_type == FIL_PAGE_PAGE_COMPRESSED ||
		page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED ||
		page_type == FIL_PAGE_INDEX ||
		page_type == FIL_PAGE_RTREE ||
		page_type == FIL_PAGE_UNDO_LOG ||
		page_type == FIL_PAGE_INODE ||
		page_type == FIL_PAGE_IBUF_FREE_LIST ||
		page_type == FIL_PAGE_TYPE_ALLOCATED ||
		page_type == FIL_PAGE_IBUF_BITMAP ||
		page_type == FIL_PAGE_TYPE_SYS ||
		page_type == FIL_PAGE_TYPE_TRX_SYS ||
		page_type == FIL_PAGE_TYPE_FSP_HDR ||
		page_type == FIL_PAGE_TYPE_XDES ||
		page_type == FIL_PAGE_TYPE_BLOB ||
		page_type == FIL_PAGE_TYPE_ZBLOB ||
		page_type == FIL_PAGE_TYPE_ZBLOB2 ||
		page_type == FIL_PAGE_TYPE_UNKNOWN))) {

		ulint space = mach_read_from_4(page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
		ulint offset = mach_read_from_4(page + FIL_PAGE_OFFSET);
		fil_system_enter();
		fil_space_t* rspace = fil_space_get_by_id(space);
		fil_system_exit();

		/* Dump out the page info */
		ib::fatal() << "Page " << space << ":" << offset
			<< " name " << (rspace ? rspace->name : "???")
			<< " page_type " << page_type
			<< " key_version "
			<< mach_read_from_4(page + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION)
			<< " lsn " << mach_read_from_8(page + FIL_PAGE_LSN)
			<< " compressed_len " << mach_read_from_2(page + FIL_PAGE_DATA);
		return false;
	}

#endif /* UNIV_DEBUG */
	return true;
}

#endif /* fil0fil_ic */

/*****************************************************************************

Copyright (C) 2013, 2018, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/******************************************************************//**
@file include/fsp0pagecompress.ic
Implementation for helper functions for extracting/storing page
compression and atomic writes information to file space.

Created 11/12/2013 Jan Lindström jan.lindstrom@mariadb.com

***********************************************************************/

/********************************************************************//**
Determine the tablespace is page compression level from dict_table_t::flags.
@return	page compression level or 0 if not compressed*/
UNIV_INLINE
ulint
fsp_flags_get_page_compression_level(
/*=================================*/
	ulint	flags)	/*!< in: tablespace flags */
{
	return(FSP_FLAGS_GET_PAGE_COMPRESSION_LEVEL(flags));
}


/*******************************************************************//**
Find out wheather the page is page compressed
@return	true if page is page compressed, false if not */
UNIV_INLINE
bool
fil_page_is_compressed(
/*===================*/
	const byte*	buf)	/*!< in: page */
{
	return(mach_read_from_2(buf+FIL_PAGE_TYPE) == FIL_PAGE_PAGE_COMPRESSED);
}

/*******************************************************************//**
Find out wheather the page is page compressed
@return	true if page is page compressed, false if not */
UNIV_INLINE
bool
fil_page_is_compressed_encrypted(
/*=============================*/
	const byte*	buf)	/*!< in: page */
{
	return(mach_read_from_2(buf+FIL_PAGE_TYPE) == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED);
}

/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2019, MariaDB Corporation.

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
@file include/fsp0fsp.ic
File space management

Created 12/18/1995 Heikki Tuuri
*******************************************************/

/**********************************************************************//**
Gets a descriptor bit of a page.
@return TRUE if free */
UNIV_INLINE
ibool
xdes_get_bit(
/*=========*/
	const xdes_t*	descr,	/*!< in: descriptor */
	ulint		bit,	/*!< in: XDES_FREE_BIT or XDES_CLEAN_BIT */
	ulint		offset)	/*!< in: page offset within extent:
				0 ... FSP_EXTENT_SIZE - 1 */
{
	ut_ad(offset < FSP_EXTENT_SIZE);
	ut_ad(bit == XDES_FREE_BIT || bit == XDES_CLEAN_BIT);

	ulint	index = bit + XDES_BITS_PER_PAGE * offset;

	ulint	bit_index = index % 8;
	ulint	byte_index = index / 8;

	return ut_bit_get_nth(descr[XDES_BITMAP + byte_index], bit_index);
}

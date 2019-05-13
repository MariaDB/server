/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2017, MariaDB Corporation.

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

#ifndef UNIV_INNOCHECKSUM

/** Checks if a page address is an extent descriptor page address.
@param[in]	page_id		page id
@param[in]	page_size	page size
@return TRUE if a descriptor page */
UNIV_INLINE
ibool
fsp_descr_page(
	const page_id_t		page_id,
	const page_size_t&	page_size)
{
	return((page_id.page_no() & (page_size.physical() - 1))
	       == FSP_XDES_OFFSET);
}

/** Calculates the descriptor index within a descriptor page.
@param[in]	page_size	page size
@param[in]	offset		page offset
@return descriptor index */
UNIV_INLINE
ulint
xdes_calc_descriptor_index(
	const page_size_t&	page_size,
	ulint			offset)
{
	return(ut_2pow_remainder(offset, page_size.physical())
	       / FSP_EXTENT_SIZE);
}
#endif /*!UNIV_INNOCHECKSUM */

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

	return(ut_bit_get_nth(
			mach_read_ulint(descr + XDES_BITMAP + byte_index,
					MLOG_1BYTE),
			bit_index));
}

#ifndef UNIV_INNOCHECKSUM
/** Calculates the page where the descriptor of a page resides.
@param[in]	page_size	page size
@param[in]	offset		page offset
@return descriptor page offset */
UNIV_INLINE
ulint
xdes_calc_descriptor_page(
	const page_size_t&	page_size,
	ulint			offset)
{
#ifndef DOXYGEN /* Doxygen gets confused by these */
# if UNIV_PAGE_SIZE_MAX <= XDES_ARR_OFFSET				\
			   + (UNIV_PAGE_SIZE_MAX / FSP_EXTENT_SIZE_MAX)	\
			   * XDES_SIZE_MAX
#  error
# endif
# if UNIV_ZIP_SIZE_MIN <= XDES_ARR_OFFSET				\
			  + (UNIV_ZIP_SIZE_MIN / FSP_EXTENT_SIZE_MIN)	\
			  * XDES_SIZE_MIN
#  error
# endif
#endif /* !DOXYGEN */

	ut_ad(UNIV_PAGE_SIZE > XDES_ARR_OFFSET
	      + (UNIV_PAGE_SIZE / FSP_EXTENT_SIZE)
	      * XDES_SIZE);
	ut_ad(UNIV_ZIP_SIZE_MIN > XDES_ARR_OFFSET
	      + (UNIV_ZIP_SIZE_MIN / FSP_EXTENT_SIZE)
	      * XDES_SIZE);

#ifdef UNIV_DEBUG
	if (page_size.is_compressed()) {
		ut_a(page_size.physical() > XDES_ARR_OFFSET
		     + (page_size.physical() / FSP_EXTENT_SIZE) * XDES_SIZE);
	}
#endif /* UNIV_DEBUG */

	return(ut_2pow_round(offset, page_size.physical()));
}
#endif /* !UNIV_INNOCHECKSUM */

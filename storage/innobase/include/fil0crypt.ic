/*****************************************************************************

Copyright (c) 2015, 2017, MariaDB Corporation.

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
@file include/fil0crypt.ic
The low-level file system encryption support functions

Created 04/01/2015 Jan Lindström
*******************************************************/

/*******************************************************************//**
Find out whether the page is page encrypted
@return	true if page is page encrypted, false if not */
UNIV_INLINE
bool
fil_page_is_encrypted(
/*==================*/
	const byte *buf)	/*!< in: page */
{
	return(mach_read_from_4(buf+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION) != 0);
}

/*******************************************************************//**
Get current encryption mode from crypt_data.
@return string representation */
UNIV_INLINE
const char *
fil_crypt_get_mode(
/*===============*/
	const fil_space_crypt_t* crypt_data)
{
	switch (crypt_data->encryption) {
	case FIL_ENCRYPTION_DEFAULT:
		return("Default tablespace encryption mode");
	case FIL_ENCRYPTION_ON:
		return("Tablespace encrypted");
	case FIL_ENCRYPTION_OFF:
		return("Tablespace not encrypted");
	}

	ut_error;
	return ("NULL");
}

/*******************************************************************//**
Get current encryption type from crypt_data.
@return string representation */
UNIV_INLINE
const char *
fil_crypt_get_type(
	const fil_space_crypt_t* crypt_data)
{
	ut_ad(crypt_data != NULL);
	switch (crypt_data->type) {
	case CRYPT_SCHEME_UNENCRYPTED:
		return("scheme unencrypted");
		break;
	case CRYPT_SCHEME_1:
		return("scheme encrypted");
		break;
	default:
		ut_error;
	}

	return ("NULL");
}

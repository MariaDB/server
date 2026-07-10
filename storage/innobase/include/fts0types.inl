/*****************************************************************************

Copyright (c) 2007, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

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

/******************************************************************//**
@file include/fts0types.ic
Full text search types.

Created 2007-03-27 Sunny Bains
*******************************************************/

#ifndef INNOBASE_FTS0TYPES_IC
#define INNOBASE_FTS0TYPES_IC

/******************************************************************//**
Duplicate a string.
@return < 0 if n1 < n2, 0 if n1 == n2, > 0 if n1 > n2 */
UNIV_INLINE
void
fts_string_dup(
/*===========*/
	fts_string_t*		dst,		/*!< in: dup to here */
	const fts_string_t*	src,		/*!< in: src string */
	mem_heap_t*		heap)		/*!< in: heap to use */
{
	dst->f_str = (byte*)mem_heap_alloc(heap, src->f_len + 1);
	memcpy(dst->f_str, src->f_str, src->f_len);

	dst->f_len = src->f_len;
	dst->f_str[src->f_len] = 0;
	dst->f_n_char = src->f_n_char;
}

/******************************************************************//**
Duplicate a string with lower case conversion */
UNIV_INLINE
fts_string_t
fts_string_dup_casedn(
/*===========*/
	CHARSET_INFO *cs,			/*!< in: the character set */
	const fts_string_t&	src,		/*!< in: src string */
	mem_heap_t*		heap)		/*!< in: heap to use */
{
	size_t dst_nbytes = src.f_len * cs->casedn_multiply() + 1;
	fts_string_t dst;
	dst.f_str = (byte*)mem_heap_alloc(heap, dst_nbytes);
	dst.f_len = cs->casedn_z((const char *) src.f_str, src.f_len,
				(char *) dst.f_str, dst_nbytes);
	dst.f_n_char = src.f_n_char;
	return dst;
}

/******************************************************************//**
Get the first character's code position for FTS index partition */
extern
ulint
innobase_strnxfrm(
/*==============*/
        const CHARSET_INFO*	cs,	/*!< in: Character set */
        const uchar*		p2,	/*!< in: string */
        const ulint		len2);	/*!< in: string length */

/** Check if fts index charset is cjk
@param[in]	cs	charset
@retval	true	if the charset is cjk
@retval	false	if not. */
inline bool fts_is_charset_cjk(const CHARSET_INFO* cs)
{
	switch (cs->number) {
	case 24: /* my_charset_gb2312_chinese_ci */
	case 28: /* my_charset_gbk_chinese_ci */
	case 1: /* my_charset_big5_chinese_ci */
	case 12: /* my_charset_ujis_japanese_ci */
	case 13: /* my_charset_sjis_japanese_ci */
	case 95: /* my_charset_cp932_japanese_ci */
	case 97: /* my_charset_eucjpms_japanese_ci */
	case 19: /* my_charset_euckr_korean_ci */
		return true;
	default:
		return false;
	}
}

/** Select the FTS auxiliary index for the given character by range.
@param[in]	cs	charset
@param[in]	str	string
@param[in]	len	string length
@retval	the index to use for the string */
UNIV_INLINE
ulint
fts_select_index_by_range(
	const CHARSET_INFO*	cs,
	const byte*		str,
	ulint			len)
{
	ulint			selected = 0;
	ulint			value = innobase_strnxfrm(cs, str, len);

	while (fts_index_selector[selected].value != 0) {

		if (fts_index_selector[selected].value == value) {

			return(selected);

		} else if (fts_index_selector[selected].value > value) {

			return(selected > 0 ? selected - 1 : 0);
		}

		++selected;
	}

	ut_ad(selected > 1);

	return(selected - 1);
}

/** Select the FTS auxiliary index for the given character by hash.
@param[in]	cs	charset
@param[in]	str	string
@param[in]	len	string length
@retval the index to use for the string */
UNIV_INLINE
ulint
fts_select_index_by_hash(
	const CHARSET_INFO*	cs,
	const byte*		str,
	ulint			len)
{
  my_hasher_st hasher= my_hasher_mysql5x();

	ut_ad(!(str == NULL && len > 0));

	if (str == NULL || len == 0) {
		return 0;
	}

	/* Get the first char */
	/* JAN: TODO: MySQL 5.7 had
	char_len = my_mbcharlen_ptr(cs, reinterpret_cast<const char*>(str),
				    reinterpret_cast<const char*>(str + len));
	*/
	size_t char_len = size_t(cs->charlen(str, str + len));

	ut_ad(char_len <= len);

	/* Get collation hash code */
	my_ci_hash_sort(&hasher, cs, str, char_len);

	return(hasher.m_nr1 % FTS_NUM_AUX_INDEX);
}

/** Select the FTS auxiliary index for the given character.
@param[in]	cs	charset
@param[in]	str	string
@param[in]	len	string length in bytes
@retval	the index to use for the string */
UNIV_INLINE
ulint
fts_select_index(
	const CHARSET_INFO*	cs,
	const byte*		str,
	ulint			len)
{
	ulint	selected;

	if (fts_is_charset_cjk(cs)) {
		selected = fts_select_index_by_hash(cs, str, len);
	} else {
		selected = fts_select_index_by_range(cs, str, len);
	}

	return(selected);
}

/******************************************************************//**
Return the selected FTS aux index suffix. */
UNIV_INLINE
const char*
fts_get_suffix(
/*===========*/
	ulint		selected)	/*!< in: selected index */
{
	return(fts_index_selector[selected].suffix);
}

#endif /* INNOBASE_FTS0TYPES_IC */

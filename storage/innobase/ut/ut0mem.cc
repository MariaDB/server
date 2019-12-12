/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, MariaDB Corporation.

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

/********************************************************************//**
@file ut/ut0mem.cc
Memory primitives

Created 5/11/1994 Heikki Tuuri
*************************************************************************/

#include "ut0mem.h"

/********************************************************************
Concatenate 3 strings.*/
char*
ut_str3cat(
/*=======*/
				/* out, own: concatenated string, must be
				freed with ut_free() */
	const char*	s1,	/* in: string 1 */
	const char*	s2,	/* in: string 2 */
	const char*	s3)	/* in: string 3 */
{
	char*	s;
	ulint	s1_len = strlen(s1);
	ulint	s2_len = strlen(s2);
	ulint	s3_len = strlen(s3);

	s = static_cast<char*>(ut_malloc_nokey(s1_len + s2_len + s3_len + 1));

	memcpy(s, s1, s1_len);
	memcpy(s + s1_len, s2, s2_len);
	memcpy(s + s1_len + s2_len, s3, s3_len);

	s[s1_len + s2_len + s3_len] = '\0';

	return(s);
}

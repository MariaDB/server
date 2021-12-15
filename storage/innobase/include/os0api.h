/***********************************************************************

Copyright (c) 2017, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

***********************************************************************/

/**************************************************//**
@file os0api.h
The interface to the helper functions.
These functions are used on os0file.h where
including full full header is not feasible and
implemented on buf0buf.cc and fil0fil.cc.
*******************************************************/

#ifndef OS_API_H
#define OS_API_H 1

/** Page control block */
class buf_page_t;

/** File Node */
struct fil_node_t;

/**
Should we punch hole to deallocate unused portion of the page.
@param[in]	bpage		Page control block
@return true if punch hole should be used, false if not */
bool
buf_page_should_punch_hole(
	const buf_page_t* bpage)
	MY_ATTRIBUTE((warn_unused_result));

/**
Calculate the length of trim (punch_hole) operation.
@param[in]	bpage		Page control block
@param[in]	write_length	Write length
@return length of the trim or zero. */
ulint
buf_page_get_trim_length(
	const buf_page_t*	bpage,
	ulint			write_length)
	MY_ATTRIBUTE((warn_unused_result));

/**
Get should we punch hole to tablespace.
@param[in]	space		Tablespace
@return true, if punch hole should be tried, false if not. */
bool
fil_node_should_punch_hole(
	const fil_node_t*	node)
	MY_ATTRIBUTE((warn_unused_result));

/**
Set punch hole to tablespace to given value.
@param[in]	space		Tablespace
@param[in]	val		value to be set. */
void
fil_space_set_punch_hole(
	fil_node_t*		node,
	bool			val);

#endif /* OS_API_H */

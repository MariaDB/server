/*****************************************************************************

Copyright (c) 2013, 2014, Facebook, Inc. All Rights Reserved.
Copyright (c) 2014, 2018, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/********************************************************************//**
@file include/ut0timer.h
Timer routines

Created 30/07/2014 Jan Lindström jan.lindstrom@skysql.com
modified from https://github.com/facebook/mysql-5.6/commit/c75a413edeb96eb99bf11d7269bdfea06f96d6b6
*************************************************************************/
#ifndef ut0timer_h
#define ut0timer_h

#include "univ.i"

/* Current timer stats */
extern struct my_timer_unit_info ut_timer;

/**************************************************************//**
Function pointer to point selected timer function.
@return	timer current value */
extern ulonglong (*ut_timer_now)(void);

/**************************************************************//**
Sets up the data required for use of my_timer_* functions.
Selects the best timer by high frequency, and tight resolution.
Points my_timer_now() to the selected timer function.
Initializes my_timer struct to contain the info for selected timer.*/
UNIV_INTERN
void ut_init_timer(void);

/**************************************************************//**
Convert native timer units in a ulonglong into microseconds in a double
@return time in microseconds */
UNIV_INLINE
double
ut_timer_to_microseconds(
/*=====================*/
	ulonglong	when); /*!< in: time where to calculate */
/**************************************************************//**
Convert microseconds in a double to native timer units in a ulonglong
@return time in microseconds */
UNIV_INLINE
ulonglong
ut_microseconds_to_timer(
/*=====================*/
	ulonglong	when); /*!< in: time where to calculate */

#include "ut0timer.ic"

#endif

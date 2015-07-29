/*****************************************************************************

Copyright (c) 2013, 2014, Facebook, Inc. All Rights Reserved.
Copyright (c) 2014, SkySQL Ab. All Rights Reserved.

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
Timer rountines

Created 30/07/2014 Jan Lindström jan.lindstrom@skysql.com
modified from https://github.com/facebook/mysql-5.6/commit/c75a413edeb96eb99bf11d7269bdfea06f96d6b6
*************************************************************************/
#ifndef ut0timer_h
#define ut0timer_h

#include "univ.i"
#include "data0type.h"
#include <my_rdtsc.h>

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
Return time passed since time then, automatically adjusted
for the estimated timer overhead.
@return	time passed since "then" */
UNIV_INLINE
ulonglong
ut_timer_since(
/*===========*/
	ulonglong	then); /*!< in: time where to calculate */
/**************************************************************//**
Get time passed since "then", and update then to now
@return time passed sinche "then" */
UNIV_INLINE
ulonglong
ut_timer_since_and_update(
/*======================*/
	ulonglong	*then); /*!< in: time where to calculate */
/**************************************************************//**
Convert native timer units in a ulonglong into seconds in a double
@return time in a seconds */
UNIV_INLINE
double
ut_timer_to_seconds(
/*=================*/
	ulonglong	when); /*!< in: time where to calculate */
/**************************************************************//**
Convert native timer units in a ulonglong into milliseconds in a double
@return time in milliseconds */
UNIV_INLINE
double
ut_timer_to_milliseconds(
/*=====================*/
	ulonglong	when); /*!< in: time where to calculate */
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

#ifndef UNIV_NONINL
#include "ut0timer.ic"
#endif

#endif

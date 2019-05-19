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
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/********************************************************************//**
@file ut/ut0timer.cc
Timer rountines

Created 30/07/2014 Jan Lindström jan.lindstrom@skysql.com
modified from https://github.com/facebook/mysql-5.6/commit/c75a413edeb96eb99bf11d7269bdfea06f96d6b6
*************************************************************************/

#include "data0type.h"
#include <my_rdtsc.h>
#include <ut0timer.h>

/**************************************************************//**
Initial timer definition
@return	0 */
static
ulonglong
ut_timer_none(void)
/*===============*/
{
	return 0;
}

/**************************************************************//**
Function pointer to point selected timer function.
@return	timer current value */
ulonglong (*ut_timer_now)(void) = &ut_timer_none;

struct my_timer_unit_info ut_timer;
extern MYSQL_PLUGIN_IMPORT MY_TIMER_INFO sys_timer_info;

/**************************************************************//**
Sets up the data required for use of my_timer_* functions.
Selects the best timer by high frequency, and tight resolution.
Points my_timer_now() to the selected timer function.
Initializes my_timer struct to contain the info for selected timer.*/
UNIV_INTERN
void
ut_init_timer(void)
/*===============*/
{
	if (sys_timer_info.cycles.frequency > 1000000 &&
	    sys_timer_info.cycles.resolution == 1) {
		ut_timer = sys_timer_info.cycles;
		ut_timer_now = &my_timer_cycles;
	} else if (sys_timer_info.nanoseconds.frequency > 1000000 &&
		 sys_timer_info.nanoseconds.resolution == 1) {
		ut_timer = sys_timer_info.nanoseconds;
		ut_timer_now = &my_timer_nanoseconds;
	} else if (sys_timer_info.microseconds.frequency >= 1000000 &&
		sys_timer_info.microseconds.resolution == 1) {
		ut_timer = sys_timer_info.microseconds;
		ut_timer_now = &my_timer_microseconds;

	} else if (sys_timer_info.milliseconds.frequency >= 1000 &&
		sys_timer_info.milliseconds.resolution == 1) {
		ut_timer = sys_timer_info.milliseconds;
		ut_timer_now = &my_timer_milliseconds;
	} else if (sys_timer_info.ticks.frequency >= 1000 &&
		 /* Will probably be false */
		sys_timer_info.ticks.resolution == 1) {
		ut_timer = sys_timer_info.ticks;
		ut_timer_now = &my_timer_ticks;
	} else {
		/* None are acceptable, so leave it as "None", and fill in struct */
		ut_timer.frequency = 1; /* Avoid div-by-zero */
		ut_timer.overhead = 0; /* Since it doesn't do anything */
		ut_timer.resolution = 10; /* Another sign it's bad */
		ut_timer.routine = 0; /* None */
	}
}

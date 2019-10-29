/*****************************************************************************

Copyright (c) 2011, 2014, Oracle and/or its affiliates. All Rights Reserved.

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
@file buf/buf0dump.h
Implements a buffer pool dump/load.

Created April 08, 2011 Vasil Dimov
*******************************************************/

#ifndef buf0dump_h
#define buf0dump_h

#include "univ.i"

/*****************************************************************//**
Starts  the buffer pool dump/load task dump/load thread and instructs it to start
a dump. This function is called by MySQL code via buffer_pool_dump_now()
and it should return immediately because the whole MySQL is frozen during
its execution. */
void
buf_dump_start();
/*============*/

/*****************************************************************//**
Starts  the buffer pool dump/load task (if not started) and instructs it to start
a load. This function is called by MySQL code via buffer_pool_load_now()
and it should return immediately because the whole MySQL is frozen during
its execution. */
void
buf_load_start();
/*============*/

/*****************************************************************//**
Aborts a currently running buffer pool load. This function is called by
MySQL code via buffer_pool_load_abort() and it should return immediately
because the whole MySQL is frozen during its execution. */
void
buf_load_abort();
/*============*/

/** Start async buffer pool load, if srv_buffer_pool_load_at_startup was set.*/
void buf_load_at_startup();

/** Wait for currently running load/dumps to finish*/
void buf_load_dump_end();

#endif /* buf0dump_h */

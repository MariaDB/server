/* Copyright (c) 2026, Shubham Chandrawanshi

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef PFS_NATIVE_FUNC_H
#define PFS_NATIVE_FUNC_H

/**
  @file storage/perfschema/pfs_native_func.h
  Function plugins PS_THREAD_ID() and PS_CURRENT_THREAD_ID().
*/

class Plugin_function;

extern Plugin_function pfs_function_ps_thread_id;
extern Plugin_function pfs_function_ps_current_thread_id;

#endif

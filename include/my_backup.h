/*****************************************************************************
Copyright (c) 2026 MariaDB plc.

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

#pragma once

#include <cstdint>

namespace backup
{

using target_dir_t= IF_WIN(const char*,int);

inline void* to_void_ptr(target_dir_t tgt) noexcept
{
  return IF_WIN(const_cast<char*>, reinterpret_cast<void*>)(tgt);
}

inline target_dir_t to_target_dir(void* ptr) noexcept
{
  return IF_WIN(static_cast<const char*>(ptr), 
                int(reinterpret_cast<uintptr_t>(ptr)));
}

#ifndef _WIN32
/** Copy a file.
@param src  source file descriptor
@param dst  target to append src to
@return error code (negative)
@retval 0   on success */
int copy_file(int src, int dst) noexcept;

/** Copy the entire file.
@param src  source file descriptor
@param dst  target to append src to
@param size amount of data to be copied
@return error code (negative)
@retval 0   on success */
int copy_file(int src, int dst, off_t size) noexcept;
#endif // _WIN32
}

/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2011-2016 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#pragma once

#ifdef WIN32
# include <windows.h>
#endif  // WIN32

#include "dat.hpp"

namespace grn {
namespace dat {

class FileImpl {
 public:
  FileImpl();
  ~FileImpl();

  void create(const char *path, UInt64 size);
  void open(const char *path);
  void close();

  void *ptr() const {
    return ptr_;
  }
  UInt64 size() const {
    return size_;
  }

  void swap(FileImpl *rhs);

  void flush();

 private:
  void *ptr_;
  UInt64 size_;

#ifdef WIN32
  HANDLE file_;
  HANDLE map_;
  LPVOID addr_;
#else  // WIN32
  int fd_;
  void *addr_;
  ::size_t length_;
#endif  // WIN32

  void create_(const char *path, UInt64 size);
  void open_(const char *path);

  // Disallows copy and assignment.
  FileImpl(const FileImpl &);
  FileImpl &operator=(const FileImpl &);
};

}  // namespace dat
}  // namespace grn

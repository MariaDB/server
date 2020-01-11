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

#include "dat.hpp"

namespace grn {
namespace dat {

// This implementation class hides environment dependent codes required for
// memory-mapped I/O.
class FileImpl;

class GRN_DAT_API File {
 public:
  File();
  ~File();

  // This function creates a file and maps the entire file to a certain range
  // of the address space. Note that a file is truncated if exists.
  void create(const char *path, UInt64 size);

  // This function opens a file and maps the entire file to a certain range of
  // the address space.
  void open(const char *path);
  void close();

  void *ptr() const;
  UInt64 size() const;

  void swap(File *rhs);

  void flush();

 private:
  FileImpl *impl_;

  // Disallows copy and assignment.
  File(const File &);
  File &operator=(const File &);
};

}  // namespace dat
}  // namespace grn

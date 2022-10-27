/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2011-2015 Brazil

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

#include "file.hpp"
#include "file-impl.hpp"

#include <new>

namespace grn {
namespace dat {

File::File() : impl_(NULL) {}

File::~File() {
  delete impl_;
}

void File::create(const char *path, UInt64 size) {
  File new_file;
  new_file.impl_ = new (std::nothrow) FileImpl;
  GRN_DAT_THROW_IF(MEMORY_ERROR, new_file.impl_ == NULL);
  new_file.impl_->create(path, size);
  new_file.swap(this);
}

void File::open(const char *path) {
  File new_file;
  new_file.impl_ = new (std::nothrow) FileImpl;
  GRN_DAT_THROW_IF(MEMORY_ERROR, new_file.impl_ == NULL);
  new_file.impl_->open(path);
  new_file.swap(this);
}

void File::close() {
  File().swap(this);
}

void *File::ptr() const {
  return (impl_ != NULL) ? impl_->ptr() : NULL;
}

UInt64 File::size() const {
  return (impl_ != NULL) ? impl_->size() : 0;
}

void File::swap(File *rhs) {
  FileImpl * const temp = impl_;
  impl_ = rhs->impl_;
  rhs->impl_ = temp;
}

void File::flush() {
  if (impl_) {
    impl_->flush();
  }
}

}  // namespace dat
}  // namespace grn

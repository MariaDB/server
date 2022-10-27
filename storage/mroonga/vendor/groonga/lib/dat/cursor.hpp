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

#include "key.hpp"

namespace grn {
namespace dat {

class GRN_DAT_API Cursor {
 public:
  Cursor() {}
  virtual ~Cursor() {}

  virtual void close() = 0;

  virtual const Key &next() = 0;

  virtual UInt32 offset() const = 0;
  virtual UInt32 limit() const = 0;
  virtual UInt32 flags() const = 0;

 private:
  // Disallows copy and assignment.
  Cursor(const Cursor &);
  Cursor &operator=(const Cursor &);
};

}  // namespace dat
}  // namespace grn

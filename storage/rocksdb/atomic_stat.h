/* This is an atomic integer abstract data type, for high-performance
   tracking of a single stat.  It intentionally permits inconsistent
   atomic operations and reads, for better performance.  This means
   that, though no data should ever be lost by this stat, reads of it
   at any time may not include all changes up to any particular point.

   So, values read from these may only be approximately correct.

   If your use-case will fail under these conditions, do not use this.

   Copyright (C) 2012 - 2014 Steaphan Greene <steaphan@gmail.com>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the
   Free Software Foundation, Inc.
   51 Franklin Street, Fifth Floor
   Boston, MA  02110-1301, USA.
*/

#ifndef _atomic_stat_h_
#define _atomic_stat_h_

#include <atomic>

template < typename TYPE >
class atomic_stat {
public:
  // Initialize value to the default for the type
  atomic_stat() : value_(TYPE()) {};

  // This enforces a strict order, as all absolute sets should
  void clear() {
    value_.store(TYPE(), std::memory_order_seq_cst);
  };

  // Reads can get any valid value, it doesn't matter which, exactly
  TYPE load() const {
    return value_.load(std::memory_order_relaxed);
  };

  // This only supplies relative arithmetic operations
  // These are all done atomically, and so can show up in any order
  void inc(const TYPE &other) {
    value_.fetch_add(other, std::memory_order_relaxed);
  };

  void dec(const TYPE &other) {
    value_.fetch_sub(other, std::memory_order_relaxed);
  };

  void inc() {
    value_.fetch_add(1, std::memory_order_relaxed);
  };

  void dec() {
    value_.fetch_sub(1, std::memory_order_relaxed);
  };

  // This will make one attempt to set the value to the max of
  // the current value, and the passed-in value.  It can fail
  // for any reason, and we only try it once.
  void set_max_maybe(const TYPE &new_val) {
    TYPE old_val = value_;
    if (new_val > old_val) {
      value_.compare_exchange_weak(old_val, new_val,
                                   std::memory_order_relaxed,
                                   std::memory_order_relaxed);
    }
  };

  // This will make one attempt to assign the value to the passed-in
  // value.  It can fail for any reason, and we only try it once.
  void set_maybe(const TYPE &new_val) {
    TYPE old_val = value_;
    value_.compare_exchange_weak(old_val, new_val,
                                 std::memory_order_relaxed,
                                 std::memory_order_relaxed);
  };

private:
  std::atomic<TYPE> value_;
};

#endif // _atomic_stat_h_

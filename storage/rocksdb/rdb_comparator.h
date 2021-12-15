/*
   Copyright (c) 2012,2015 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */
#pragma once

/* C++ system header files */
#include <string>

/* MySQL includes */
#include "./m_ctype.h"

/* RocksDB header files */
#include "rocksdb/comparator.h"

/* MyRocks header files */
#include "./rdb_utils.h"

namespace myrocks {

/*
  The keys are in form: {index_number} {mem-comparable-key}

  (todo: knowledge about this format is shared between this class and
   Rdb_key_def)
*/
class Rdb_pk_comparator : public rocksdb::Comparator {
 public:
  Rdb_pk_comparator(const Rdb_pk_comparator &) = delete;
  Rdb_pk_comparator &operator=(const Rdb_pk_comparator &) = delete;
  Rdb_pk_comparator() = default;

  // extracting from rocksdb::BytewiseComparator()->Compare() for optimization
  int Compare(const rocksdb::Slice &a, const rocksdb::Slice &b) const override {
    return a.compare(b);
  }

  const char *Name() const override { return "RocksDB_SE_v3.10"; }

  // TODO: advanced funcs:
  // - FindShortestSeparator
  // - FindShortSuccessor

  // for now, do-nothing implementations:
  void FindShortestSeparator(std::string *start,
                             const rocksdb::Slice &limit) const override {
    rocksdb::BytewiseComparator()->FindShortestSeparator(start, limit);
  }
  void FindShortSuccessor(std::string *key) const override {
    rocksdb::BytewiseComparator()->FindShortSuccessor(key);
  }
};

class Rdb_rev_comparator : public rocksdb::Comparator {
 public:
  Rdb_rev_comparator(const Rdb_rev_comparator &) = delete;
  Rdb_rev_comparator &operator=(const Rdb_rev_comparator &) = delete;
  Rdb_rev_comparator() = default;

  // extracting from rocksdb::BytewiseComparator()->Compare() for optimization
  int Compare(const rocksdb::Slice &a, const rocksdb::Slice &b) const override {
    return -a.compare(b);
  }
  const char *Name() const override { return "rev:RocksDB_SE_v3.10"; }
  void FindShortestSeparator(std::string *start,
                             const rocksdb::Slice &limit) const override {
    rocksdb::ReverseBytewiseComparator()->FindShortestSeparator(start, limit);
  }
  void FindShortSuccessor(std::string *key) const override {
    rocksdb::ReverseBytewiseComparator()->FindShortSuccessor(key);
  }
};

}  // namespace myrocks

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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */
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

  static int bytewise_compare(const rocksdb::Slice &a,
                              const rocksdb::Slice &b) {
    const size_t a_size = a.size();
    const size_t b_size = b.size();
    const size_t len = (a_size < b_size) ? a_size : b_size;
    int res;

    if ((res = memcmp(a.data(), b.data(), len)))
      return res;

    /* Ok, res== 0 */
    if (a_size != b_size) {
      return a_size < b_size ? -1 : 1;
    }
    return HA_EXIT_SUCCESS;
  }

  /* Override virtual methods of interest */

  int Compare(const rocksdb::Slice &a, const rocksdb::Slice &b) const override {
    return bytewise_compare(a, b);
  }

  const char *Name() const override { return "RocksDB_SE_v3.10"; }

  // TODO: advanced funcs:
  // - FindShortestSeparator
  // - FindShortSuccessor

  // for now, do-nothing implementations:
  void FindShortestSeparator(std::string *start,
                             const rocksdb::Slice &limit) const override {}
  void FindShortSuccessor(std::string *key) const override {}
};

class Rdb_rev_comparator : public rocksdb::Comparator {
public:
  Rdb_rev_comparator(const Rdb_rev_comparator &) = delete;
  Rdb_rev_comparator &operator=(const Rdb_rev_comparator &) = delete;
  Rdb_rev_comparator() = default;

  static int bytewise_compare(const rocksdb::Slice &a,
                              const rocksdb::Slice &b) {
    return -Rdb_pk_comparator::bytewise_compare(a, b);
  }

  int Compare(const rocksdb::Slice &a, const rocksdb::Slice &b) const override {
    return -Rdb_pk_comparator::bytewise_compare(a, b);
  }
  const char *Name() const override { return "rev:RocksDB_SE_v3.10"; }
  void FindShortestSeparator(std::string *start,
                             const rocksdb::Slice &limit) const override {}
  void FindShortSuccessor(std::string *key) const override {}
};

} // namespace myrocks

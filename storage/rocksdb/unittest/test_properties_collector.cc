/*
   Copyright (c) 2015, Facebook, Inc.

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

/* MyRocks header files */
#include "../ha_rocksdb.h"
#include "../rdb_datadic.h"

void putKeys(myrocks::Rdb_tbl_prop_coll *coll, int num, bool is_delete,
             uint64_t expected_deleted) {
  std::string str("aaaaaaaaaaaaaa");
  rocksdb::Slice sl(str.data(), str.size());

  for (int i = 0; i < num; i++) {
    coll->AddUserKey(
        sl, sl, is_delete ? rocksdb::kEntryDelete : rocksdb::kEntryPut, 0, 100);
  }
  DBUG_ASSERT(coll->GetMaxDeletedRows() == expected_deleted);
}

int main(int argc, char **argv) {
  // test the circular buffer for delete flags
  myrocks::Rdb_compact_params params;
  params.m_file_size = 333;
  params.m_deletes = 333; // irrelevant
  params.m_window = 10;

  myrocks::Rdb_tbl_prop_coll coll(nullptr, params, 0,
                                  RDB_DEFAULT_TBL_STATS_SAMPLE_PCT);

  putKeys(&coll, 2, true, 2);    // [xx]
  putKeys(&coll, 3, false, 2);   // [xxo]
  putKeys(&coll, 1, true, 3);    // [xxox]
  putKeys(&coll, 6, false, 3);   // [xxoxoooooo]
  putKeys(&coll, 3, true, 4);    // xxo[xooooooxxx]
  putKeys(&coll, 1, false, 4);   // xxox[ooooooxxxo]
  putKeys(&coll, 100, false, 4); // ....[oooooooooo]
  putKeys(&coll, 100, true, 10); // ....[xxxxxxxxxx]
  putKeys(&coll, 100, true, 10); // ....[oooooooooo]

  return 0;
}

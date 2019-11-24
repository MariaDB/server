/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2012-2015 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include <string.h>
#include <cppcutter.h>

#include <mrn_path_mapper.hpp>

namespace test_mrn_path_mapper {
  namespace db_path {
    namespace without_prefix {
      void test_normal_db() {
        mrn::PathMapper mapper("./db/", NULL);
        cppcut_assert_equal("db.mrn", mapper.db_path());
      }

      void test_normal_table() {
        mrn::PathMapper mapper("./db/table", NULL);
        cppcut_assert_equal("db.mrn", mapper.db_path());
      }

      void test_temporary_table() {
        mrn::PathMapper mapper("/tmp/mysqld.1/#sql27c5_1_0", NULL);
        cppcut_assert_equal("/tmp/mysqld.1/#sql27c5_1_0.mrn",
                            mapper.db_path());
      }
    }

    namespace with_prefix {
      void test_normal_db() {
        mrn::PathMapper mapper("./db/", "mroonga.data/");
        cppcut_assert_equal("mroonga.data/db.mrn", mapper.db_path());
      }

      void test_normal_table() {
        mrn::PathMapper mapper("./db/table", "mroonga.data/");
        cppcut_assert_equal("mroonga.data/db.mrn", mapper.db_path());
      }

      void test_temporary_table() {
        mrn::PathMapper mapper("/tmp/mysqld.1/#sql27c5_1_0", "mroonga.data/");
        cppcut_assert_equal("/tmp/mysqld.1/#sql27c5_1_0.mrn",
                            mapper.db_path());
      }
    }
  }

  namespace db_name {
    void test_normal_db() {
      mrn::PathMapper mapper("./db/", NULL);
      cppcut_assert_equal("db", mapper.db_name());
    }

    void test_normal_table() {
      mrn::PathMapper mapper("./db/table", NULL);
      cppcut_assert_equal("db", mapper.db_name());
    }

    void test_temporary_table() {
      mrn::PathMapper mapper("/tmp/mysqld.1/#sql27c5_1_0", NULL);
      cppcut_assert_equal("/tmp/mysqld.1/#sql27c5_1_0",
                          mapper.db_name());
    }
  }

  namespace table_name {
    void test_normal_table() {
      mrn::PathMapper mapper("./db/table", NULL);
      cppcut_assert_equal("table", mapper.table_name());
    }

    void test_temporary_table() {
      mrn::PathMapper mapper("/tmp/mysqld.1/#sql27c5_1_0", NULL);
      cppcut_assert_equal("#sql27c5_1_0", mapper.table_name());
    }

    void test_underscore_start_table() {
      mrn::PathMapper mapper("./db/_table", NULL);
      cppcut_assert_equal("@005ftable", mapper.table_name());
    }
  }

  namespace mysql_table_name {
    void test_normal_table() {
      mrn::PathMapper mapper("./db/table", NULL);
      cppcut_assert_equal("table", mapper.mysql_table_name());
    }

    void test_temporary_table() {
      mrn::PathMapper mapper("/tmp/mysqld.1/#sql27c5_1_0", NULL);
      cppcut_assert_equal("#sql27c5_1_0", mapper.mysql_table_name());
    }

    void test_underscore_start_table() {
      mrn::PathMapper mapper("./db/_table", NULL);
      cppcut_assert_equal("_table", mapper.mysql_table_name());
    }
  }

  namespace mysql_path {
    void test_normal_table() {
      mrn::PathMapper mapper("./db/table");
      cppcut_assert_equal("./db/table", mapper.mysql_path());
    }

    void test_temporary_table() {
      mrn::PathMapper mapper("/tmp/mysqld.1/#sql27c5_1_0");
      cppcut_assert_equal("/tmp/mysqld.1/#sql27c5_1_0",
                          mapper.mysql_path());
    }

    void test_partition_table_path() {
      mrn::PathMapper mapper("./db/table#P#p1");
      cppcut_assert_equal("./db/table", mapper.mysql_path());
    }
  }
}


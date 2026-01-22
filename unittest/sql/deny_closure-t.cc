/*
   Copyright (c) 2026, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include <tap.h>
#include "deny_closure.h"
#include "lex_ident.h"

CHARSET_INFO *table_alias_charset= &my_charset_bin;

static bool equals(const deny_entry_t &a, const deny_entry_t &b)
{
  return a.type == b.type &&
         a.db == b.db &&
         a.table == b.table &&
         a.column == b.column &&
         a.denies == b.denies &&
         a.subtree_denies == b.subtree_denies;
}

static void diag_entry(const char *prefix, const deny_entry_t &e)
{
  diag("%s type=%d db='%s' table='%s' column='%s' denies=%llu subtree=%llu",
       prefix,
       static_cast<int>(e.type),
       e.db.c_str(),
       e.table.c_str(),
       e.column.c_str(),
       static_cast<unsigned long long>(e.denies),
       static_cast<unsigned long long>(e.subtree_denies));
}

static void expect_entries(const char *label,
                           const deny_set_t &actual,
                           const deny_set_t &expected)
{
  ok(actual.size() == expected.size(), "%s: size", label);

  size_t n= actual.size() < expected.size() ? actual.size() : expected.size();
  for (size_t i= 0; i < n; ++i)
  {
    bool match= equals(actual[i], expected[i]);
    ok(match, "%s: entry %zu", label, i);
    if (!match)
    {
      diag_entry("expected:", expected[i]);
      diag_entry("actual  :", actual[i]);
    }
  }
}

// Single column input should materialize global/db/table parents and subtree denies.
static void test_single_column()
{
  const deny_set_t input= {
    {PRIV_TYPE_COLUMN, "db1", "t1", "c1", SELECT_ACL, NO_ACL},
  };

  const deny_set_t expected= {
    {PRIV_TYPE_GLOBAL, "", "", "", NO_ACL, SELECT_ACL},
    {PRIV_TYPE_DB, "db1", "", "", NO_ACL, SELECT_ACL},
    {PRIV_TYPE_TABLE, "db1", "t1", "", NO_ACL, SELECT_ACL},
    {PRIV_TYPE_COLUMN, "db1", "t1", "c1", SELECT_ACL, NO_ACL},
  };

  expect_entries("single column", build_deny_closure(input), expected);
}

// Mixed table and column denies should aggregate subtree denies at db/global.
static void test_complex_closure()
{
  const deny_set_t input= {
    {PRIV_TYPE_TABLE, "db1", "t1", "", INSERT_ACL, NO_ACL},
    {PRIV_TYPE_COLUMN, "db1", "t1", "c1", SELECT_ACL, NO_ACL},
    {PRIV_TYPE_TABLE, "db1", "t2", "", UPDATE_ACL, NO_ACL},
  };

  const privilege_t expected_db_subtree= (SELECT_ACL | INSERT_ACL | UPDATE_ACL);
  const deny_set_t expected= {
    {PRIV_TYPE_GLOBAL, "", "", "", NO_ACL, expected_db_subtree},
    {PRIV_TYPE_DB, "db1", "", "", NO_ACL, expected_db_subtree},
    {PRIV_TYPE_TABLE, "db1", "t1", "", INSERT_ACL, SELECT_ACL},
    {PRIV_TYPE_TABLE, "db1", "t2", "", UPDATE_ACL, NO_ACL},
    {PRIV_TYPE_COLUMN, "db1", "t1", "c1", SELECT_ACL, NO_ACL},
  };

  expect_entries("complex", build_deny_closure(input), expected);
}

// Adding a deny should yield delta entries for global/db/table with computed subtree.
static void test_diff_add()
{
  const deny_set_t old_input= {
    {PRIV_TYPE_TABLE, "db1", "t1", "", INSERT_ACL, NO_ACL},
  };
  const deny_set_t new_input= {
    {PRIV_TYPE_TABLE, "db1", "t1", "", INSERT_ACL, NO_ACL},
    {PRIV_TYPE_COLUMN, "db1", "t1", "c1", SELECT_ACL, NO_ACL},
  };

  const privilege_t expected_db_subtree= (INSERT_ACL | SELECT_ACL);
  const deny_set_t expected= {
    {PRIV_TYPE_GLOBAL, "", "", "", NO_ACL, expected_db_subtree},
    {PRIV_TYPE_DB, "db1", "", "", NO_ACL, expected_db_subtree},
    {PRIV_TYPE_TABLE, "db1", "t1", "", INSERT_ACL, SELECT_ACL},
    {PRIV_TYPE_COLUMN, "db1", "t1", "c1", SELECT_ACL, NO_ACL},
  };

  expect_entries("diff add", diff_deny_closure_inputs(old_input, new_input), expected);
}

// Removing a deny should yield the same identities with denies cleared to NO_ACL.
static void test_diff_remove()
{
  const deny_set_t old_input= {
    {PRIV_TYPE_TABLE, "db1", "t1", "", INSERT_ACL, NO_ACL},
    {PRIV_TYPE_COLUMN, "db1", "t1", "c1", SELECT_ACL, NO_ACL},
  };
  const deny_set_t new_input= {
    {PRIV_TYPE_TABLE, "db1", "t1", "", INSERT_ACL, NO_ACL},
  };

  const deny_set_t expected= {
    {PRIV_TYPE_GLOBAL, "", "", "", NO_ACL, INSERT_ACL},
    {PRIV_TYPE_DB, "db1", "", "", NO_ACL, INSERT_ACL},
    {PRIV_TYPE_TABLE, "db1", "t1", "", INSERT_ACL, NO_ACL},
    {PRIV_TYPE_COLUMN, "db1", "t1", "c1", NO_ACL, NO_ACL},
  };

  expect_entries("diff remove", diff_deny_closure_inputs(old_input, new_input),
                 expected);
}

int main(int, char *argv[])
{
  MY_INIT(argv[0]);
  table_alias_charset= &my_charset_bin;

  plan(21);

  test_single_column();
  test_complex_closure();
  test_diff_add();
  test_diff_remove();

  my_end(0);
  return exit_status();
}

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

static deny_entry_t make_entry(ACL_PRIV_TYPE type,
                               const char *db,
                               const char *table,
                               const char *column,
                               privilege_t denies)
{
  deny_entry_t entry;
  entry.type= type;
  entry.db= db ? db : "";
  entry.table= table ? table : "";
  entry.column= column ? column : "";
  entry.denies= denies;
  entry.subtree_denies= NO_ACL;
  return entry;
}

static const deny_entry_t *find_entry(const deny_set_t &set,
                                      ACL_PRIV_TYPE type,
                                      const char *db,
                                      const char *table,
                                      const char *column)
{
  for (const auto &entry : set)
  {
    if (deny_matches(entry.type, entry.db.c_str(), entry.table.c_str(),
                     entry.column.c_str(), type, db, table, column))
      return &entry;
  }

  return NULL;
}

// Single column input should materialize global/db/table parents and subtree denies.
static void test_single_column()
{
  deny_set_t input;
  input.push_back(make_entry(PRIV_TYPE_COLUMN, "db1", "t1", "c1", SELECT_ACL));

  deny_set_t closure= build_deny_closure(input);

  ok(closure.size() == 4, "single column: closure size");

  const deny_entry_t *global= find_entry(closure, PRIV_TYPE_GLOBAL, "", "", "");
  ok(global != NULL, "single column: global present");
  SKIP_BLOCK_IF(!global, 2, "single column: global missing")
  {
    ok(global->denies == NO_ACL, "single column: global denies");
    ok(global->subtree_denies == SELECT_ACL, "single column: global subtree");
  }

  const deny_entry_t *db= find_entry(closure, PRIV_TYPE_DB, "db1", "", "");
  ok(db != NULL, "single column: db present");
  SKIP_BLOCK_IF(!db, 2, "single column: db missing")
  {
    ok(db->denies == NO_ACL, "single column: db denies");
    ok(db->subtree_denies == SELECT_ACL, "single column: db subtree");
  }

  const deny_entry_t *table= find_entry(closure, PRIV_TYPE_TABLE, "db1", "t1", "");
  ok(table != NULL, "single column: table present");
  SKIP_BLOCK_IF(!table, 2, "single column: table missing")
  {
    ok(table->denies == NO_ACL, "single column: table denies");
    ok(table->subtree_denies == SELECT_ACL, "single column: table subtree");
  }

  const deny_entry_t *column= find_entry(closure, PRIV_TYPE_COLUMN, "db1", "t1", "c1");
  ok(column != NULL, "single column: column present");
  SKIP_BLOCK_IF(!column, 2, "single column: column missing")
  {
    ok(column->denies == SELECT_ACL, "single column: column denies");
    ok(column->subtree_denies == NO_ACL, "single column: column subtree");
  }
}

// Mixed table and column denies should aggregate subtree denies at db/global.
static void test_complex_closure()
{
  deny_set_t input;
  input.push_back(make_entry(PRIV_TYPE_TABLE, "db1", "t1", "", INSERT_ACL));
  input.push_back(make_entry(PRIV_TYPE_COLUMN, "db1", "t1", "c1", SELECT_ACL));
  input.push_back(make_entry(PRIV_TYPE_TABLE, "db1", "t2", "", UPDATE_ACL));

  deny_set_t closure= build_deny_closure(input);

  ok(closure.size() == 5, "complex: closure size");

  privilege_t expected_db_subtree= (SELECT_ACL | INSERT_ACL | UPDATE_ACL);

  const deny_entry_t *global= find_entry(closure, PRIV_TYPE_GLOBAL, "", "", "");
  ok(global != NULL, "complex: global present");
  SKIP_BLOCK_IF(!global, 2, "complex: global missing")
  {
    ok(global->denies == NO_ACL, "complex: global denies");
    ok(global->subtree_denies == expected_db_subtree, "complex: global subtree");
  }

  const deny_entry_t *db= find_entry(closure, PRIV_TYPE_DB, "db1", "", "");
  ok(db != NULL, "complex: db present");
  SKIP_BLOCK_IF(!db, 2, "complex: db missing")
  {
    ok(db->denies == NO_ACL, "complex: db denies");
    ok(db->subtree_denies == expected_db_subtree, "complex: db subtree");
  }

  const deny_entry_t *table1= find_entry(closure, PRIV_TYPE_TABLE, "db1", "t1", "");
  ok(table1 != NULL, "complex: table t1 present");
  SKIP_BLOCK_IF(!table1, 2, "complex: table t1 missing")
  {
    ok(table1->denies == INSERT_ACL, "complex: table t1 denies");
    ok(table1->subtree_denies == SELECT_ACL, "complex: table t1 subtree");
  }

  const deny_entry_t *table2= find_entry(closure, PRIV_TYPE_TABLE, "db1", "t2", "");
  ok(table2 != NULL, "complex: table t2 present");
  SKIP_BLOCK_IF(!table2, 2, "complex: table t2 missing")
  {
    ok(table2->denies == UPDATE_ACL, "complex: table t2 denies");
    ok(table2->subtree_denies == NO_ACL, "complex: table t2 subtree");
  }

  const deny_entry_t *column= find_entry(closure, PRIV_TYPE_COLUMN, "db1", "t1", "c1");
  ok(column != NULL, "complex: column c1 present");
  SKIP_BLOCK_IF(!column, 2, "complex: column c1 missing")
  {
    ok(column->denies == SELECT_ACL, "complex: column c1 denies");
    ok(column->subtree_denies == NO_ACL, "complex: column c1 subtree");
  }
}

// Adding a deny should yield delta entries for global/db/table with computed subtree.
static void test_diff_add()
{
  deny_set_t old_input;
  deny_set_t new_input;
  old_input.push_back(make_entry(PRIV_TYPE_TABLE, "db1", "t1", "", INSERT_ACL));
  new_input.push_back(make_entry(PRIV_TYPE_TABLE, "db1", "t1", "", INSERT_ACL));
  new_input.push_back(make_entry(PRIV_TYPE_COLUMN, "db1", "t1", "c1", SELECT_ACL));

  deny_set_t delta= diff_deny_closure_inputs(old_input, new_input);

  ok(delta.size() == 4, "diff add: delta size");

  const deny_entry_t *column= find_entry(delta, PRIV_TYPE_COLUMN, "db1", "t1", "c1");
  ok(column != NULL, "diff add: column present");
  SKIP_BLOCK_IF(!column, 2, "diff add: column missing")
  {
    ok(column->denies == SELECT_ACL, "diff add: column denies");
    ok(column->subtree_denies == NO_ACL, "diff add: column subtree");
  }

  const deny_entry_t *table= find_entry(delta, PRIV_TYPE_TABLE, "db1", "t1", "");
  ok(table != NULL, "diff add: table present");
  SKIP_BLOCK_IF(!table, 2, "diff add: table missing")
  {
    ok(table->denies == INSERT_ACL, "diff add: table denies");
    ok(table->subtree_denies == SELECT_ACL, "diff add: table subtree");
  }

  const deny_entry_t *db= find_entry(delta, PRIV_TYPE_DB, "db1", "", "");
  ok(db != NULL, "diff add: db present");
  SKIP_BLOCK_IF(!db, 2, "diff add: db missing")
  {
    ok(db->denies == NO_ACL, "diff add: db denies");
    ok(db->subtree_denies == (INSERT_ACL | SELECT_ACL), "diff add: db subtree");
  }

  const deny_entry_t *global= find_entry(delta, PRIV_TYPE_GLOBAL, "", "", "");
  ok(global != NULL, "diff add: global present");
  SKIP_BLOCK_IF(!global, 2, "diff add: global missing")
  {
    ok(global->denies == NO_ACL, "diff add: global denies");
    ok(global->subtree_denies == (INSERT_ACL | SELECT_ACL), "diff add: global subtree");
  }
}

// Removing a deny should yield the same identities with denies cleared to NO_ACL.
static void test_diff_remove()
{
  deny_set_t old_input;
  deny_set_t new_input;
  old_input.push_back(make_entry(PRIV_TYPE_TABLE, "db1", "t1", "", INSERT_ACL));
  old_input.push_back(make_entry(PRIV_TYPE_COLUMN, "db1", "t1", "c1", SELECT_ACL));
  new_input.push_back(make_entry(PRIV_TYPE_TABLE, "db1", "t1", "", INSERT_ACL));

  deny_set_t delta= diff_deny_closure_inputs(old_input, new_input);

  ok(delta.size() == 4, "diff remove: delta size");

  const deny_entry_t *column= find_entry(delta, PRIV_TYPE_COLUMN, "db1", "t1", "c1");
  ok(column != NULL, "diff remove: column present");
  SKIP_BLOCK_IF(!column, 2, "diff remove: column missing")
  {
    ok(column->denies == NO_ACL, "diff remove: column denies");
    ok(column->subtree_denies == NO_ACL, "diff remove: column subtree");
  }

  const deny_entry_t *table= find_entry(delta, PRIV_TYPE_TABLE, "db1", "t1", "");
  ok(table != NULL, "diff remove: table present");
  SKIP_BLOCK_IF(!table, 2, "diff remove: table missing")
  {
    ok(table->denies == INSERT_ACL, "diff remove: table denies");
    ok(table->subtree_denies == NO_ACL, "diff remove: table subtree");
  }

  const deny_entry_t *db= find_entry(delta, PRIV_TYPE_DB, "db1", "", "");
  ok(db != NULL, "diff remove: db present");
  SKIP_BLOCK_IF(!db, 2, "diff remove: db missing")
  {
    ok(db->denies == NO_ACL, "diff remove: db denies");
    ok(db->subtree_denies == INSERT_ACL, "diff remove: db subtree");
  }

  const deny_entry_t *global= find_entry(delta, PRIV_TYPE_GLOBAL, "", "", "");
  ok(global != NULL, "diff remove: global present");
  SKIP_BLOCK_IF(!global, 2, "diff remove: global missing")
  {
    ok(global->denies == NO_ACL, "diff remove: global denies");
    ok(global->subtree_denies == INSERT_ACL, "diff remove: global subtree");
  }
}

int main(int, char *argv[])
{
  MY_INIT(argv[0]);
  table_alias_charset= &my_charset_bin;

  plan(55);

  test_single_column();
  test_complex_closure();
  test_diff_add();
  test_diff_remove();

  my_end(0);
  return exit_status();
}

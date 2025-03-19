/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef RPL_FILTER_H
#define RPL_FILTER_H

#include "mysql.h"
#include "mysqld.h"
#include "sql_list.h"                           /* I_List */
#include "hash.h"                               /* HASH */
#include "filter.hpp"

class String;
struct TABLE_LIST;
typedef struct st_dynamic_array DYNAMIC_ARRAY;


/*
  Rpl_filter

  Inclusion and exclusion rules of tables and databases.
  Also handles rewrites of db.
  Used for replication and binlogging.
 */
class Rpl_filter 
{
public:
  Rpl_filter();
  ~Rpl_filter();
  Rpl_filter(Rpl_filter const&);
  Rpl_filter& operator=(Rpl_filter const&);
 
  /* Checks - returns true if ok to replicate/log */

#ifndef MYSQL_CLIENT
  bool tables_ok(const char* db, TABLE_LIST *tables);
#endif 
  bool db_ok(const char* db);
  bool db_ok_with_wild_table(const char *db);

  bool is_on();
  bool is_db_empty();

  /* Filters */
  const RewriteDB rewrite_db;

  const class IgnoreDB: public HashFilter<const Binary_string *>
  {
    inline void
    hash_element_append_string(uchar *element, String *out_string) override;
  protected:
    inline virtual bool
    add_rule(const char *rule, const char *rule_end) override;
  public:
    IgnoreDB();
    virtual bool operator[](const Binary_string *key) override;
  } ignore_db;
  const class DoDB: public InvertedFilter<IgnoreDB> {} do_db;

  const class IgnoreTable: public IgnoreDB
  {
    inline bool add_rule(const char *rule, const char *rule_end) override;
  } ignore_table;
  const class DoTable: public InvertedFilter<IgnoreTable> {} do_table;

  const class WildIgnoreTable: public IgnoreTable
  {
    virtual bool operator[](const Binary_string *key) override;
  } wild_ignore_table;
  const class WildDoTable: public InvertedFilter<WildIgnoreTable>
  {} wild_do_table;

  void set_parallel_mode(enum_slave_parallel_mode mode)
  {
    parallel_mode= mode;
  }
  /* Return given parallel mode or if one is not given, the default mode */
  enum_slave_parallel_mode get_parallel_mode()
  {
    return parallel_mode;
  }

private:
  enum_slave_parallel_mode parallel_mode;

  bool table_rules_on;
};

extern Rpl_filter *global_rpl_filter;
extern Rpl_filter *binlog_filter;

#endif // RPL_FILTER_H

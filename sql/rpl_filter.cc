/* Copyright (c) 2000, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2009, 2020, MariaDB Corporation.
   
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

#include "mariadb.h"
#include "sql_priv.h"
#include "mysqld.h"                             // system_charset_info
#include "rpl_filter.h"
#include "hash.h"                               // my_hash_free
#include "table.h"                              // TABLE_LIST

#define TABLE_RULE_HASH_SIZE   16
#define TABLE_RULE_ARR_SIZE   16

Rpl_filter::Rpl_filter() : 
  parallel_mode(SLAVE_PARALLEL_OPTIMISTIC),
  table_rules_on(0),
  rewrite_db(),
  ignore_db(), do_db(), ignore_table(), do_table(),
  wild_ignore_table(), wild_do_table()
{}

#ifndef MYSQL_CLIENT
/*
  Returns true if table should be logged/replicated 

  SYNOPSIS
    tables_ok()
    db              db to use if db in TABLE_LIST is undefined for a table
    tables          list of tables to check

  NOTES
    Changing table order in the list can lead to different results. 
    
    Note also order of precedence of do/ignore rules (see code).  For
    that reason, users should not set conflicting rules because they
    may get unpredicted results (precedence order is explained in the
    manual).

    If no table in the list is marked "updating", then we always
    return 0, because there is no reason to execute this statement on
    slave if it updates nothing.  (Currently, this can only happen if
    statement is a multi-delete (SQLCOM_DELETE_MULTI) and "tables" are
    the tables in the FROM):

    In the case of SQLCOM_DELETE_MULTI, there will be a second call to
    tables_ok(), with tables having "updating==TRUE" (those after the
    DELETE), so this second call will make the decision (because
    all_tables_not_ok() = !tables_ok(1st_list) &&
    !tables_ok(2nd_list)).

  TODO
    "Include all tables like "abc.%" except "%.EFG"". (Can't be done now.)
    If we supported Perl regexps, we could do it with pattern: /^abc\.(?!EFG)/
    (I could not find an equivalent in the regex library MySQL uses).

  RETURN VALUES
    0           should not be logged/replicated
    1           should be logged/replicated                  
*/

bool 
Rpl_filter::tables_ok(const char* db, TABLE_LIST* tables)
{
  /*
  bool some_tables_updating= 0;
  DBUG_ENTER("Rpl_filter::tables_ok");
  
  for (; tables; tables= tables->next_global)
  {
    char hash_key[SAFE_NAME_LEN*2+2];
    char *end;
    uint len;

    if (!tables->updating) 
      continue;
    some_tables_updating= 1;
    end= strmov(hash_key, tables->db.str ? tables->db.str : db);
    *end++= '.';
    len= (uint) (strmov(end, tables->table_name.str) - hash_key);
  //  if (do_table_inited) // if there are any do's
  //  {
  //    if (my_hash_search(&do_table, (uchar*) hash_key, len))
	//DBUG_RETURN(1);
  //  }
  //  if (ignore_table_inited) // if there are any ignores
  //  {
  //    if (my_hash_search(&ignore_table, (uchar*) hash_key, len))
	//DBUG_RETURN(0); 
  //  }
    if (wild_do_table_inited && 
	find_wild(&wild_do_table, hash_key, len))
      DBUG_RETURN(1);
    if (wild_ignore_table_inited && 
	find_wild(&wild_ignore_table, hash_key, len))
      DBUG_RETURN(0);
  }

  /*
    If no table was to be updated, ignore statement (no reason we play it on
    slave, slave is supposed to replicate _changes_ only).
    If no explicit rule found and there was a do list, do not replicate.
    If there was no do list, go ahead
  */
  //DBUG_RETURN(some_tables_updating &&
  //            !do_table_inited && !wild_do_table_inited);
  return true;
}

#endif

/*
  Checks whether a db matches some do_db and ignore_db rules

  SYNOPSIS
    db_ok()
    db              name of the db to check

  RETURN VALUES
    0           should not be logged/replicated
    1           should be logged/replicated                  
*/

bool
Rpl_filter::db_ok(const char* db)
{
  return true;
//  DBUG_ENTER("Rpl_filter::db_ok");
//
//  if (do_db.is_empty() && ignore_db.is_empty())
//    DBUG_RETURN(1); // Ok to replicate if the user puts no constraints
//
//  /*
//    Previous behaviour "if the user has specified restrictions on which
//    databases to replicate and db was not selected, do not replicate" has
//    been replaced with "do replicate".
//    Since the filtering criteria is not equal to "NULL" the statement should
//    be logged into binlog.
//  */
//  if (!db)
//    DBUG_RETURN(1);
//
//  if (!do_db.is_empty()) // if the do's are not empty
//  {
//    I_List_iterator<i_string> it(do_db);
//    i_string* tmp;
//
//    while ((tmp=it++))
//    {
//      if (!strcmp(tmp->ptr, db))
//	DBUG_RETURN(1); // match
//    }
//    DBUG_PRINT("exit", ("Don't replicate"));
//    DBUG_RETURN(0);
//  }
//  else // there are some elements in the don't, otherwise we cannot get here
//  {
//    I_List_iterator<i_string> it(ignore_db);
//    i_string* tmp;
//
//    while ((tmp=it++))
//    {
//      if (!strcmp(tmp->ptr, db))
//      {
//        DBUG_PRINT("exit", ("Don't replicate"));
//	DBUG_RETURN(0); // match
//      }
//    }
//    DBUG_RETURN(1);
//  }
}


/*
  Checks whether a db matches wild_do_table and wild_ignore_table
  rules (for replication)

  SYNOPSIS
    db_ok_with_wild_table()
    db          name of the db to check. Is tested with
                Lex_ident_db::check_name() before calling this function.

  NOTES
    Here is the reason for this function.
    We advise users who want to exclude a database 'db1' safely to do it
    with replicate_wild_ignore_table='db1.%' instead of binlog_ignore_db or
    replicate_ignore_db because the two lasts only check for the selected db,
    which won't work in that case:
    USE db2;
    UPDATE db1.t SET ... #this will be replicated and should not
    whereas replicate_wild_ignore_table will work in all cases.
    With replicate_wild_ignore_table, we only check tables. When
    one does 'DROP DATABASE db1', tables are not involved and the
    statement will be replicated, while users could expect it would not (as it
    rougly means 'DROP db1.first_table, DROP db1.second_table...').
    In other words, we want to interpret 'db1.%' as "everything touching db1".
    That is why we want to match 'db1' against 'db1.%' wild table rules.

  RETURN VALUES
    0           should not be logged/replicated
    1           should be logged/replicated
*/

bool
Rpl_filter::db_ok_with_wild_table(const char *db)
{
  return true;
//  DBUG_ENTER("Rpl_filter::db_ok_with_wild_table");
//
//  char hash_key[SAFE_NAME_LEN+2];
//  char *end;
//  int len;
//  end= strmov(hash_key, db);
//  *end++= '.';
//  len= (int)(end - hash_key);
//  if (wild_do_table_inited && find_wild(&wild_do_table, hash_key, len))
//  {
//    DBUG_PRINT("return",("1"));
//    DBUG_RETURN(1);
//  }
//  if (wild_ignore_table_inited && find_wild(&wild_ignore_table, hash_key, len))
//  {
//    DBUG_PRINT("return",("0"));
//    DBUG_RETURN(0);
//  }  
//
//  /*
//    If no explicit rule found and there was a do list, do not replicate.
//    If there was no do list, go ahead
//  */
//  DBUG_PRINT("return",("db=%s,retval=%d", db, !wild_do_table_inited));
//  DBUG_RETURN(!wild_do_table_inited);
}

/*
bool
Rpl_filter::is_on()
{
  return table_rules_on;
}


bool
Rpl_filter::is_db_empty()
{
  return do_db.is_empty() && ignore_db.is_empty();
}*/

void Rpl_filter::IgnoreDB::
hash_element_append_string(uchar *element, String *out_string)
{ out_string->append(*reinterpret_cast<String *>(element)); }

Rpl_filter::IgnoreDB::IgnoreDB(): HashFilter()
{
  my_hash_init(key_memory_TABLE_RULE_ENT, &hashset,
               Lex_ident_rpl_filter::charset_info(), TABLE_RULE_HASH_SIZE, 0, 0,
               [] (const void *p, size_t *len, my_bool)
               {
                 auto e= static_cast<const String *>(p);
                 (*len)= e->length();
                 return reinterpret_cast<const uchar *>(e->ptr());
               },
               [] (void *p)
               { // my_free() is not `operator delete`
                 auto e= static_cast<String *>(p);
                 e->~String();
                 my_free(p);
               }, 0);
}

bool Rpl_filter::IgnoreDB::operator[](const Binary_string *key)
{
  return my_hash_search
    (&hashset, reinterpret_cast<const uchar *>(key->ptr()), key->length());
}

bool Rpl_filter::IgnoreDB::add_rule(const char *rule, const char *rule_end)
{
  auto element= static_cast<String *>
    (my_malloc(key_memory_TABLE_RULE_ENT, sizeof(String), MYF(MY_WME)));
  if (!element)
    return true;
  element->set(rule, rule_end - rule, hashset.charset);
  return my_hash_insert(&hashset, reinterpret_cast<const uchar *>(element));
}


bool Rpl_filter::IgnoreTable::add_rule(const char *rule, const char *rule_end)
{
  // Assert there exists a '.'
  for(const char *chr= rule; chr < rule_end; ++rule)
    if (*chr == '.')
      return Rpl_filter::IgnoreDB::add_rule(rule, rule_end);
  return true;
}

bool Rpl_filter::WildIgnoreTable::operator[](const Binary_string *key)
{
  const char *str= key->ptr(), *str_end= key->end();
  for (ulong idx= 0; idx < hashset.records; ++idx)
  {
    auto e= reinterpret_cast<String *>(my_hash_element(&hashset, idx));
    if (system_charset_info->wildcmp(str, str_end, e->ptr(), e->end(),
                                     '\\', wild_one, wild_many))
      return true;
  }
  return false;
}

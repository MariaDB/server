/*
   Copyright (c) 2024, MariaDB

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

/*
  This file contains some routines to do client-side parsing of CREATE TABLE
  statements. The goal is to extract the primary key, constraints, and
  secondary key. his is useful for optimizing the import process, to delay
  secondary index creation until after the data has been loaded.
*/

#include <string>
#include <vector>
#include <pcre2posix.h>

#include "import_util.h"
#include <assert.h>

/**
 * Extract the first CREATE TABLE statement from a script.
 *
 * @param script The input script containing SQL statements.
 * @return std::string The first CREATE TABLE statement found, or an empty
 * string if not found.
 */
std::string extract_first_create_table(const std::string &script)
{
  regex_t create_table_regex;
  regmatch_t match[2];
  const char *pattern= "(CREATE\\s+TABLE\\s+[^;]+;)\\s*\\n";
  regcomp(&create_table_regex, pattern, REG_EXTENDED);

  if (regexec(&create_table_regex, script.c_str(), 2, match, 0) == 0)
  {
    std::string result=
        script.substr(match[1].rm_so, match[1].rm_eo - match[1].rm_so);
    regfree(&create_table_regex);
    return result;
  }

  regfree(&create_table_regex);
  return "";
}

TableDDLInfo::TableDDLInfo(const std::string &create_table_stmt)
{
  regex_t primary_key_regex, constraint_regex, index_regex, engine_regex,
      table_name_regex;
  constexpr size_t MAX_MATCHES= 10;
  regmatch_t match[10];

  regcomp(&primary_key_regex, "\\n\\s*(PRIMARY\\s+KEY\\s+(.*?)),?\\n",
          REG_EXTENDED);
  regcomp(&constraint_regex,
          "\\n\\s*(CONSTRAINT\\s+(`?(?:[^`]|``)+`?)\\s+.*?),?\\n",
          REG_EXTENDED);
  regcomp(&index_regex,
          "\\n\\s*(((?:UNIQUE|FULLTEXT|VECTOR|SPATIAL)\\s+)?(INDEX|KEY)\\s+(`(?:[^`]|``)+`)\\s+.*?),?\\n",
          REG_EXTENDED);
  regcomp(&engine_regex, "\\bENGINE\\s*=\\s*(\\w+)", REG_EXTENDED);
  regcomp(&table_name_regex, "CREATE\\s+TABLE\\s+(`?(?:[^`]|``)+`?)\\s*\\(",
          REG_EXTENDED);

  const char *stmt= create_table_stmt.c_str();
  const char *search_start= stmt;

  // Extract primary key
  if (regexec(&primary_key_regex, search_start, MAX_MATCHES, match, 0) == 0)
  {
    primary_key= {std::string(stmt + match[1].rm_so,  match[1].rm_eo - match[1].rm_so),
        "PRIMARY"};
  }

  // Extract constraints and foreign keys
  search_start= stmt;
  while (regexec(&constraint_regex, search_start, MAX_MATCHES, match, 0) == 0)
  {
    assert(match[2].rm_so != -1);
    assert(match[1].rm_so != -1);
    std::string name(search_start + match[2].rm_so, match[2].rm_eo - match[2].rm_so);
    std::string definition(search_start + match[1].rm_so, match[1].rm_eo - match[1].rm_so);
    constraints.push_back({definition, name});
    search_start+= match[0].rm_eo - 1;
  }

  // Extract secondary indexes
  search_start= stmt;
  while (regexec(&index_regex, search_start, MAX_MATCHES, match, 0) == 0)
  {
    assert(match[4].rm_so != -1);
    std::string name(search_start + match[4].rm_so, match[4].rm_eo - match[4].rm_so);
    std::string definition(search_start + match[1].rm_so, match[1].rm_eo - match[1].rm_so);
    secondary_indexes.push_back({definition, name});
    search_start+= match[0].rm_eo -1;
  }

  // Extract storage engine
  if (regexec(&engine_regex, stmt, MAX_MATCHES, match, 0) == 0)
  {
    storage_engine= std::string(stmt + match[1].rm_so,  match[1].rm_eo - match[1].rm_so);
  }

  // Extract table name
  if (regexec(&table_name_regex, stmt, MAX_MATCHES, match, 0) == 0)
  {
    table_name= std::string(stmt + match[1].rm_so,  match[1].rm_eo - match[1].rm_so);
  }
  if (primary_key.definition.empty() && storage_engine == "InnoDB")
  {
    for (const auto &index : secondary_indexes)
    {
      if (index.definition.find("UNIQUE") != std::string::npos)
      {
        non_pk_clustering_key_name= index.name;
        break;
      }
    }
  }
  regfree(&primary_key_regex);
  regfree(&constraint_regex);
  regfree(&index_regex);
  regfree(&engine_regex);
  regfree(&table_name_regex);
}

/**
 Convert a KeyOrConstraintDefinitionType enum value to its
 corresponding string representation.

 @param type The KeyOrConstraintDefinitionType enum value.
 @return std::string The string representation of the
  KeyOrConstraintDefinitionType.
*/
static std::string to_string(KeyOrConstraintType type)
{
  switch (type)
  {
  case KeyOrConstraintType::CONSTRAINT:
    return "CONSTRAINT";
  case KeyOrConstraintType::INDEX:
    return "INDEX";
  default:
    return "UNKNOWN";
  }
}

std::string TableDDLInfo::generate_alter_add(
    const std::vector<KeyDefinition> &definitions,
    KeyOrConstraintType type) const
{
  if (definitions.empty() ||
      (type == KeyOrConstraintType::INDEX && definitions.size() == 1
      && !non_pk_clustering_key_name.empty()))
  {
    return "";
  }

  std::string sql= "ALTER TABLE " + table_name + " ";
  bool need_comma= false;
  for (const auto &definition : definitions)
  {
    /*
      Do not add or drop clustering secondary index
    */
    if (type == KeyOrConstraintType::INDEX &&
        definition.name == non_pk_clustering_key_name)
      continue;

    if (need_comma)
      sql+= ", ";
    else
      need_comma= true;
    sql+= "ADD " + definition.definition;
  }
  return sql;
}

std::string TableDDLInfo::generate_alter_drop(
    const std::vector<KeyDefinition> &definitions, KeyOrConstraintType type) const
{
  if (definitions.empty() ||
      (type == KeyOrConstraintType::INDEX && definitions.size() == 1 &&
       !non_pk_clustering_key_name.empty()))
  {
    return "";
  }

  std::string sql= "ALTER TABLE " + table_name + " ";
  bool need_comma= false;
  for (const auto &definition : definitions)
  {
    if (type == KeyOrConstraintType::INDEX &&
        definition.name == non_pk_clustering_key_name)
      continue;

    if (need_comma)
      sql+= ", ";
    else
      need_comma= true;
    sql+= "DROP " + to_string(type) + " " +
          definition.name;
  }
  return sql;
}

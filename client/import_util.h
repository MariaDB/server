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

#pragma once
#include <string>
#include <vector>

/* TABLE DDL INFO - representation of parsed CREATE TABLE Statement */

enum class KeyOrConstraintType
{
  CONSTRAINT,
  INDEX,
  UNKNOWN
};

/**
 *  Struct representing a table key or constraint definition
 */
struct KeyDefinition
{
  /** Full key or constraint definition string,
  e.g  UNIQUE KEY `uniq_idx` (`col`) */
  std::string definition;
  /** The name of key or constraint, including escape chars */
  std::string name;
};

/**
   Information about keys and constraints, extracted from
   CREATE TABLE statement
 */
struct TableDDLInfo
{
  TableDDLInfo(const std::string &create_table_stmt);
  KeyDefinition primary_key;
  std::vector<KeyDefinition> constraints;
  std::vector<KeyDefinition> secondary_indexes;
  std::string storage_engine;
  std::string table_name;
  /* Innodb is using first UNIQUE key for clustering, if no PK is set*/
  std::string non_pk_clustering_key_name;

  /**
    Generate ALTER TABLE ADD/DROP statements for keys or constraints.
    The goal is to remove indexes/constraints before the data is imported
    and recreate them after import.
    PRIMARY key is not affected by these operations
  */
  std::string generate_alter_add(const std::vector<KeyDefinition> &defs,
                                 KeyOrConstraintType type) const;
  std::string generate_alter_drop(const std::vector<KeyDefinition> &defs,
                                  KeyOrConstraintType type) const;


  std::string drop_constraints_sql() const
  {
    return generate_alter_drop(constraints, KeyOrConstraintType::CONSTRAINT);
  }
  std::string add_constraints_sql() const
  {
    return generate_alter_add(constraints, KeyOrConstraintType::CONSTRAINT);
  }
  std::string drop_secondary_indexes_sql() const
  {
    return generate_alter_drop(secondary_indexes,
                               KeyOrConstraintType::INDEX);
  }
  std::string add_secondary_indexes_sql() const
  {
    return generate_alter_add(secondary_indexes,
                              KeyOrConstraintType::INDEX);
  }
};
std::string extract_first_create_table(const std::string &script);

/* Copyright (c) 2010, 2014, Oracle and/or its affiliates.
   Copyright (c) 2013, 2020, MariaDB Corporation.

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

#ifndef SQL_ALTER_TABLE_H
#define SQL_ALTER_TABLE_H

class Alter_drop;
class Alter_column;
class Alter_rename_key;
class Alter_index_ignorability;
class Key;

/**
  Data describing the table being created by CREATE TABLE or
  altered by ALTER TABLE.
*/

class Alter_info
{
public:

  enum enum_enable_or_disable { LEAVE_AS_IS, ENABLE, DISABLE };

  bool vers_prohibited(THD *thd) const;

  /**
     The different values of the ALGORITHM clause.
     Describes which algorithm to use when altering the table.
  */
  enum enum_alter_table_algorithm
  {
/*
  Use thd->variables.alter_algorithm for alter method. If this is also
  default then use the fastest possible ALTER TABLE method
  (INSTANT, NOCOPY, INPLACE, COPY)
*/
    ALTER_TABLE_ALGORITHM_DEFAULT,

    // Copy if supported, error otherwise.
    ALTER_TABLE_ALGORITHM_COPY,

    // In-place if supported, error otherwise.
    ALTER_TABLE_ALGORITHM_INPLACE,

    // No Copy will refuse any operation which does rebuild.
    ALTER_TABLE_ALGORITHM_NOCOPY,

    // Instant should allow any operation that changes metadata only.
    ALTER_TABLE_ALGORITHM_INSTANT,

    // When there is no specification of algorithm during alter table.
    ALTER_TABLE_ALGORITHM_NONE
  };


  /**
     The different values of the LOCK clause.
     Describes the level of concurrency during ALTER TABLE.
  */
  enum enum_alter_table_lock
  {
    // Maximum supported level of concurency for the given operation.
    ALTER_TABLE_LOCK_DEFAULT,

    // Allow concurrent reads & writes. If not supported, give error.
    ALTER_TABLE_LOCK_NONE,

    // Allow concurrent reads only. If not supported, give error.
    ALTER_TABLE_LOCK_SHARED,

    // Block reads and writes.
    ALTER_TABLE_LOCK_EXCLUSIVE
  };


  // Columns and keys to be dropped.
  List<Alter_drop>              drop_list;
  // Columns for ALTER_CHANGE_COLUMN_DEFAULT.
  List<Alter_column>            alter_list;
  // List of keys, used by both CREATE and ALTER TABLE.
  List<Key>                     key_list;
  // List of keys to be renamed.
  List<Alter_rename_key>        alter_rename_key_list;
  // List of columns, used by both CREATE and ALTER TABLE.
  List<Create_field>            create_list;
  // Indexes whose ignorability needs to be changed.
  List<Alter_index_ignorability>  alter_index_ignorability_list;
  List<Virtual_column_info>     check_constraint_list;
  // Type of ALTER TABLE operation.
  alter_table_operations        flags;
  ulong                         partition_flags;
  // Enable or disable keys.
  enum_enable_or_disable        keys_onoff;
  // List of partitions.
  List<const char>              partition_names;
  // Number of partitions.
  uint                          num_parts;
private:
  // Type of ALTER TABLE algorithm.
  enum_alter_table_algorithm    requested_algorithm;

public:
  // Type of ALTER TABLE lock.
  enum_alter_table_lock         requested_lock;


  Alter_info() :
  flags(0), partition_flags(0),
    keys_onoff(LEAVE_AS_IS),
    num_parts(0),
    requested_algorithm(ALTER_TABLE_ALGORITHM_NONE),
    requested_lock(ALTER_TABLE_LOCK_DEFAULT)
  {}

  void reset()
  {
    drop_list.empty();
    alter_list.empty();
    key_list.empty();
    alter_rename_key_list.empty();
    create_list.empty();
    alter_index_ignorability_list.empty();
    check_constraint_list.empty();
    flags= 0;
    partition_flags= 0;
    keys_onoff= LEAVE_AS_IS;
    num_parts= 0;
    partition_names.empty();
    requested_algorithm= ALTER_TABLE_ALGORITHM_NONE;
    requested_lock= ALTER_TABLE_LOCK_DEFAULT;
  }


  /**
    Construct a copy of this object to be used for mysql_alter_table
    and mysql_create_table.

    Historically, these two functions modify their Alter_info
    arguments. This behaviour breaks re-execution of prepared
    statements and stored procedures and is compensated by always
    supplying a copy of Alter_info to these functions.

    @param  rhs       Alter_info to make copy of
    @param  mem_root  Mem_root for new Alter_info

    @note You need to use check the error in THD for out
    of memory condition after calling this function.
  */
  Alter_info(const Alter_info &rhs, MEM_ROOT *mem_root);


  /**
     Parses the given string and sets requested_algorithm
     if the string value matches a supported value.
     Supported values: INPLACE, COPY, DEFAULT

     @param  str    String containing the supplied value
     @retval false  Supported value found, state updated
     @retval true   Not supported value, no changes made
  */
  bool set_requested_algorithm(const LEX_CSTRING *str);


  /**
     Parses the given string and sets requested_lock
     if the string value matches a supported value.
     Supported values: NONE, SHARED, EXCLUSIVE, DEFAULT

     @param  str    String containing the supplied value
     @retval false  Supported value found, state updated
     @retval true   Not supported value, no changes made
  */

  bool set_requested_lock(const LEX_CSTRING *str);

  /**
    Set the requested algorithm to the given algorithm value
    @param algo_value	algorithm to be set
   */
  void set_requested_algorithm(enum_alter_table_algorithm algo_value);

  /**
     Returns the algorithm value in the format "algorithm=value"
  */
  const char* algorithm_clause(THD *thd) const;

  /**
     Returns the lock value in the format "lock=value"
  */
  const char* lock() const;

  /**
     Check whether the given result can be supported
     with the specified user alter algorithm.

     @param  thd            Thread handle
     @param  ha_alter_info  Structure describing changes to be done
                            by ALTER TABLE and holding data during
                            in-place alter
     @retval false  Supported operation
     @retval true   Not supported value
  */
  bool supports_algorithm(THD *thd,
                          const Alter_inplace_info *ha_alter_info);

  /**
     Check whether the given result can be supported
     with the specified user lock type.

     @param  ha_alter_info  Structure describing changes to be done
                            by ALTER TABLE and holding data during
                            in-place alter
     @retval false  Supported lock type
     @retval true   Not supported value
  */
  bool supports_lock(THD *thd, const Alter_inplace_info *ha_alter_info);

  /**
    Return user requested algorithm. If user does not specify
    algorithm then return alter_algorithm variable value.
   */
  enum_alter_table_algorithm algorithm(const THD *thd) const;

private:
  Alter_info &operator=(const Alter_info &rhs); // not implemented
  Alter_info(const Alter_info &rhs);            // not implemented
};


/** Runtime context for ALTER TABLE. */
class Alter_table_ctx
{
public:
  Alter_table_ctx();

  Alter_table_ctx(THD *thd, TABLE_LIST *table_list, uint tables_opened_arg,
                  const LEX_CSTRING *new_db_arg, const LEX_CSTRING *new_name_arg);

  /**
     @return true if the table is moved to another database, false otherwise.
  */
  bool is_database_changed() const
  { return (new_db.str != db.str); };

  /**
     @return true if the table is renamed, false otherwise.
  */
  bool is_table_renamed() const
  { return (is_database_changed() || new_name.str != table_name.str); };

  /**
     @return filename (including .frm) for the new table.
  */
  const char *get_new_filename() const
  {
    DBUG_ASSERT(!tmp_table);
    return new_filename;
  }

  /**
     @return path to the original table.
  */
  const char *get_path() const
  {
    DBUG_ASSERT(!tmp_table);
    return path;
  }

  /**
     @return path to the new table.
  */
  const char *get_new_path() const
  {
    DBUG_ASSERT(!tmp_table);
    return new_path;
  }

  /**
     @return path to the temporary table created during ALTER TABLE.
  */
  const char *get_tmp_path() const
  { return tmp_path; }

  const LEX_CSTRING get_tmp_cstring_path() const
  {
    LEX_CSTRING tmp= { tmp_path, strlen(tmp_path) };
    return tmp;
  };

  /**
    Mark ALTER TABLE as needing to produce foreign key error if
    it deletes a row from the table being changed.
  */
  void set_fk_error_if_delete_row(FOREIGN_KEY_INFO *fk)
  {
    fk_error_if_delete_row= true;
    fk_error_id= fk->foreign_id->str;
    fk_error_table= fk->foreign_table->str;
  }

  void report_implicit_default_value_error(THD *thd, const TABLE_SHARE *) const;
public:
  Create_field *implicit_default_value_error_field;
  bool         error_if_not_empty;
  uint         tables_opened;
  LEX_CSTRING  db;
  LEX_CSTRING  table_name;
  LEX_CSTRING  storage_engine_name;
  LEX_CSTRING  alias;
  LEX_CSTRING  new_db;
  LEX_CSTRING  new_name;
  LEX_CSTRING  new_alias;
  LEX_CSTRING  tmp_name;
  LEX_CSTRING  tmp_storage_engine_name;
  LEX_CUSTRING tmp_id, id;
  char         tmp_buff[80];
  uchar        id_buff[MY_UUID_SIZE];
  char         storage_engine_buff[NAME_LEN], tmp_storage_engine_buff[NAME_LEN];
  bool         storage_engine_partitioned;
  bool         tmp_storage_engine_name_partitioned;

  /**
    Indicates that if a row is deleted during copying of data from old version
    of table to the new version ER_FK_CANNOT_DELETE_PARENT error should be
    emitted.
  */
  bool         fk_error_if_delete_row;
  /** Name of foreign key for the above error. */
  const char   *fk_error_id;
  /** Name of table for the above error. */
  const char   *fk_error_table;
  /** Indicates that we are altering temporary table */
  bool tmp_table;

private:
  char new_filename[FN_REFLEN + 1];
  char new_alias_buff[NAME_LEN + 1];
  char tmp_name_buff[NAME_LEN + 1];
  char path[FN_REFLEN + 1];
  char new_path[FN_REFLEN + 1];
  char tmp_path[FN_REFLEN + 1];

  Alter_table_ctx &operator=(const Alter_table_ctx &rhs); // not implemented
  Alter_table_ctx(const Alter_table_ctx &rhs);            // not implemented
};


/**
  Sql_cmd_common_alter_table represents the common properties of the ALTER TABLE
  statements.
  @todo move Alter_info and other ALTER generic structures from Lex here.
*/
class Sql_cmd_common_alter_table : public Sql_cmd
{
protected:
  /**
    Constructor.
  */
  Sql_cmd_common_alter_table()
  {}

  virtual ~Sql_cmd_common_alter_table()
  {}

  virtual enum_sql_command sql_command_code() const
  {
    return SQLCOM_ALTER_TABLE;
  }
};

/**
  Sql_cmd_alter_table represents the generic ALTER TABLE statement.
  @todo move Alter_info and other ALTER specific structures from Lex here.
*/
class Sql_cmd_alter_table : public Sql_cmd_common_alter_table,
                            public Storage_engine_name
{
public:
  /**
    Constructor, used to represent a ALTER TABLE statement.
  */
  Sql_cmd_alter_table()
  {}

  ~Sql_cmd_alter_table()
  {}

  Storage_engine_name *option_storage_engine_name() { return this; }

  bool execute(THD *thd);
};


/**
  Sql_cmd_alter_sequence represents the ALTER SEQUENCE statement.
*/
class Sql_cmd_alter_sequence : public Sql_cmd,
                               public DDL_options
{
public:
  /**
    Constructor, used to represent a ALTER TABLE statement.
  */
  Sql_cmd_alter_sequence(const DDL_options &options)
   :DDL_options(options)
  {}

  ~Sql_cmd_alter_sequence()
  {}

  enum_sql_command sql_command_code() const
  {
    return SQLCOM_ALTER_SEQUENCE;
  }
  bool execute(THD *thd);
};


/**
  Sql_cmd_alter_table_tablespace represents ALTER TABLE
  IMPORT/DISCARD TABLESPACE statements.
*/
class Sql_cmd_discard_import_tablespace : public Sql_cmd_common_alter_table
{
public:
  enum enum_tablespace_op_type
  {
    DISCARD_TABLESPACE, IMPORT_TABLESPACE
  };

  Sql_cmd_discard_import_tablespace(enum_tablespace_op_type tablespace_op_arg)
    : m_tablespace_op(tablespace_op_arg)
  {}

  bool execute(THD *thd);

private:
  const enum_tablespace_op_type m_tablespace_op;
};

#endif

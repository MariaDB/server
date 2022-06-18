/* Copyright (c) 2009, 2010, Oracle and/or its affiliates. All rights reserved.

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

/**
  @file Representation of an SQL command.
*/

#ifndef SQL_CMD_INCLUDED
#define SQL_CMD_INCLUDED

/*
  When a command is added here, be sure it's also added in mysqld.cc
  in "struct show_var_st status_vars[]= {" ...

  If the command returns a result set or is not allowed in stored
  functions or triggers, please also make sure that
  sp_get_flags_for_command (sp_head.cc) returns proper flags for the
  added SQLCOM_.
*/

enum enum_sql_command {
  SQLCOM_SELECT, SQLCOM_CREATE_TABLE, SQLCOM_CREATE_INDEX, SQLCOM_ALTER_TABLE,
  SQLCOM_UPDATE, SQLCOM_INSERT, SQLCOM_INSERT_SELECT,
  SQLCOM_DELETE, SQLCOM_TRUNCATE, SQLCOM_DROP_TABLE, SQLCOM_DROP_INDEX,

  SQLCOM_SHOW_DATABASES, SQLCOM_SHOW_TABLES, SQLCOM_SHOW_FIELDS,
  SQLCOM_SHOW_KEYS, SQLCOM_SHOW_VARIABLES, SQLCOM_SHOW_STATUS,
  SQLCOM_SHOW_ENGINE_LOGS, SQLCOM_SHOW_ENGINE_STATUS, SQLCOM_SHOW_ENGINE_MUTEX,
  SQLCOM_SHOW_PROCESSLIST, SQLCOM_SHOW_BINLOG_STAT, SQLCOM_SHOW_SLAVE_STAT,
  SQLCOM_SHOW_GRANTS, SQLCOM_SHOW_CREATE, SQLCOM_SHOW_CHARSETS,
  SQLCOM_SHOW_COLLATIONS, SQLCOM_SHOW_CREATE_DB, SQLCOM_SHOW_TABLE_STATUS,
  SQLCOM_SHOW_TRIGGERS,

  SQLCOM_LOAD,SQLCOM_SET_OPTION,SQLCOM_LOCK_TABLES,SQLCOM_UNLOCK_TABLES,
  SQLCOM_GRANT,
  SQLCOM_CHANGE_DB, SQLCOM_CREATE_DB, SQLCOM_DROP_DB, SQLCOM_ALTER_DB,
  SQLCOM_REPAIR, SQLCOM_REPLACE, SQLCOM_REPLACE_SELECT,
  SQLCOM_CREATE_FUNCTION, SQLCOM_DROP_FUNCTION,
  SQLCOM_REVOKE,SQLCOM_OPTIMIZE, SQLCOM_CHECK,
  SQLCOM_ASSIGN_TO_KEYCACHE, SQLCOM_PRELOAD_KEYS,
  SQLCOM_FLUSH, SQLCOM_KILL, SQLCOM_ANALYZE,
  SQLCOM_ROLLBACK, SQLCOM_ROLLBACK_TO_SAVEPOINT,
  SQLCOM_COMMIT, SQLCOM_SAVEPOINT, SQLCOM_RELEASE_SAVEPOINT,
  SQLCOM_SLAVE_START, SQLCOM_SLAVE_STOP,
  SQLCOM_BEGIN, SQLCOM_CHANGE_MASTER,
  SQLCOM_RENAME_TABLE,  
  SQLCOM_RESET, SQLCOM_PURGE, SQLCOM_PURGE_BEFORE, SQLCOM_SHOW_BINLOGS,
  SQLCOM_SHOW_OPEN_TABLES,
  SQLCOM_HA_OPEN, SQLCOM_HA_CLOSE, SQLCOM_HA_READ,
  SQLCOM_SHOW_SLAVE_HOSTS, SQLCOM_DELETE_MULTI, SQLCOM_UPDATE_MULTI,
  SQLCOM_SHOW_BINLOG_EVENTS, SQLCOM_DO,
  SQLCOM_SHOW_WARNS, SQLCOM_EMPTY_QUERY, SQLCOM_SHOW_ERRORS,
  SQLCOM_SHOW_STORAGE_ENGINES, SQLCOM_SHOW_PRIVILEGES,
  SQLCOM_HELP, SQLCOM_CREATE_USER, SQLCOM_DROP_USER, SQLCOM_RENAME_USER,
  SQLCOM_REVOKE_ALL, SQLCOM_CHECKSUM,
  SQLCOM_CREATE_PROCEDURE, SQLCOM_CREATE_SPFUNCTION, SQLCOM_CALL,
  SQLCOM_DROP_PROCEDURE, SQLCOM_ALTER_PROCEDURE,SQLCOM_ALTER_FUNCTION,
  SQLCOM_SHOW_CREATE_PROC, SQLCOM_SHOW_CREATE_FUNC,
  SQLCOM_SHOW_STATUS_PROC, SQLCOM_SHOW_STATUS_FUNC,
  SQLCOM_PREPARE, SQLCOM_EXECUTE, SQLCOM_DEALLOCATE_PREPARE,
  SQLCOM_CREATE_VIEW, SQLCOM_DROP_VIEW,
  SQLCOM_CREATE_TRIGGER, SQLCOM_DROP_TRIGGER,
  SQLCOM_XA_START, SQLCOM_XA_END, SQLCOM_XA_PREPARE,
  SQLCOM_XA_COMMIT, SQLCOM_XA_ROLLBACK, SQLCOM_XA_RECOVER,
  SQLCOM_SHOW_PROC_CODE, SQLCOM_SHOW_FUNC_CODE,
  SQLCOM_INSTALL_PLUGIN, SQLCOM_UNINSTALL_PLUGIN,
  SQLCOM_SHOW_AUTHORS, SQLCOM_BINLOG_BASE64_EVENT,
  SQLCOM_SHOW_PLUGINS, SQLCOM_SHOW_CONTRIBUTORS,
  SQLCOM_CREATE_SERVER, SQLCOM_DROP_SERVER, SQLCOM_ALTER_SERVER,
  SQLCOM_CREATE_EVENT, SQLCOM_ALTER_EVENT, SQLCOM_DROP_EVENT,
  SQLCOM_SHOW_CREATE_EVENT, SQLCOM_SHOW_EVENTS,
  SQLCOM_SHOW_CREATE_TRIGGER,
  SQLCOM_ALTER_DB_UPGRADE,
  SQLCOM_SHOW_PROFILE, SQLCOM_SHOW_PROFILES,
  SQLCOM_SIGNAL, SQLCOM_RESIGNAL,
  SQLCOM_SHOW_RELAYLOG_EVENTS,
  SQLCOM_GET_DIAGNOSTICS,
  SQLCOM_SLAVE_ALL_START, SQLCOM_SLAVE_ALL_STOP,
  SQLCOM_SHOW_EXPLAIN,
  SQLCOM_SHOW_ANALYZE, SQLCOM_SHUTDOWN,
  SQLCOM_CREATE_ROLE, SQLCOM_DROP_ROLE, SQLCOM_GRANT_ROLE, SQLCOM_REVOKE_ROLE,
  SQLCOM_COMPOUND,
  SQLCOM_SHOW_GENERIC,
  SQLCOM_ALTER_USER,
  SQLCOM_SHOW_CREATE_USER,
  SQLCOM_EXECUTE_IMMEDIATE,
  SQLCOM_CREATE_SEQUENCE,
  SQLCOM_DROP_SEQUENCE,
  SQLCOM_ALTER_SEQUENCE,
  SQLCOM_CREATE_PACKAGE,
  SQLCOM_DROP_PACKAGE,
  SQLCOM_CREATE_PACKAGE_BODY,
  SQLCOM_DROP_PACKAGE_BODY,
  SQLCOM_SHOW_CREATE_PACKAGE,
  SQLCOM_SHOW_CREATE_PACKAGE_BODY,
  SQLCOM_SHOW_STATUS_PACKAGE,
  SQLCOM_SHOW_STATUS_PACKAGE_BODY,
  SQLCOM_SHOW_PACKAGE_BODY_CODE,
  SQLCOM_BACKUP, SQLCOM_BACKUP_LOCK,

  /*
    When a command is added here, be sure it's also added in mysqld.cc
    in "struct show_var_st com_status_vars[]= {" ...
  */
  /* This should be the last !!! */
  SQLCOM_END
};

struct TABLE_LIST;

class Storage_engine_name
{
protected:
  LEX_CSTRING m_storage_engine_name;
public:
  Storage_engine_name()
  {
    m_storage_engine_name.str= NULL;
    m_storage_engine_name.length= 0;
  }
  Storage_engine_name(const LEX_CSTRING &name)
   :m_storage_engine_name(name)
  { }
  Storage_engine_name(const LEX_STRING &name)
  {
    m_storage_engine_name.str= name.str;
    m_storage_engine_name.length= name.length;
  }
  bool resolve_storage_engine_with_error(THD *thd,
                                         handlerton **ha,
                                         bool tmp_table);
  bool is_set() { return m_storage_engine_name.str != NULL; }
};


class Prepared_statement;

/**
  @class Sql_cmd - Representation of an SQL command.

  This class is an interface between the parser and the runtime.
  The parser builds the appropriate derived classes of Sql_cmd
  to represent a SQL statement in the parsed tree.
  The execute() method in the derived classes of Sql_cmd contain the runtime
  implementation.
  Note that this interface is used for SQL statements recently implemented,
  the code for older statements tend to load the LEX structure with more
  attributes instead.
  Implement new statements by sub-classing Sql_cmd, as this improves
  code modularity (see the 'big switch' in dispatch_command()), and decreases
  the total size of the LEX structure (therefore saving memory in stored
  programs).
  The recommended name of a derived class of Sql_cmd is Sql_cmd_<derived>.

  Notice that the Sql_cmd class should not be confused with the
  Statement class.  Statement is a class that is used to manage an SQL
  command or a set of SQL commands. When the SQL statement text is
  analyzed, the parser will create one or more Sql_cmd objects to
  represent the actual SQL commands.
*/
class Sql_cmd : public Sql_alloc
{
private:
  Sql_cmd(const Sql_cmd &);         // No copy constructor wanted
  void operator=(Sql_cmd &);        // No assignment operator wanted

public:
  /**
    @brief Return the command code for this statement
  */
  virtual enum_sql_command sql_command_code() const = 0;

  /**
    @brief Check whether the statement has been prepared
    @returns true if this statement is prepared, false otherwise
  */
  bool is_prepared() const { return m_prepared; }

  /**
    @brief Prepare this SQL statement
    @param thd global context the processed statement
    @returns false if success, true if error
  */
  virtual bool prepare(THD *thd)
  {
    /* Default behavior for a statement is to have no preparation code. */
    DBUG_ASSERT(!is_prepared());
    set_prepared();
    return false;
  }

  /**
    @brief Execute this SQL statement
    @param thd global context the processed statement
    @returns false if success, true if error
  */
  virtual bool execute(THD *thd) = 0;

  virtual Storage_engine_name *option_storage_engine_name()
  {
    return NULL;
  }

  /**
    @brief Set the owning prepared statement
  */
  void set_owner(Prepared_statement *stmt) { m_owner = stmt; }

  /**
    @breaf Get the owning prepared statement
  */
  Prepared_statement *get_owner() { return m_owner; }

  /**
    @brief Check whether this command is a DML statement
    @return true if SQL command is a DML statement, false otherwise
  */
  virtual bool is_dml() const { return false; }

  /**
    @brief Unprepare prepared statement for the command
    @param thd global context of the processed statement

    @notes
    Temporary function used to "unprepare" a prepared statement after
    preparation, so that a subsequent execute statement will reprepare it.
    This is done because UNIT::cleanup() will un-resolve all resolved QBs.
  */
  virtual void unprepare(THD *thd)
  {
    DBUG_ASSERT(is_prepared());
    m_prepared = false;
  }

protected:
 Sql_cmd() :  m_prepared(false), m_owner(nullptr)
  {}

  virtual ~Sql_cmd()
  {
    /*
      Sql_cmd objects are allocated in thd->mem_root.
      In MySQL, the C++ destructor is never called, the underlying MEM_ROOT is
      simply destroyed instead.
      Do not rely on the destructor for any cleanup.
    */
    DBUG_ASSERT(false);
  }

  /**
    @brief Set this statement as prepared
  */
  void set_prepared() { m_prepared = true; }

 private:
  /* True when statement has been prepared */
  bool m_prepared;
  /* Owning prepared statement, nullptr if not prepared */
  Prepared_statement *m_owner;

};

struct LEX;
class select_result;
class Prelocking_strategy;
class DML_prelocking_strategy;
class Protocol;

/**
  @class Sql_cmd_dml - derivative abstract class used for DML statements

  This class is a class derived from Sql_cmd used when processing such
  data manipulation commands as SELECT, INSERT, UPDATE, DELETE and others
  that operate over some tables.
  After the parser phase all these commands are supposed to be processed
  by the same schema:
    - precheck of the access rights is performed for the used tables
    - the used tables are opened
    - context analysis phase is performed for the statement
    - the used tables are locked
    - the statement is optimized and executed
    - clean-up is performed for the statement.
  This schema is reflected in the function Sql_cmd_dml::execute() that
  uses Sql_cmd_dml::prepare is the statement has not been prepared yet.
  Precheck of the access right, context analysis are specific for statements
  of a certain type. That's why the methods implementing this operations are
  declared as abstract in this class.

  @note
  Currently this class is used only for UPDATE and DELETE commands.
*/
class Sql_cmd_dml : public Sql_cmd
{
public:

  /**
    @brief Check whether the statement changes the contents of used tables
    @return true if this is data change statement, false otherwise
  */
  virtual bool is_data_change_stmt() const { return true; }

  /**
    @brief Perform context analysis of the statement
    @param thd  global context the processed statement
    @returns false on success, true on error
  */
  virtual bool prepare(THD *thd);

  /**
    Execute the processed statement once
    @param thd  global context the processed statement
    @returns false on success, true on error
  */
  virtual bool execute(THD *thd);

  virtual bool is_dml() const { return true; }

  select_result *get_result() { return result; }

protected:
  Sql_cmd_dml()
      : Sql_cmd(), lex(nullptr), result(nullptr),
        m_empty_query(false)
  {}

  /**
    @brief Check whether query is guaranteed to return no data
    @return true if query is guaranteed to return no data, false otherwise

    @todo Also check this for the following cases:
          - Empty source for multi-table UPDATE and DELETE.
          - Check empty query expression for INSERT
  */
  bool is_empty_query() const
  {
    DBUG_ASSERT(is_prepared());
    return m_empty_query;
  }

  /**
    @brief Set statement as returning no data
  */
  void set_empty_query() { m_empty_query = true; }

  /**
    @brief Perform precheck of table privileges for the specific command
    @param thd  global context the processed statement
    @returns false if success, true if false

    @details
    Check that user has some relevant privileges for all tables involved in
    the statement, e.g. SELECT privileges for tables selected from, INSERT
    privileges for tables inserted into, etc. This function will also populate
    TABLE_LIST::grant with all privileges the user has for each table, which
    is later used during checking of column privileges.
    Note that at preparation time, views are not expanded yet. Privilege
    checking is thus rudimentary and must be complemented with later calls to
    SELECT_LEX::check_view_privileges().
    The reason to call this function at such an early stage is to be able to
    quickly reject statements for which the user obviously has insufficient
    privileges.
  */
  virtual bool precheck(THD *thd) = 0;

  /**
    @brief Perform the command-specific actions of the context analysis
    @param thd  global context the processed statement
    @returns false if success, true if error

    @note
    This function is called from prepare()
  */
  virtual bool prepare_inner(THD *thd) = 0;

  /**
    @brief Perform the command-specific actions of optimization and excution
    @param thd  global context the processed statement
    @returns false on success, true on error
  */
  virtual bool execute_inner(THD *thd);

  virtual DML_prelocking_strategy *get_dml_prelocking_strategy() = 0;

  uint table_count;

 protected:
  LEX *lex;              /**< Pointer to LEX for this statement */
  select_result *result; /**< Pointer to object for handling of the result */
  bool m_empty_query;    /**< True if query will produce no rows */
};


class Sql_cmd_show_slave_status: public Sql_cmd
{
protected:
  bool show_all_slaves_status;
public:
  Sql_cmd_show_slave_status()
    :show_all_slaves_status(false)
  {}

  Sql_cmd_show_slave_status(bool status_all)
    :show_all_slaves_status(status_all)
  {}

  enum_sql_command sql_command_code() const { return SQLCOM_SHOW_SLAVE_STAT; }

  bool execute(THD *thd);
  bool is_show_all_slaves_stat() { return show_all_slaves_status; }
};


class Sql_cmd_create_table_like: public Sql_cmd,
                                 public Storage_engine_name
{
public:
  Storage_engine_name *option_storage_engine_name() { return this; }
  bool execute(THD *thd);
};

class Sql_cmd_create_table: public Sql_cmd_create_table_like
{
public:
  enum_sql_command sql_command_code() const { return SQLCOM_CREATE_TABLE; }
};

class Sql_cmd_create_sequence: public Sql_cmd_create_table_like
{
public:
  enum_sql_command sql_command_code() const { return SQLCOM_CREATE_SEQUENCE; }
};


/**
  Sql_cmd_call represents the CALL statement.
*/
class Sql_cmd_call : public Sql_cmd
{
public:
  class sp_name *m_name;
  const class Sp_handler *m_handler;
  Sql_cmd_call(class sp_name *name, const class Sp_handler *handler)
   :m_name(name),
    m_handler(handler)
  {}

  virtual ~Sql_cmd_call() = default;

  /**
    Execute a CALL statement at runtime.
    @param thd the current thread.
    @return false on success.
  */
  bool execute(THD *thd);

  virtual enum_sql_command sql_command_code() const
  {
    return SQLCOM_CALL;
  }
};

#endif // SQL_CMD_INCLUDED

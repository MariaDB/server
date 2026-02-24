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

#include "sql_command.h"

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
  const LEX_CSTRING *name() const { return &m_storage_engine_name; }
};


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
    Execute this SQL statement.
    @param thd the current thread.
    @retval false on success.
    @retval true on error
  */
  virtual bool execute(THD *thd) = 0;

  virtual Storage_engine_name *option_storage_engine_name()
  {
    return NULL;
  }

protected:
  Sql_cmd() = default;

  virtual ~Sql_cmd()
  {
    /*
      Sql_cmd objects are allocated in thd->mem_root.
      In MySQL, the C++ destructor is never called, the underlying MEM_ROOT is
      simply destroyed instead.
      Do not rely on the destructor for any cleanup.
    */
    DBUG_ASSERT(FALSE);
  }
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

  enum_sql_command sql_command_code() const override { return SQLCOM_SHOW_SLAVE_STAT; }

  bool execute(THD *thd) override;
  bool is_show_all_slaves_stat() { return show_all_slaves_status; }
};


class Sql_cmd_create_table_like: public Sql_cmd,
                                 public Storage_engine_name
{
public:
  Storage_engine_name *option_storage_engine_name() override { return this; }
  bool execute(THD *thd) override;
};

class Sql_cmd_create_table: public Sql_cmd_create_table_like
{
public:
  enum_sql_command sql_command_code() const override { return SQLCOM_CREATE_TABLE; }
};

class Sql_cmd_create_sequence: public Sql_cmd_create_table_like
{
public:
  enum_sql_command sql_command_code() const override { return SQLCOM_CREATE_SEQUENCE; }
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
  bool execute(THD *thd) override;

  enum_sql_command sql_command_code() const override
  {
    return SQLCOM_CALL;
  }
};


#ifndef DBUG_OFF
/**
  Sql_cmd_call represents the SHOW CODE statement:
  - SHOW PROCEDURE CODE
  - SHOW FUNCTION CODE
  - SHOW PACKAGE BODY CODE
*/
class Sql_cmd_show_routine_code : public Sql_cmd
{
public:
  class sp_name *m_name;
  const class Sp_handler *m_handler;
  enum_sql_command m_sql_command;
  Sql_cmd_show_routine_code(class sp_name *name,
                            const class Sp_handler *handler,
                            enum_sql_command sql_command)
   :m_name(name),
    m_handler(handler),
    m_sql_command(sql_command)
  {}

  virtual ~Sql_cmd_show_routine_code() = default;

  bool execute(THD *thd) override;

  enum_sql_command sql_command_code() const override
  {
    return m_sql_command;
  }
};
#endif // DBUG_OFF


#endif // SQL_CMD_INCLUDED

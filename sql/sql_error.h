/* Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2017, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef SQL_ERROR_H
#define SQL_ERROR_H

#include "sql_list.h" 	/* Sql_alloc, MEM_ROOT, list */
#include "sql_type_int.h" // Longlong_hybrid
#include "sql_string.h"                        /* String */
#include "sql_plist.h" /* I_P_List */
#include "mysql_com.h" /* MYSQL_ERRMSG_SIZE */
#include "my_time.h"   /* MYSQL_TIME */
#include "decimal.h"

class THD;
class my_decimal;
class sp_condition_value;

///////////////////////////////////////////////////////////////////////////

class Sql_state
{
protected:
  /**
    This member is always NUL terminated.
  */
  char m_sqlstate[SQLSTATE_LENGTH + 1];
public:
  Sql_state()
  {
    memset(m_sqlstate, 0, sizeof(m_sqlstate));
  }

  Sql_state(const char *sqlstate)
  {
    set_sqlstate(sqlstate);
  }

  const char* get_sqlstate() const
  { return m_sqlstate; }

  void set_sqlstate(const Sql_state *other)
  {
    *this= *other;
  }
  void set_sqlstate(const char *sqlstate)
  {
    memcpy(m_sqlstate, sqlstate, SQLSTATE_LENGTH);
    m_sqlstate[SQLSTATE_LENGTH]= '\0';
  }
  bool eq(const Sql_state *other) const
  {
    return strcmp(m_sqlstate, other->m_sqlstate) == 0;
  }

  bool has_sql_state() const { return m_sqlstate[0] != '\0'; }

  /**
    Checks if this SQL state defines a WARNING condition.
    Note: m_sqlstate must contain a valid SQL-state.

    @retval true if this SQL state defines a WARNING condition.
    @retval false otherwise.
  */
  inline bool is_warning() const
  { return m_sqlstate[0] == '0' && m_sqlstate[1] == '1'; }


  /**
    Checks if this SQL state defines a NOT FOUND condition.
    Note: m_sqlstate must contain a valid SQL-state.

    @retval true if this SQL state defines a NOT FOUND condition.
    @retval false otherwise.
  */
  inline bool is_not_found() const
  { return m_sqlstate[0] == '0' && m_sqlstate[1] == '2'; }


  /**
    Checks if this SQL state defines an EXCEPTION condition.
    Note: m_sqlstate must contain a valid SQL-state.

    @retval true if this SQL state defines an EXCEPTION condition.
    @retval false otherwise.
  */
  inline bool is_exception() const
  { return m_sqlstate[0] != '0' || m_sqlstate[1] > '2'; }

};


class Sql_state_errno: public Sql_state
{
protected:
  /**
    MySQL extension, MYSQL_ERRNO condition item.
    SQL error number. One of ER_ codes from share/errmsg.txt.
    Set by set_error_status.
  */
  uint m_sql_errno;

public:
  Sql_state_errno()
   :m_sql_errno(0)
  { }
  Sql_state_errno(uint sql_errno)
   :m_sql_errno(sql_errno)
  { }
  Sql_state_errno(uint sql_errno, const char *sql_state)
   :Sql_state(sql_state),
    m_sql_errno(sql_errno)
  { }
  /**
    Get the SQL_ERRNO of this condition.
    @return the sql error number condition item.
  */
  uint get_sql_errno() const
  { return m_sql_errno; }

  void set(uint sql_errno, const char *sqlstate)
  {
    m_sql_errno= sql_errno;
    set_sqlstate(sqlstate);
  }
  void clear()
  {
    m_sql_errno= 0;
  }
};


class Sql_state_errno_level: public Sql_state_errno
{
public:
  /*
    Enumeration value describing the severity of the error.

    Note that these enumeration values must correspond to the indices
    of the sql_print_message_handlers array.
  */
  enum enum_warning_level
  { WARN_LEVEL_NOTE, WARN_LEVEL_WARN, WARN_LEVEL_ERROR, WARN_LEVEL_END};

protected:
  /** Severity (error, warning, note) of this condition. */
  enum_warning_level m_level;

  void assign_defaults(const Sql_state_errno *value);

public:
  /**
    Get the error level of this condition.
    @return the error level condition item.
  */
  enum_warning_level get_level() const
  { return m_level; }

   Sql_state_errno_level()
    :m_level(WARN_LEVEL_ERROR)
  { }

  Sql_state_errno_level(uint sqlerrno, const char* sqlstate,
                         enum_warning_level level)
   :Sql_state_errno(sqlerrno, sqlstate),
    m_level(level)
  { }
  Sql_state_errno_level(const Sql_state_errno &state_errno,
                         enum_warning_level level)
   :Sql_state_errno(state_errno),
    m_level(level)
  { }
  void clear()
  {
    m_level= WARN_LEVEL_ERROR;
    Sql_state_errno::clear();
  }
};


/*
  class Sql_user_condition_identity.
  Instances of this class uniquely idetify user defined conditions (EXCEPTION).

    SET sql_mode=ORACLE;
    CREATE PROCEDURE p1
    AS
      a EXCEPTION;
    BEGIN
      RAISE a;
    EXCEPTION
      WHEN a THEN NULL;
    END;

  Currently a user defined condition is identified by a pointer to
  its parse time sp_condition_value instance. This can change when
  we add packages. See MDEV-10591.
*/
class Sql_user_condition_identity
{
protected:
  const sp_condition_value *m_user_condition_value;
public:
  Sql_user_condition_identity()
   :m_user_condition_value(NULL)
  { }
  Sql_user_condition_identity(const sp_condition_value *value)
   :m_user_condition_value(value)
  { }
  const sp_condition_value *get_user_condition_value() const
  { return m_user_condition_value; }

  void set(const Sql_user_condition_identity &identity)
  {
    *this= identity;
  }
  void clear()
  {
    m_user_condition_value= NULL;
  }
};


/**
  class Sql_condition_identity.
  Instances of this class uniquely identify conditions
  (including user-defined exceptions for sql_mode=ORACLE)
  and store everything that is needed for handler search
  purposes in sp_pcontext::find_handler().
*/
class Sql_condition_identity: public Sql_state_errno_level,
                              public Sql_user_condition_identity
{
public:
  Sql_condition_identity()
  { }
  Sql_condition_identity(const Sql_state_errno_level &st,
                         const Sql_user_condition_identity &ucid)
   :Sql_state_errno_level(st),
    Sql_user_condition_identity(ucid)
  { }
  Sql_condition_identity(const Sql_state_errno &st,
                         enum_warning_level level,
                         const Sql_user_condition_identity &ucid)
   :Sql_state_errno_level(st, level),
    Sql_user_condition_identity(ucid)
  { }
  Sql_condition_identity(uint sqlerrno,
                         const char* sqlstate,
                         enum_warning_level level,
                         const Sql_user_condition_identity &ucid)
    :Sql_state_errno_level(sqlerrno, sqlstate, level),
     Sql_user_condition_identity(ucid)
  { }
  void clear()
  {
    Sql_state_errno_level::clear();
    Sql_user_condition_identity::clear();
  }
};


class Sql_condition_items
{
protected:
  /** SQL CLASS_ORIGIN condition item. */
  String m_class_origin;

  /** SQL SUBCLASS_ORIGIN condition item. */
  String m_subclass_origin;

  /** SQL CONSTRAINT_CATALOG condition item. */
  String m_constraint_catalog;

  /** SQL CONSTRAINT_SCHEMA condition item. */
  String m_constraint_schema;

  /** SQL CONSTRAINT_NAME condition item. */
  String m_constraint_name;

  /** SQL CATALOG_NAME condition item. */
  String m_catalog_name;

  /** SQL SCHEMA_NAME condition item. */
  String m_schema_name;

  /** SQL TABLE_NAME condition item. */
  String m_table_name;

  /** SQL COLUMN_NAME condition item. */
  String m_column_name;

  /** SQL CURSOR_NAME condition item. */
  String m_cursor_name;

  Sql_condition_items()
   :m_class_origin((const char*) NULL, 0, & my_charset_utf8mb3_bin),
    m_subclass_origin((const char*) NULL, 0, & my_charset_utf8mb3_bin),
    m_constraint_catalog((const char*) NULL, 0, & my_charset_utf8mb3_bin),
    m_constraint_schema((const char*) NULL, 0, & my_charset_utf8mb3_bin),
    m_constraint_name((const char*) NULL, 0, & my_charset_utf8mb3_bin),
    m_catalog_name((const char*) NULL, 0, & my_charset_utf8mb3_bin),
    m_schema_name((const char*) NULL, 0, & my_charset_utf8mb3_bin),
    m_table_name((const char*) NULL, 0, & my_charset_utf8mb3_bin),
    m_column_name((const char*) NULL, 0, & my_charset_utf8mb3_bin),
    m_cursor_name((const char*) NULL, 0, & my_charset_utf8mb3_bin)
  { }

  void clear()
  {
    m_class_origin.length(0);
    m_subclass_origin.length(0);
    m_constraint_catalog.length(0);
    m_constraint_schema.length(0);
    m_constraint_name.length(0);
    m_catalog_name.length(0);
    m_schema_name.length(0);
    m_table_name.length(0);
    m_column_name.length(0);
    m_cursor_name.length(0);
  }
};


/**
  Representation of a SQL condition.
  A SQL condition can be a completion condition (note, warning),
  or an exception condition (error, not found).
*/
class Sql_condition : public Sql_alloc,
                      public Sql_condition_identity,
                      public Sql_condition_items
{
public:

  /**
    Convert a bitmask consisting of MYSQL_TIME_{NOTE|WARN}_XXX bits
    to WARN_LEVEL_XXX
  */
  static enum_warning_level time_warn_level(uint warnings)
  {
    return MYSQL_TIME_WARN_HAVE_WARNINGS(warnings) ?
           WARN_LEVEL_WARN : WARN_LEVEL_NOTE;
  }

  /**
    Get the MESSAGE_TEXT of this condition.
    @return the message text.
  */
  const char* get_message_text() const;

  /**
    Get the MESSAGE_OCTET_LENGTH of this condition.
    @return the length in bytes of the message text.
  */
  int get_message_octet_length() const;

private:
  /*
    The interface of Sql_condition is mostly private, by design,
    so that only the following code:
    - various raise_error() or raise_warning() methods in class THD,
    - the implementation of SIGNAL / RESIGNAL / GET DIAGNOSTICS
    - catch / re-throw of SQL conditions in stored procedures (sp_rcontext)
    is allowed to create / modify a SQL condition.
    Enforcing this policy prevents confusion, since the only public
    interface available to the rest of the server implementation
    is the interface offered by the THD methods (THD::raise_error()),
    which should be used.
  */
  friend class THD;
  friend class Warning_info;
  friend class Sql_cmd_common_signal;
  friend class Sql_cmd_signal;
  friend class Sql_cmd_resignal;
  friend class sp_rcontext;
  friend class Condition_information_item;

  /**
    Default constructor.
    This constructor is usefull when allocating arrays.
    Note that the init() method should be called to complete the Sql_condition.
  */
  Sql_condition()
   :m_mem_root(NULL)
  { }

  /**
    Complete the Sql_condition initialisation.
    @param mem_root The memory root to use for the condition items
    of this condition
  */
  void init(MEM_ROOT *mem_root)
  {
    DBUG_ASSERT(mem_root != NULL);
    DBUG_ASSERT(m_mem_root == NULL);
    m_mem_root= mem_root;
  }

  /**
    Constructor.
    @param mem_root The memory root to use for the condition items
    of this condition
  */
  Sql_condition(MEM_ROOT *mem_root)
   :m_mem_root(mem_root)
  {
    DBUG_ASSERT(mem_root != NULL);
  }

  Sql_condition(MEM_ROOT *mem_root, const Sql_user_condition_identity &ucid)
   :Sql_condition_identity(Sql_state_errno_level(), ucid),
    m_mem_root(mem_root)
  {
    DBUG_ASSERT(mem_root != NULL);
  }
  /**
    Constructor for a fixed message text.
    @param mem_root - memory root
    @param value    - the error number and the sql state for this condition
    @param level    - the error level for this condition
    @param msg      - the message text for this condition
  */
  Sql_condition(MEM_ROOT *mem_root,
                const Sql_condition_identity &value,
                const char *msg)
   :Sql_condition_identity(value),
    m_mem_root(mem_root)
  {
    DBUG_ASSERT(mem_root != NULL);
    DBUG_ASSERT(value.get_sql_errno() != 0);
    DBUG_ASSERT(msg != NULL);
    set_builtin_message_text(msg);
  }

  /** Destructor. */
  ~Sql_condition()
  {}

  /**
    Copy optional condition items attributes.
    @param cond the condition to copy.
  */
  void copy_opt_attributes(const Sql_condition *cond);

  /**
    Set the condition message test.
    @param str Message text, expressed in the character set derived from
    the server --language option
  */
  void set_builtin_message_text(const char* str);

  /** Set the CLASS_ORIGIN of this condition. */
  void set_class_origin();

  /** Set the SUBCLASS_ORIGIN of this condition. */
  void set_subclass_origin();

  /**
    Assign the condition items 'MYSQL_ERRNO', 'level' and 'MESSAGE_TEXT'
    default values of a condition.
    @param thd   - current thread, to access to localized error messages
    @param from  - copy condition items from here (can be NULL)
  */
  void assign_defaults(THD *thd, const Sql_state_errno *from);

  /**
    Clear this SQL condition.
  */
  void clear()
  {
    Sql_condition_identity::clear();
    Sql_condition_items::clear();
    m_message_text.length(0);
  }

private:
  /** Message text, expressed in the character set implied by --language. */
  String m_message_text;

  /** Pointers for participating in the list of conditions. */
  Sql_condition *next_in_wi;
  Sql_condition **prev_in_wi;

  /** Memory root to use to hold condition item values. */
  MEM_ROOT *m_mem_root;
};

///////////////////////////////////////////////////////////////////////////

/**
  Information about warnings of the current connection.
*/
class Warning_info
{
  /** The type of the counted and doubly linked list of conditions. */
  typedef I_P_List<Sql_condition,
                   I_P_List_adapter<Sql_condition,
                                    &Sql_condition::next_in_wi,
                                    &Sql_condition::prev_in_wi>,
                   I_P_List_counter,
                   I_P_List_fast_push_back<Sql_condition> >
          Sql_condition_list;

  /** A memory root to allocate warnings and errors */
  MEM_ROOT           m_warn_root;

  /** List of warnings of all severities (levels). */
  Sql_condition_list   m_warn_list;

  /** A break down of the number of warnings per severity (level). */
  uint	             m_warn_count[(uint) Sql_condition::WARN_LEVEL_END];

  /**
    The number of warnings of the current statement. Warning_info
    life cycle differs from statement life cycle -- it may span
    multiple statements. In that case we get
    m_current_statement_warn_count 0, whereas m_warn_list is not empty.
  */
  uint	             m_current_statement_warn_count;

  /*
    Row counter, to print in errors and warnings. Not increased in
    create_sort_index(); may differ from examined_row_count.
  */
  ulong              m_current_row_for_warning;

  /** Used to optionally clear warnings only once per statement. */
  ulonglong          m_warn_id;

  /**
    A pointer to an element of m_warn_list. It determines SQL-condition
    instance which corresponds to the error state in Diagnostics_area.
  
    This is needed for properly processing SQL-conditions in SQL-handlers.
    When an SQL-handler is found for the current error state in Diagnostics_area,
    this pointer is needed to remove the corresponding SQL-condition from the
    Warning_info list.
  
    @note m_error_condition might be NULL in the following cases:
       - Diagnostics_area set to fatal error state (like OOM);
       - Max number of Warning_info elements has been reached (thus, there is
         no corresponding SQL-condition object in Warning_info).
  */
  const Sql_condition *m_error_condition;

  /** Indicates if push_warning() allows unlimited number of warnings. */
  bool               m_allow_unlimited_warnings;
  bool		     initialized;    /* Set to 1 if init() has been called */

  /** Read only status. */
  bool m_read_only;

  /** Pointers for participating in the stack of Warning_info objects. */
  Warning_info *m_next_in_da;
  Warning_info **m_prev_in_da;

  List<Sql_condition> m_marked_sql_conditions;

public:
  Warning_info(ulonglong warn_id_arg, bool allow_unlimited_warnings,
               bool initialized);
  ~Warning_info();
  /* Allocate memory for structures */
  void init();
  void free_memory();

private:
  Warning_info(const Warning_info &rhs); /* Not implemented */
  Warning_info& operator=(const Warning_info &rhs); /* Not implemented */

  /**
    Checks if Warning_info contains SQL-condition with the given message.

    @param message_str    Message string.
    @param message_length Length of message string.

    @return true if the Warning_info contains an SQL-condition with the given
    message.
  */
  bool has_sql_condition(const char *message_str, size_t message_length) const;

  /**
    Reset the warning information. Clear all warnings,
    the number of warnings, reset current row counter
    to point to the first row.

    @param new_id new Warning_info id.
  */
  void clear(ulonglong new_id);

  /**
    Only clear warning info if haven't yet done that already
    for the current query. Allows to be issued at any time
    during the query, without risk of clearing some warnings
    that have been generated by the current statement.

    @todo: This is a sign of sloppy coding. Instead we need to
    designate one place in a statement life cycle where we call
    Warning_info::clear().

    @param query_id Current query id.
  */
  void opt_clear(ulonglong query_id)
  {
    if (query_id != m_warn_id)
      clear(query_id);
  }

  /**
    Concatenate the list of warnings.

    It's considered tolerable to lose an SQL-condition in case of OOM-error,
    or if the number of SQL-conditions in the Warning_info reached top limit.

    @param thd    Thread context.
    @param source Warning_info object to copy SQL-conditions from.
  */
  void append_warning_info(THD *thd, const Warning_info *source);

  /**
    Reset between two COM_ commands. Warnings are preserved
    between commands, but statement_warn_count indicates
    the number of warnings of this particular statement only.
  */
  void reset_for_next_command()
  { m_current_statement_warn_count= 0; }

  /**
    Mark active SQL-conditions for later removal.
    This is done to simulate stacked DAs for HANDLER statements.
  */
  void mark_sql_conditions_for_removal();

  /**
    Unmark SQL-conditions, which were marked for later removal.
    This is done to simulate stacked DAs for HANDLER statements.
  */
  void unmark_sql_conditions_from_removal()
  { m_marked_sql_conditions.empty(); }

  /**
    Remove SQL-conditions that are marked for deletion.
    This is done to simulate stacked DAs for HANDLER statements.
  */
  void remove_marked_sql_conditions();

  /**
    Check if the given SQL-condition is marked for removal in this Warning_info
    instance.

    @param cond the SQL-condition.

    @retval true if the given SQL-condition is marked for removal in this
                 Warning_info instance.
    @retval false otherwise.
  */
  bool is_marked_for_removal(const Sql_condition *cond) const;

  /**
    Mark a single SQL-condition for removal (add the given SQL-condition to the
    removal list of this Warning_info instance).
  */
  void mark_condition_for_removal(Sql_condition *cond)
  { m_marked_sql_conditions.push_back(cond, &m_warn_root); }

  /**
    Used for @@warning_count system variable, which prints
    the number of rows returned by SHOW WARNINGS.
  */
  ulong warn_count() const
  {
    /*
      This may be higher than warn_list.elements() if we have
      had more warnings than thd->variables.max_error_count.
    */
    return (m_warn_count[(uint) Sql_condition::WARN_LEVEL_NOTE] +
            m_warn_count[(uint) Sql_condition::WARN_LEVEL_ERROR] +
            m_warn_count[(uint) Sql_condition::WARN_LEVEL_WARN]);
  }

  /**
    The number of errors, or number of rows returned by SHOW ERRORS,
    also the value of session variable @@error_count.
  */
  ulong error_count() const
  { return m_warn_count[(uint) Sql_condition::WARN_LEVEL_ERROR]; }

  /**
    The number of conditions (errors, warnings and notes) in the list.
  */
  uint cond_count() const
  {
    return m_warn_list.elements();
  }

  /** Id of the warning information area. */
  ulonglong id() const { return m_warn_id; }

  /** Set id of the warning information area. */
  void id(ulonglong id_arg) { m_warn_id= id_arg; }

  /** Do we have any errors and warnings that we can *show*? */
  bool is_empty() const { return m_warn_list.is_empty(); }

  /** Increment the current row counter to point at the next row. */
  void inc_current_row_for_warning() { m_current_row_for_warning++; }

  /** Reset the current row counter. Start counting from the first row. */
  void reset_current_row_for_warning() { m_current_row_for_warning= 1; }

  /** Return the current counter value. */
  ulong current_row_for_warning() const { return m_current_row_for_warning; }

  /** Return the number of warnings thrown by the current statement. */
  ulong current_statement_warn_count() const
  { return m_current_statement_warn_count; }

  /** Make sure there is room for the given number of conditions. */
  void reserve_space(THD *thd, uint count);

  /**
    Add a new SQL-condition to the current list and increment the respective
    counters.

    @param thd        Thread context.
    @param identity   SQL-condition identity
    @param msg        SQL-condition message.

    @return a pointer to the added SQL-condition.
  */
  Sql_condition *push_warning(THD *thd,
                              const Sql_condition_identity *identity,
                              const char* msg);

  /**
    Add a new SQL-condition to the current list and increment the respective
    counters.

    @param thd            Thread context.
    @param sql_condition  SQL-condition to copy values from.

    @return a pointer to the added SQL-condition.
  */
  Sql_condition *push_warning(THD *thd, const Sql_condition *sql_condition);

  /**
    Set the read only status for this statement area.
    This is a privileged operation, reserved for the implementation of
    diagnostics related statements, to enforce that the statement area is
    left untouched during execution.
    The diagnostics statements are:
    - SHOW WARNINGS
    - SHOW ERRORS
    - GET DIAGNOSTICS
    @param read_only the read only property to set.
  */
  void set_read_only(bool read_only_arg)
  { m_read_only= read_only_arg; }

  /**
    Read only status.
    @return the read only property.
  */
  bool is_read_only() const
  { return m_read_only; }

  /**
    @return SQL-condition, which corresponds to the error state in
    Diagnostics_area.

    @see m_error_condition.
  */
  const Sql_condition *get_error_condition() const
  { return m_error_condition; }

  /**
    Set SQL-condition, which corresponds to the error state in Diagnostics_area.

    @see m_error_condition.
  */
  void set_error_condition(const Sql_condition *error_condition)
  { m_error_condition= error_condition; }

  /**
    Reset SQL-condition, which corresponds to the error state in
    Diagnostics_area.

    @see m_error_condition.
  */
  void clear_error_condition()
  { m_error_condition= NULL; }

  // for:
  //   - m_next_in_da / m_prev_in_da
  //   - is_marked_for_removal()
  friend class Diagnostics_area;
};


extern size_t err_conv(char *buff, uint to_length, const char *from,
                       uint from_length, CHARSET_INFO *from_cs);

class ErrBuff
{
protected:
  mutable char err_buffer[MYSQL_ERRMSG_SIZE];
public:
  ErrBuff()
  {
    err_buffer[0]= '\0';
  }
  const char *ptr() const { return err_buffer; }
  LEX_CSTRING set_longlong(const Longlong_hybrid &nr) const
  {
    int radix= nr.is_unsigned() ? 10 : -10;
    const char *end= longlong10_to_str(nr.value(), err_buffer, radix);
    DBUG_ASSERT(end >= err_buffer);
    return {err_buffer, (size_t) (end - err_buffer)};
  }
  LEX_CSTRING set_double(double nr) const
  {
    size_t length= my_gcvt(nr, MY_GCVT_ARG_DOUBLE,
                           sizeof(err_buffer), err_buffer, 0);
    return {err_buffer, length};
  }
  LEX_CSTRING set_decimal(const decimal_t *d) const
  {
    int length= sizeof(err_buffer);
    decimal2string(d, err_buffer, &length, 0, 0, ' ');
    DBUG_ASSERT(length >= 0);
    return {err_buffer, (size_t) length};
  }
  LEX_CSTRING set_str(const char *str, size_t len, CHARSET_INFO *cs) const
  {
    DBUG_ASSERT(len < UINT_MAX32);
    len= err_conv(err_buffer, (uint) sizeof(err_buffer), str, (uint) len, cs);
    return {err_buffer, len};
  }
  LEX_CSTRING set_mysql_time(const MYSQL_TIME *ltime) const
  {
    int length= my_TIME_to_str(ltime, err_buffer, AUTO_SEC_PART_DIGITS);
    DBUG_ASSERT(length >= 0);
    return {err_buffer, (size_t) length};
  }
};


class ErrConv: public ErrBuff
{
public:
  ErrConv() {}
  virtual ~ErrConv() {}
  virtual LEX_CSTRING lex_cstring() const= 0;
  inline const char *ptr() const
  {
    return lex_cstring().str;
  }
};

class ErrConvString : public ErrConv
{
  const char *str;
  size_t len;
  CHARSET_INFO *cs;
public:
  ErrConvString(const char *str_arg, size_t len_arg, CHARSET_INFO *cs_arg)
    : ErrConv(), str(str_arg), len(len_arg), cs(cs_arg) {}
  ErrConvString(const char *str_arg, CHARSET_INFO *cs_arg)
    : ErrConv(), str(str_arg), len(strlen(str_arg)), cs(cs_arg) {}
  ErrConvString(const String *s)
    : ErrConv(), str(s->ptr()), len(s->length()), cs(s->charset()) {}
  LEX_CSTRING lex_cstring() const override
  {
    return set_str(str, len, cs);
  }
};

class ErrConvInteger : public ErrConv, public Longlong_hybrid
{
public:
  ErrConvInteger(const Longlong_hybrid &nr)
   : ErrConv(), Longlong_hybrid(nr) { }
  LEX_CSTRING lex_cstring() const override
  {
    return set_longlong(static_cast<Longlong_hybrid>(*this));
  }
};

class ErrConvDouble: public ErrConv
{
  double num;
public:
  ErrConvDouble(double num_arg) : ErrConv(), num(num_arg) {}
  LEX_CSTRING lex_cstring() const override
  {
    return set_double(num);
  }
};

class ErrConvTime : public ErrConv
{
  const MYSQL_TIME *ltime;
public:
  ErrConvTime(const MYSQL_TIME *ltime_arg) : ErrConv(), ltime(ltime_arg) {}
  LEX_CSTRING lex_cstring() const override
  {
    return set_mysql_time(ltime);
  }
};

class ErrConvDecimal : public ErrConv
{
  const decimal_t *d;
public:
  ErrConvDecimal(const decimal_t *d_arg) : ErrConv(), d(d_arg) {}
  LEX_CSTRING lex_cstring() const override
  {
    return set_decimal(d);
  }
};

///////////////////////////////////////////////////////////////////////////

/**
  Stores status of the currently executed statement.
  Cleared at the beginning of the statement, and then
  can hold either OK, ERROR, or EOF status.
  Can not be assigned twice per statement.
*/

class Diagnostics_area: public Sql_state_errno,
                        public Sql_user_condition_identity
{
private:
  /** The type of the counted and doubly linked list of conditions. */
  typedef I_P_List<Warning_info,
                   I_P_List_adapter<Warning_info,
                                    &Warning_info::m_next_in_da,
                                    &Warning_info::m_prev_in_da>,
                   I_P_List_counter,
                   I_P_List_fast_push_back<Warning_info> >
          Warning_info_list;

public:
  /** Const iterator used to iterate through the warning list. */
  typedef Warning_info::Sql_condition_list::Const_Iterator
    Sql_condition_iterator;

  enum enum_diagnostics_status
  {
    /** The area is cleared at start of a statement. */
    DA_EMPTY= 0,
    /** Set whenever one calls my_ok(). */
    DA_OK,
    /** Set whenever one calls my_eof(). */
    DA_EOF,
    /** Set whenever one calls my_ok() in PS bulk mode. */
    DA_OK_BULK,
    /** Set whenever one calls my_eof() in PS bulk mode. */
    DA_EOF_BULK,
    /** Set whenever one calls my_error() or my_message(). */
    DA_ERROR,
    /** Set in case of a custom response, such as one from COM_STMT_PREPARE. */
    DA_DISABLED
  };

  void set_overwrite_status(bool can_overwrite_status)
  { m_can_overwrite_status= can_overwrite_status; }

  /** True if status information is sent to the client. */
  bool is_sent() const { return m_is_sent; }

  void set_is_sent(bool is_sent_arg) { m_is_sent= is_sent_arg; }

  void set_ok_status(ulonglong affected_rows,
                     ulonglong last_insert_id,
                     const char *message);

  void set_eof_status(THD *thd);

  void set_error_status(uint sql_errno);

  void set_error_status(uint sql_errno,
                        const char *message,
                        const char *sqlstate,
                        const Sql_user_condition_identity &ucid,
                        const Sql_condition *error_condition);

  void set_error_status(uint sql_errno,
                        const char *message,
                        const char *sqlstate,
                        const Sql_condition *error_condition)
  {
    set_error_status(sql_errno, message, sqlstate,
                     Sql_user_condition_identity(),
                     error_condition);
  }

  void disable_status();

  void reset_diagnostics_area();

  bool is_set() const { return m_status != DA_EMPTY; }

  bool is_error() const { return m_status == DA_ERROR; }

  bool is_eof() const { return m_status == DA_EOF; }

  bool is_ok() const { return m_status == DA_OK; }

  bool is_disabled() const { return m_status == DA_DISABLED; }

  void set_bulk_execution(bool bulk) { is_bulk_execution= bulk; }

  bool is_bulk_op() const { return is_bulk_execution; }

  enum_diagnostics_status status() const { return m_status; }

  const char *message() const
  {
    DBUG_ASSERT(m_status == DA_ERROR || m_status == DA_OK ||
                m_status == DA_OK_BULK || m_status == DA_EOF_BULK);
    return m_message;
  }


  uint sql_errno() const
  {
    DBUG_ASSERT(m_status == DA_ERROR);
    return Sql_state_errno::get_sql_errno();
  }

  const char* get_sqlstate() const
  { DBUG_ASSERT(m_status == DA_ERROR); return Sql_state::get_sqlstate(); }

  ulonglong affected_rows() const
  {
    DBUG_ASSERT(m_status == DA_OK || m_status == DA_OK_BULK);
    return m_affected_rows;
  }

  ulonglong last_insert_id() const
  {
    DBUG_ASSERT(m_status == DA_OK || m_status == DA_OK_BULK);
    return m_last_insert_id;
  }

  uint statement_warn_count() const
  {
    DBUG_ASSERT(m_status == DA_OK || m_status == DA_OK_BULK ||
                m_status == DA_EOF ||m_status == DA_EOF_BULK );
    return m_statement_warn_count;
  }

  /**
    Get the current errno, state and id of the user defined condition
    and return them as Sql_condition_identity.
  */
  Sql_condition_identity get_error_condition_identity() const
  {
    DBUG_ASSERT(m_status == DA_ERROR);
    return Sql_condition_identity(*this /*Sql_state_errno*/,
                                  Sql_condition::WARN_LEVEL_ERROR,
                                  *this /*Sql_user_condition_identity*/);
  }

  /* Used to count any warnings pushed after calling set_ok_status(). */
  void increment_warning()
  {
    if (m_status != DA_EMPTY)
      m_statement_warn_count++;
  }

  Diagnostics_area(bool initialize);
  Diagnostics_area(ulonglong warning_info_id, bool allow_unlimited_warnings,
                   bool initialize);
  void init() { m_main_wi.init() ; }
  void free_memory() { m_main_wi.free_memory() ; }

  void push_warning_info(Warning_info *wi)
  { m_wi_stack.push_front(wi); }

  void pop_warning_info()
  {
    DBUG_ASSERT(m_wi_stack.elements() > 0);
    m_wi_stack.remove(m_wi_stack.front());
  }

  void set_warning_info_id(ulonglong id)
  { get_warning_info()->id(id); }

  ulonglong warning_info_id() const
  { return get_warning_info()->id(); }

  /**
    Compare given current warning info and current warning info
    and see if they are different. They will be different if
    warnings have been generated or statements that use tables
    have been executed. This is checked by comparing m_warn_id.

    @param wi  Warning info to compare with current Warning info.

    @return    false if they are equal, true if they are not.
  */
  bool warning_info_changed(const Warning_info *wi) const
  { return get_warning_info()->id() != wi->id(); }

  bool is_warning_info_empty() const
  { return get_warning_info()->is_empty(); }

  ulong current_statement_warn_count() const
  { return get_warning_info()->current_statement_warn_count(); }

  bool has_sql_condition(const char *message_str, size_t message_length) const
  { return get_warning_info()->has_sql_condition(message_str, message_length); }

  void reset_for_next_command()
  { get_warning_info()->reset_for_next_command(); }

  void clear_warning_info(ulonglong id)
  { get_warning_info()->clear(id); }

  void opt_clear_warning_info(ulonglong query_id)
  { get_warning_info()->opt_clear(query_id); }

  ulong current_row_for_warning() const
  { return get_warning_info()->current_row_for_warning(); }

  void inc_current_row_for_warning()
  { get_warning_info()->inc_current_row_for_warning(); }

  void reset_current_row_for_warning()
  { get_warning_info()->reset_current_row_for_warning(); }

  bool is_warning_info_read_only() const
  { return get_warning_info()->is_read_only(); }

  void set_warning_info_read_only(bool read_only_arg)
  { get_warning_info()->set_read_only(read_only_arg); }

  ulong error_count() const
  { return get_warning_info()->error_count(); }

  ulong warn_count() const
  { return get_warning_info()->warn_count(); }

  uint cond_count() const
  { return get_warning_info()->cond_count(); }

  Sql_condition_iterator sql_conditions() const
  { return get_warning_info()->m_warn_list; }

  void reserve_space(THD *thd, uint count)
  { get_warning_info()->reserve_space(thd, count); }

  Sql_condition *push_warning(THD *thd, const Sql_condition *sql_condition)
  { return get_warning_info()->push_warning(thd, sql_condition); }

  Sql_condition *push_warning(THD *thd,
                              uint sql_errno_arg,
                              const char* sqlstate,
                              Sql_condition::enum_warning_level level,
                              const Sql_user_condition_identity &ucid,
                              const char* msg)
  {
    Sql_condition_identity tmp(sql_errno_arg, sqlstate, level, ucid);
    return get_warning_info()->push_warning(thd, &tmp, msg);
  }

  Sql_condition *push_warning(THD *thd,
                              uint sqlerrno,
                              const char* sqlstate,
                              Sql_condition::enum_warning_level level,
                              const char* msg)
  {
    return push_warning(thd, sqlerrno, sqlstate, level,
                        Sql_user_condition_identity(), msg);
  }
  void mark_sql_conditions_for_removal()
  { get_warning_info()->mark_sql_conditions_for_removal(); }

  void unmark_sql_conditions_from_removal()
  { get_warning_info()->unmark_sql_conditions_from_removal(); }

  void remove_marked_sql_conditions()
  { get_warning_info()->remove_marked_sql_conditions(); }

  const Sql_condition *get_error_condition() const
  { return get_warning_info()->get_error_condition(); }

  void copy_sql_conditions_to_wi(THD *thd, Warning_info *dst_wi) const
  { dst_wi->append_warning_info(thd, get_warning_info()); }

  void copy_sql_conditions_from_wi(THD *thd, const Warning_info *src_wi)
  { get_warning_info()->append_warning_info(thd, src_wi); }

  void copy_non_errors_from_wi(THD *thd, const Warning_info *src_wi);

private:
  Warning_info *get_warning_info() { return m_wi_stack.front(); }

  const Warning_info *get_warning_info() const { return m_wi_stack.front(); }

private:
  /** True if status information is sent to the client. */
  bool m_is_sent;

  /** Set to make set_error_status after set_{ok,eof}_status possible. */
  bool m_can_overwrite_status;

  /** Message buffer. Can be used by OK or ERROR status. */
  char m_message[MYSQL_ERRMSG_SIZE];

  /**
    The number of rows affected by the last statement. This is
    semantically close to thd->m_row_count_func, but has a different
    life cycle. thd->m_row_count_func stores the value returned by
    function ROW_COUNT() and is cleared only by statements that
    update its value, such as INSERT, UPDATE, DELETE and few others.
    This member is cleared at the beginning of the next statement.

    We could possibly merge the two, but life cycle of thd->m_row_count_func
    can not be changed.
  */
  ulonglong    m_affected_rows;

  /**
    Similarly to the previous member, this is a replacement of
    thd->first_successful_insert_id_in_prev_stmt, which is used
    to implement LAST_INSERT_ID().
  */

  ulonglong   m_last_insert_id;
  /**
    Number of warnings of this last statement. May differ from
    the number of warnings returned by SHOW WARNINGS e.g. in case
    the statement doesn't clear the warnings, and doesn't generate
    them.
  */
  uint	     m_statement_warn_count;

  enum_diagnostics_status m_status;

  my_bool is_bulk_execution;

  Warning_info m_main_wi;

  Warning_info_list m_wi_stack;
};

///////////////////////////////////////////////////////////////////////////

void convert_error_to_warning(THD *thd);

void push_warning(THD *thd, Sql_condition::enum_warning_level level,
                  uint code, const char *msg);

void push_warning_printf(THD *thd, Sql_condition::enum_warning_level level,
                         uint code, const char *format, ...);

bool mysqld_show_warnings(THD *thd, ulong levels_to_show);

size_t convert_error_message(char *to, size_t to_length,
                             CHARSET_INFO *to_cs,
                             const char *from, size_t from_length,
                             CHARSET_INFO *from_cs, uint *errors);

extern const LEX_CSTRING warning_level_names[];

bool is_sqlstate_valid(const LEX_CSTRING *sqlstate);
/**
  Checks if the specified SQL-state-string defines COMPLETION condition.
  This function assumes that the given string contains a valid SQL-state.

  @param s the condition SQLSTATE.

  @retval true if the given string defines COMPLETION condition.
  @retval false otherwise.
*/
inline bool is_sqlstate_completion(const char *s)
{ return s[0] == '0' && s[1] == '0'; }


#endif // SQL_ERROR_H

/* Copyright (c) 2008 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

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

#ifndef SQL_SIGNAL_H
#define SQL_SIGNAL_H

/**
  Sql_cmd_common_signal represents the common properties of the
  SIGNAL and RESIGNAL statements.
*/
class Sql_cmd_common_signal : public Sql_cmd
{
protected:
  /**
    Constructor.
    @param cond the condition signaled if any, or NULL.
    @param set collection of signal condition item assignments.
  */
  Sql_cmd_common_signal(const sp_condition_value *cond,
                        const Set_signal_information& set)
    : Sql_cmd(),
      m_cond(cond),
      m_set_signal_information(set)
  {}

  virtual ~Sql_cmd_common_signal() = default;

  /**
    Evaluate each signal condition items for this statement.
    @param thd the current thread.
    @param cond the condition to update.
    @return 0 on success.
  */
  int eval_signal_informations(THD *thd, Sql_condition *cond);

  /**
    Raise a SQL condition.
    @param thd the current thread.
    @param cond the condition to raise.
    @return false on success.
  */
  bool raise_condition(THD *thd, Sql_condition *cond);

  /**
    The condition to signal or resignal.
    This member is optional and can be NULL (RESIGNAL).
  */
  const sp_condition_value *m_cond;

  /**
    Collection of 'SET item = value' assignments in the
    SIGNAL/RESIGNAL statement.
  */
  Set_signal_information m_set_signal_information;
};

/**
  Sql_cmd_signal represents a SIGNAL statement.
*/
class Sql_cmd_signal : public Sql_cmd_common_signal
{
public:
  /**
    Constructor, used to represent a SIGNAL statement.
    @param cond the SQL condition to signal (required).
    @param set the collection of signal information to signal.
  */
  Sql_cmd_signal(const sp_condition_value *cond,
                 const Set_signal_information& set)
    : Sql_cmd_common_signal(cond, set)
  {}

  virtual ~Sql_cmd_signal() = default;

  enum_sql_command sql_command_code() const override
  {
    return SQLCOM_SIGNAL;
  }

  bool execute(THD *thd) override;
};

/**
  Sql_cmd_resignal represents a RESIGNAL statement.
*/
class Sql_cmd_resignal : public Sql_cmd_common_signal
{
public:
  /**
    Constructor, used to represent a RESIGNAL statement.
    @param cond the SQL condition to resignal (optional, may be NULL).
    @param set the collection of signal information to resignal.
  */
  Sql_cmd_resignal(const sp_condition_value *cond,
                   const Set_signal_information& set)
    : Sql_cmd_common_signal(cond, set)
  {}

  virtual ~Sql_cmd_resignal() = default;

  enum_sql_command sql_command_code() const override
  {
    return SQLCOM_RESIGNAL;
  }

  bool execute(THD *thd) override;
};

#endif


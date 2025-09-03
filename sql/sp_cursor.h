/*
   Copyright (c) 2009, 2025, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef SP_CURSOR_INCLUDED
#define SP_CURSOR_INCLUDED

#include "sp_rcontext_handler.h"

class Server_side_cursor;

class sp_cursor_statistics
{
protected:
  ulonglong m_fetch_count; // Number of FETCH commands since last OPEN
  ulonglong m_row_count;   // Number of successful FETCH since last OPEN
  bool m_found;            // If last FETCH fetched a row
public:
  sp_cursor_statistics()
   :m_fetch_count(0),
    m_row_count(0),
    m_found(false)
  { }
  bool found() const
  { return m_found; }

  ulonglong row_count() const
  { return m_row_count; }

  ulonglong fetch_count() const
  { return m_fetch_count; }
  void reset() { *this= sp_cursor_statistics(); }
};


class sp_instr_cpush;

/* A mediator between stored procedures and server side cursors */
class sp_lex_keeper;
class sp_cursor: public sp_cursor_statistics
{
private:
  /// An interceptor of cursor result set used to implement
  /// FETCH <cname> INTO <varlist>.
  class Select_fetch_into_spvars: public select_result_interceptor
  {
    List<sp_fetch_target> *m_fetch_target_list;
    uint field_count;
    bool m_view_structure_only;
    bool send_data_to_variable_list(List<sp_fetch_target> &vars,
                                    List<Item> &items);
  public:
    Select_fetch_into_spvars(THD *thd_arg, bool view_structure_only)
     :select_result_interceptor(thd_arg),
      m_view_structure_only(view_structure_only)
    {}
    void reset(THD *thd_arg)
    {
      select_result_interceptor::reinit(thd_arg);
      m_fetch_target_list= NULL;
      field_count= 0;
    }
    uint get_field_count() { return field_count; }
    void set_spvar_list(List<sp_fetch_target> *vars)
    {
      m_fetch_target_list= vars;
    }

    bool send_eof() override { return FALSE; }
    int send_data(List<Item> &items) override;
    int prepare(List<Item> &list, SELECT_LEX_UNIT *u) override;
    bool view_structure_only() const override { return m_view_structure_only; }
};

public:
  sp_cursor(const Lex_ident_sys &ps_name)
   :m_ps_name(ps_name),
    result(NULL, false),
    server_side_cursor(NULL)
  { }
  sp_cursor(THD *thd_arg, bool view_structure_only)
   :result(thd_arg, view_structure_only),
    server_side_cursor(NULL)
  {}

  virtual ~sp_cursor()
  { destroy(); }

  virtual sp_lex_keeper *get_lex_keeper() { return nullptr; }

  int open(THD *thd, bool check_max_open_cursor_counter= true);

  int close(THD *thd);

  my_bool is_open() const
  { return MY_TEST(server_side_cursor); }

  int fetch(THD *, List<sp_fetch_target> *vars, bool error_on_no_data);

  bool export_structure(THD *thd, Row_definition_list *list);

  void reset(THD *thd_arg)
  {
    sp_cursor_statistics::reset();
    result.reinit(thd_arg);
    server_side_cursor= NULL;
  }

  /*
    Reset a cursor before reopening (two OPEN without CLOSE in between).
    This method does not raise ER_SP_CURSOR_ALREADY_OPEN.
    It's used to handle:
      c SYS_REFCURSOR;
      OPEN c FOR SELECT 1;
      OPEN c FOR SELECT 2; -- This is allowed without closing the previous OPEN
  */
  void reset_for_reopen(THD *thd_arg)
  {
    if (is_open())
      close(thd_arg);
    reset(thd_arg);
  }

  const Lex_ident_sys & ps_name() const { return m_ps_name; }
  virtual sp_instr_cpush *get_push_instr() { return nullptr; }
protected:
  Lex_ident_sys m_ps_name;
private:
public:
  Select_fetch_into_spvars result;
  Server_side_cursor *server_side_cursor;
private:
  void destroy();
};


class sp_cursor_array_element: public sp_cursor
{
  uint m_ref_count;
public:
  sp_cursor_array_element()
   :sp_cursor(Lex_ident_sys()),
    m_ref_count(0)
  { }
  uint ref_count() const { return m_ref_count; }
  void ref_count_inc() { m_ref_count++; }
  void ref_count_dec(THD *thd)
  {
    /*
      For performance purposes, the SP instructions in sp_head::m_instr
      do not guarantee that the number of ref_cursor_inc() calls matches the
      number of ref_cursor_dec() calls:

      We don't add sp_instr_destruct_variable instructions in these cases:
      - before sp_instr_freturn and sp_instr_preturn
      - after the very last instruction
        (the one before the END of the most outer stored routine block)
      So sp_head::execute() can leave with some SYS_REFCURORs variables
      still attached to thd->m_statement_cursor elements.

      Later they get detached by the sp_rcontext::sp_variable_detach_all()
      calls in sp_head::execute_procedure() and sp_head::execute_function().
      Executing a bunch of sp_instr_destruct_variable instructions would
      be more expensive.
    */
    if (m_ref_count > 0)
    {
      m_ref_count--;
      if (!m_ref_count && is_open())
        close(thd);
    }
  }
  void reset(THD *thd, uint ref_count)
  {
    sp_cursor::reset(thd);
    m_ref_count= ref_count;
  }
};


class sp_cursor_array: public Dynamic_array<sp_cursor_array_element>
{
protected:
  Type_ref_null find_unused()
  {
    for (size_t i= 0 ; i < size(); i++)
    {
      if (!at(i).is_open() && !at(i).ref_count())
        return Type_ref_null((ulonglong) i);
    }
    return Type_ref_null();
  }

  Type_ref_null append(THD *thd);

public:
  sp_cursor_array()
   :Dynamic_array(PSI_INSTRUMENT_MEM, 0)
  {}
  ~sp_cursor_array()
  {
    free(current_thd);
  }

  ULonglong_null ref_count(ulonglong offset) const
  {
    return offset < elements() ?
           ULonglong_null((ulonglong) at((size_t) offset).ref_count()) :
           ULonglong_null();
  }

  void ref_count_inc(ulonglong offset)
  {
    if (offset < elements())
      at((size_t) offset).ref_count_inc();
  }

  void ref_count_dec(THD *thd, ulonglong offset)
  {
    if (offset < elements())
      at((size_t) offset).ref_count_dec(thd);
  }

  void ref_count_update(THD *thd, const Type_ref_null &old_value,
                                  const Type_ref_null &new_value)
  {
    if (old_value.is_null())
    {
      if (!new_value.is_null())
        ref_count_inc(new_value.value());
    }
    else if (new_value.is_null())
    {
      ref_count_dec(thd, old_value.value());
    }
    else if (old_value.value() != new_value.value())
    {
      ref_count_dec(thd, old_value.value());
      ref_count_inc(new_value.value());
    }
  }

  /*
    Find a cursor at the offset specified by "ref".
    @param thd      - current thd
    @param ref      - the field containing the cursor offset
    @param for_open - tells if the cursor is needed for OPEN or
                      for FETCH/CLOSE and determines the behaviour
                      on dereference failure.

    Dereference failure means either of these:
    - ref->is_null() returned true.
      This happens when the reference SYS_REFCURSOR variable
      owning the Field "ref" is not assigned to any cursors yet.
    - ref->val_int() returned an offset greater than elements()-1.
      This can mean that something went wrong in the code.

    If dereference failed, then:
    - In case for_open is false the function returns nullptr.
    - In case for_open is true, the function searches for an unused cursor.
      If all cursors are used, it appends a new cursor to the end of the array.
  */
  sp_cursor_array_element *get_cursor_by_ref(THD *thd, Field *ref,
                                             bool for_open);
  void close(THD *thd)
  {
    for (uint i= 0; i < (uint) size(); i++)
    {
      if (at(i).is_open())
        at(i).close(thd);
    }
  }
  void free(THD *thd)
  {
    close(thd);
    free_memory();
  }
};

#endif // SP_CURSOR_INCLUDED

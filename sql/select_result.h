/* Copyright (c) 2000, 2016, Oracle and/or its affiliates.
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

#ifndef SELECT_RESULT_INCLUDED
#define SELECT_RESULT_INCLUDED

/* Pure interface for sending tabular data */
class select_result_sink: public Sql_alloc
{
public:
  THD *thd;
  select_result_sink(THD *thd_arg): thd(thd_arg) {}
  int send_data_with_check(List<Item> &items,
                           SELECT_LEX_UNIT *u,
                           ha_rows sent);
  /*
    send_data returns 0 on ok, 1 on error and -1 if data was ignored, for
    example for a duplicate row entry written to a temp table.
  */
  virtual int send_data(List<Item> &items)=0;
  virtual ~select_result_sink() = default;
  // Used in cursors to initialize and reset
  void reinit(THD *thd_arg) { thd= thd_arg; }
};

class select_result_interceptor;

/*
  Interface for sending tabular data, together with some other stuff:

  - Primary purpose seems to be seding typed tabular data:
     = the DDL is sent with send_fields()
     = the rows are sent with send_data()
  Besides that,
  - there seems to be an assumption that the sent data is a result of
    SELECT_LEX_UNIT *unit,
  - nest_level is used by SQL parser
*/

class select_result :public select_result_sink
{
protected:
  /*
    All descendant classes have their send_data() skip the first
    unit->offset_limit_cnt rows sent.  Select_materialize
    also uses unit->get_column_types().
  */
  SELECT_LEX_UNIT *unit;
  /* Something used only by the parser: */
public:
  ha_rows est_records;  /* estimated number of records in the result */
  select_result(THD *thd_arg): select_result_sink(thd_arg), est_records(0) {}
  void set_unit(SELECT_LEX_UNIT *unit_arg) { unit= unit_arg; }
  virtual ~select_result() = default;
  /**
    Change wrapped select_result.

    Replace the wrapped result object with new_result and call
    prepare() and prepare2() on new_result.

    This base class implementation doesn't wrap other select_results.

    @param new_result The new result object to wrap around

    @retval false Success
    @retval true  Error
  */
  virtual bool change_result(select_result *new_result)
  {
    return false;
  }
  virtual int prepare(List<Item> &list, SELECT_LEX_UNIT *u)
  {
    unit= u;
    return 0;
  }
  virtual int prepare2(JOIN *join) { return 0; }
  /*
    Because of peculiarities of prepared statements protocol
    we need to know number of columns in the result set (if
    there is a result set) apart from sending columns metadata.
  */
  virtual uint field_count(List<Item> &fields) const
  { return fields.elements; }
  virtual bool send_result_set_metadata(List<Item> &list, uint flags)=0;
  virtual bool initialize_tables (JOIN *join) { return 0; }
  virtual bool send_eof()=0;
  /**
    Check if this query returns a result set and therefore is allowed in
    cursors and set an error message if it is not the case.

    @retval FALSE     success
    @retval TRUE      error, an error message is set
  */
  virtual bool check_simple_select() const;
  virtual void abort_result_set() {}
  virtual void reset_for_next_ps_execution();
  void set_thd(THD *thd_arg) { thd= thd_arg; }
  void reinit(THD *thd_arg)
  {
    select_result_sink::reinit(thd_arg);
    unit= NULL;
  }
#ifdef EMBEDDED_LIBRARY
  virtual void begin_dataset() {}
#else
  void begin_dataset() {}
#endif
  virtual void update_used_tables() {}

  /* this method is called just before the first row of the table can be read */
  virtual void prepare_to_read_rows() {}

  void remove_offset_limit()
  {
    unit->lim.remove_offset();
  }

  /*
    This returns
    - NULL if the class sends output row to the client
    - this if the output is set elsewhere (a file, @variable, or table).
  */
  virtual select_result_interceptor *result_interceptor()=0;

  /*
    This method is used to distinguish an normal SELECT from the cursor
    structure discovery for cursor%ROWTYPE routine variables.
    If this method returns "true", then a SELECT execution performs only
    all preparation stages, but does not fetch any rows.
  */
  virtual bool view_structure_only() const { return false; }
};



/*
  Base class for select_result descendands which intercept and
  transform result set rows. As the rows are not sent to the client,
  sending of result set metadata should be suppressed as well.
*/

class select_result_interceptor: public select_result
{
public:
  select_result_interceptor(THD *thd_arg):
    select_result(thd_arg), suppress_my_ok(false)
  {
    DBUG_ENTER("select_result_interceptor::select_result_interceptor");
    DBUG_PRINT("enter", ("this %p", this));
    DBUG_VOID_RETURN;
  }              /* Remove gcc warning */
  uint field_count(List<Item> &fields) const override { return 0; }
  bool send_result_set_metadata(List<Item> &fields, uint flag) override { return FALSE; }
  select_result_interceptor *result_interceptor() override { return this; }

  /*
    Instruct the object to not call my_ok(). Client output will be handled
    elsewhere. (this is used by ANALYZE $stmt feature).
  */
  void disable_my_ok_calls() { suppress_my_ok= true; }
  void reinit(THD *thd_arg)
  {
    select_result::reinit(thd_arg);
    suppress_my_ok= false;
  }
protected:
  bool suppress_my_ok;
};

#endif // SELECT_RESULT_INCLUDED

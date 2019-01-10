/* Copyright (C) 2014 SkySQL Ab, MariaDB Corporation Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#ifndef JSON_WRITER_INCLUDED
#define JSON_WRITER_INCLUDED
#include "my_base.h"
#include "sql_select.h"
class Opt_trace_stmt;
class Opt_trace_context;
class Json_writer;
struct TABLE_LIST;


/*
  Single_line_formatting_helper is used by Json_writer to do better formatting
  of JSON documents. 

  The idea is to catch arrays that can be printed on one line:

    arrayName : [ "boo", 123, 456 ] 

  and actually print them on one line. Arrrays that occupy too much space on
  the line, or have nested members cannot be printed on one line.
  
  We hook into JSON printing functions and try to detect the pattern. While
  detecting the pattern, we will accumulate "boo", 123, 456 as strings.

  Then, 
   - either the pattern is broken, and we print the elements out, 
   - or the pattern lasts till the end of the array, and we print the 
     array on one line.
*/

class Single_line_formatting_helper
{
  enum enum_state
  {
    INACTIVE,
    ADD_MEMBER,
    IN_ARRAY,
    DISABLED
  };

  /*
    This works like a finite automaton. 

    state=DISABLED means the helper is disabled - all on_XXX functions will
    return false (which means "not handled") and do nothing.

                                      +->-+
                                      |   v
       INACTIVE ---> ADD_MEMBER ---> IN_ARRAY--->-+
          ^                                       |
          +------------------<--------------------+
                              
    For other states: 
    INACTIVE    - initial state, we have nothing.
    ADD_MEMBER  - add_member() was called, the buffer has "member_name\0".
    IN_ARRAY    - start_array() was called.


  */
  enum enum_state state;
  enum { MAX_LINE_LEN= 80 };
  char buffer[80];

  /* The data in the buffer is located between buffer[0] and buf_ptr */
  char *buf_ptr;
  uint line_len;

  Json_writer *owner;
public:
  Single_line_formatting_helper() : state(INACTIVE), buf_ptr(buffer) {}

  void init(Json_writer *owner_arg) { owner= owner_arg; }

  bool on_add_member(const char *name);

  bool on_start_array();
  bool on_end_array();
  void on_start_object();
  // on_end_object() is not needed.
   
  bool on_add_str(const char *str, size_t num_bytes);

  void flush_on_one_line();
  void disable_and_flush();
};


/*
  A class to write well-formed JSON documents. The documents are also formatted
  for human readability.
*/

class Json_writer
{
public:
  /* Add a member. We must be in an object. */
  Json_writer& add_member(const char *name);
  
  /* Add atomic values */
  void add_str(const char* val);
  void add_str(const char* val, size_t num_bytes);
  void add_str(const String &str);
  void add_str(Item *item);
  void add_table_name(JOIN_TAB *tab);

  void add_ll(longlong val);
  void add_size(longlong val);
  void add_double(double val);
  void add_bool(bool val);
  void add_null();

private:
  void add_unquoted_str(const char* val);
public:
  /* Start a child object */
  void start_object();
  void start_array();

  void end_object();
  void end_array();
  
  Json_writer() : 
    indent_level(0), document_start(true), element_started(false), 
    first_child(true)
  {
    fmt_helper.init(this);
  }
private:
  // TODO: a stack of (name, bool is_object_or_array) elements.
  int indent_level;
  enum { INDENT_SIZE = 2 };

  friend class Single_line_formatting_helper;
  friend class Json_writer_nesting_guard;
  bool document_start;
  bool element_started;
  bool first_child;

  Single_line_formatting_helper fmt_helper;

  void append_indent();
  void start_element();
  void start_sub_element();

  //const char *new_member_name;
public:
  String output;
};

/* A class to add values to Json_writer_object and Json_writer_array */
class Json_value_context
{
  Json_writer* writer;
  public:
  void init(Json_writer *my_writer) { writer= my_writer; }
  void add_str(const char* val)
  {
    if (writer)
      writer->add_str(val);
  }
  void add_str(const char* val, size_t length)
  {
    if (writer)
      writer->add_str(val);
  }
  void add_str(const String &str)
  {
    if (writer)
      writer->add_str(str);
  }
  void add_str(LEX_CSTRING str)
  {
    if (writer)
      writer->add_str(str.str);
  }
  void add_str(Item *item)
  {
    if (writer)
      writer->add_str(item);
  }

  void add_ll(longlong val)
  {
    if (writer)
      writer->add_ll(val);
  }
  void add_size(longlong val)
  {
    if (writer)
      writer->add_size(val);
  }
  void add_double(double val)
  {
    if (writer)
      writer->add_double(val);
  }
  void add_bool(bool val)
  {
    if (writer)
      writer->add_bool(val);
  }
  void add_null()
  {
    if (writer)
      writer->add_null();
  }
  void add_table_name(JOIN_TAB *tab)
  {
    if (writer)
      writer->add_table_name(tab);
  }
};

/* A common base for Json_writer_object and Json_writer_array */
class Json_writer_struct
{
protected:
  Json_writer* my_writer;
  Json_value_context context;
  /*
    Tells when a json_writer_struct has been closed or not
  */
  bool closed;

public:
  Json_writer_struct(Json_writer* writer)
  {
    my_writer= writer;
    context.init(writer);
    closed= false;
  }
};


/*
  RAII-based class to start/end writing a JSON object into the JSON document
*/

class Json_writer_object:public Json_writer_struct
{
private:
  void add_member(const char *name)
  {
    if (my_writer)
      my_writer->add_member(name);
  }
public:
  Json_writer_object(Json_writer *w);
  Json_writer_object(Json_writer *w, const char *str);
  Json_writer_object& add(const char *name, bool value)
  {
    add_member(name);
    context.add_bool(value);
    return *this;
  }
  Json_writer_object& add(const char* name, uint value)
  {
    add_member(name);
    context.add_ll(value);
    return *this;
  }
  Json_writer_object& add(const char* name, ha_rows value)
  {
    add_member(name);
    context.add_ll(value);
    return *this;
  }
  Json_writer_object& add(const char *name, longlong value)
  {
    add_member(name);
    context.add_ll(value);
    return *this;
  }
  Json_writer_object& add(const char *name, double value)
  {
    add_member(name);
    context.add_double(value);
    return *this;
  }
  Json_writer_object& add(const char *name, size_t value)
  {
    add_member(name);
    context.add_ll(value);
    return *this;
  }
  Json_writer_object& add(const char *name, const char *value)
  {
    add_member(name);
    context.add_str(value);
    return *this;
  }
  Json_writer_object& add(const char *name, const char *value, size_t num_bytes)
  {
    add_member(name);
    context.add_str(value, num_bytes);
    return *this;
  }
  Json_writer_object& add(const char *name, const String &value)
  {
    add_member(name);
    context.add_str(value);
    return *this;
  }
  Json_writer_object& add(const char *name, LEX_CSTRING value)
  {
    add_member(name);
    context.add_str(value.str);
    return *this;
  }
  Json_writer_object& add(const char *name, Item *value)
  {
    add_member(name);
    context.add_str(value);
    return *this;
  }
  Json_writer_object& add_null(const char*name)
  {
    add_member(name);
    context.add_null();
    return *this;
  }
  Json_writer_object& add_table_name(JOIN_TAB *tab)
  {
    add_member("table");
    context.add_table_name(tab);
    return *this;
  }
  void end()
  {
    if (my_writer)
      my_writer->end_object();
    closed= TRUE;
  }
  ~Json_writer_object();
};


/*
  RAII-based class to start/end writing a JSON array into the JSON document
*/
class Json_writer_array:public Json_writer_struct
{
public:
  Json_writer_array(Json_writer *w);
  Json_writer_array(Json_writer *w, const char *str);
  void end()
  {
    if (my_writer)
      my_writer->end_array();
    closed= TRUE;
  }
  Json_writer_array& add(bool value)
  {
    context.add_bool(value);
    return *this;
  }
  Json_writer_array& add(uint value)
  {
    context.add_ll(value);
    return *this;
  }
  Json_writer_array& add(ha_rows value)
  {
    context.add_ll(value);
    return *this;
  }
  Json_writer_array& add(longlong value)
  {
    context.add_ll(value);
    return *this;
  }
  Json_writer_array& add(double value)
  {
    context.add_double(value);
    return *this;
  }
  Json_writer_array& add(size_t value)
  {
    context.add_ll(value);
    return *this;
  }
  Json_writer_array& add(const char *value)
  {
    context.add_str(value);
    return *this;
  }
  Json_writer_array& add(const char *value, size_t num_bytes)
  {
    context.add_str(value, num_bytes);
    return *this;
  }
  Json_writer_array& add(const String &value)
  {
    context.add_str(value);
    return *this;
  }
  Json_writer_array& add(LEX_CSTRING value)
  {
    context.add_str(value.str);
    return *this;
  }
  Json_writer_array& add(Item *value)
  {
    context.add_str(value);
    return *this;
  }
  Json_writer_array& add_null()
  {
    context.add_null();
    return *this;
  }
  Json_writer_array& add_table_name(JOIN_TAB *tab)
  {
    context.add_table_name(tab);
    return *this;
  }
  ~Json_writer_array();
};


/*
  RAII-based helper class to detect incorrect use of Json_writer.

  The idea is that a function typically must leave Json_writer at the same
  identation level as it was when it was invoked. Leaving it at a different 
  level typically means we forgot to close an object or an array

  So, here is a way to guard
  void foo(Json_writer *writer)
  {
    Json_writer_nesting_guard(writer);
    .. do something with writer

    // at the end of the function, ~Json_writer_nesting_guard() is called
    // and it makes sure that the nesting is the same as when the function was
    // entered.
  }
*/

class Json_writer_nesting_guard
{
#ifdef DBUG_OFF
public:
  Json_writer_nesting_guard(Json_writer *) {}
#else
  Json_writer* writer;
  int indent_level;
public:
  Json_writer_nesting_guard(Json_writer *writer_arg) : 
    writer(writer_arg),
    indent_level(writer->indent_level)
  {}

  ~Json_writer_nesting_guard()
  {
    DBUG_ASSERT(indent_level == writer->indent_level);
  }
#endif
};

#endif

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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

class Json_writer;

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
   
  bool on_add_str(const char *str);

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
  void add_str(const String &str);

  void add_ll(longlong val);
  void add_ull(ulonglong val);
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
};



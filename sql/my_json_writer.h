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

#ifndef JSON_WRITER_INCLUDED
#define JSON_WRITER_INCLUDED

#include "my_base.h"
#include "sql_string.h"

#if !defined(NDEBUG) || defined(JSON_WRITER_UNIT_TEST) || defined ENABLED_JSON_WRITER_CONSISTENCY_CHECKS
#include <set>
#include <stack>
#include <string>
#include <vector>
#endif

#ifdef JSON_WRITER_UNIT_TEST
// Also, mock objects are defined in my_json_writer-t.cc
#define VALIDITY_ASSERT(x) if (!(x)) this->invalid_json= true;
#else
#include "sql_class.h"  // For class THD
#include "log.h" // for sql_print_error
#define VALIDITY_ASSERT(x) DBUG_ASSERT(x)
#endif

#include <type_traits>

class Opt_trace_stmt;
class Opt_trace_context;
class Json_writer;

struct TABLE;
struct st_join_table;
using JOIN_TAB= struct st_join_table;

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

  bool on_add_member(const char *name, size_t len);

  bool on_start_array();
  bool on_end_array();
  void on_start_object();
  // on_end_object() is not needed.

  bool on_add_str(const char *str, size_t num_bytes);

  /*
    Returns true if the helper is flushing its buffer and is probably
    making calls back to its Json_writer. (The Json_writer uses this
    function to avoid re-doing the processing that it has already done
    before making a call to fmt_helper)
  */
  bool is_making_writer_calls() const { return state == DISABLED; }

private:
  void flush_on_one_line();
  void disable_and_flush();
};


/*
  Something that looks like class String, but has an internal limit of
  how many bytes one can append to it.

  Bytes that were truncated due to the size limitation are counted.
*/

class String_with_limit
{
public:

  String_with_limit() : size_limit(SIZE_T_MAX), truncated_len(0)
  {
    str.length(0);
  }

  size_t get_truncated_bytes() const { return truncated_len; }
  size_t get_size_limit() { return size_limit; }

  void set_size_limit(size_t limit_arg)
  {
    // Setting size limit to be shorter than length will not have the desired
    // effect
    DBUG_ASSERT(str.length() < size_limit);
    size_limit= limit_arg;
  }

  void append(const char *s, size_t size)
  {
    if (str.length() + size <= size_limit)
    {
      // Whole string can be added, just do it
      str.append(s, size);
    }
    else
    {
      // We cannot add the whole string
      if (str.length() < size_limit)
      {
        // But we can still add something
        size_t bytes_to_add = size_limit - str.length();
        str.append(s, bytes_to_add);
        truncated_len += size - bytes_to_add;
      }
      else
        truncated_len += size;
    }
  }

  void append(const char *s)
  {
    append(s, strlen(s));
  }

  void append(char c)
  {
    if (str.length() + 1 > size_limit)
      truncated_len++;
    else
      str.append(c);
  }

  const String *get_string() { return &str; }
  size_t length() { return str.length(); }
private:
  String str;

  // str must not get longer than this many bytes.
  size_t size_limit;

  // How many bytes were truncated from the string
  size_t truncated_len;
};

/*
  A class to write well-formed JSON documents. The documents are also formatted
  for human readability.
*/

class Json_writer
{
#if !defined(NDEBUG) || defined(JSON_WRITER_UNIT_TEST)
  /*
    In debug mode, Json_writer will fail and assertion if one attempts to
    produce an invalid JSON document (e.g. JSON array having named elements).
  */
  std::vector<bool> named_items_expectation;
  std::stack<std::set<std::string> > named_items;

  bool named_item_expected() const;

  bool got_name;

#ifdef JSON_WRITER_UNIT_TEST
public:
  // When compiled for unit test, creating invalid JSON will set this to true
  // instead of an assertion.
  bool invalid_json= false;
#endif
#endif

public:
  /* Add a member. We must be in an object. */
  Json_writer& add_member(const char *name);
  Json_writer& add_member(const char *name, size_t len);
  
  /* Add atomic values */

  /* Note: the add_str methods do not do escapes. Should this change? */
  void add_str(const char* val);
  void add_str(const char* val, size_t num_bytes);
  void add_str(const String &str);
  void add_str(Item *item);
  void add_table_name(const JOIN_TAB *tab);
  void add_table_name(const TABLE* table);

  void add_ll(longlong val);
  void add_ull(ulonglong val);
  void add_size(longlong val);
  void add_double(double val);
  void add_bool(bool val);
  void add_null();

private:
  void add_unquoted_str(const char* val);
  void add_unquoted_str(const char* val, size_t len);

  bool on_add_str(const char *str, size_t num_bytes);
  void on_start_object();

public:
  /* Start a child object */
  void start_object();
  void start_array();

  void end_object();
  void end_array();
  
  /*
    One can set a limit of how large a JSON document should be.
    Writes beyond that size will be counted, but will not be collected.
  */
  void set_size_limit(size_t mem_size) { output.set_size_limit(mem_size); }

  size_t get_truncated_bytes() { return output.get_truncated_bytes(); }

  Json_writer() : 
#if !defined(NDEBUG) || defined(JSON_WRITER_UNIT_TEST)
    got_name(false),
#endif
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

public:
  String_with_limit output;
};

/* A class to add values to Json_writer_object and Json_writer_array */
class Json_value_helper
{
  Json_writer* writer;

public:
  void init(Json_writer *my_writer) { writer= my_writer; }
  void add_str(const char* val)
  {
      writer->add_str(val);
  }
  void add_str(const char* val, size_t length)
  {
      writer->add_str(val, length);
  }
  void add_str(const String &str)
  {
      writer->add_str(str.ptr(), str.length());
  }
  void add_str(const LEX_CSTRING &str)
  {
      writer->add_str(str.str, str.length);
  }
  void add_str(Item *item)
  {
      writer->add_str(item);
  }

  void add_ll(longlong val)
  {
      writer->add_ll(val);
  }
  void add_size(longlong val)
  {
      writer->add_size(val);
  }
  void add_double(double val)
  {
      writer->add_double(val);
  }
  void add_bool(bool val)
  {
      writer->add_bool(val);
  }
  void add_null()
  {
      writer->add_null();
  }
  void add_table_name(const JOIN_TAB *tab)
  {
      writer->add_table_name(tab);
  }
  void add_table_name(const TABLE* table)
  {
      writer->add_table_name(table);
  }
};

/* A common base for Json_writer_object and Json_writer_array */
class Json_writer_struct
{
  Json_writer_struct(const Json_writer_struct&)= delete;
  Json_writer_struct& operator=(const Json_writer_struct&)= delete;

#ifdef ENABLED_JSON_WRITER_CONSISTENCY_CHECKS
  static thread_local std::vector<bool> named_items_expectation;
#endif
protected:
  Json_writer* my_writer;
  Json_value_helper context;
  /*
    Tells when a json_writer_struct has been closed or not
  */
  bool closed;

  explicit Json_writer_struct(Json_writer *writer)
  : my_writer(writer)
  {
    context.init(my_writer);
    closed= false;
#ifdef ENABLED_JSON_WRITER_CONSISTENCY_CHECKS
    named_items_expectation.push_back(expect_named_children);
#endif
  }
  explicit Json_writer_struct(THD *thd)
  : Json_writer_struct(thd->opt_trace.get_current_json())
  {
  }

public:

#ifdef ENABLED_JSON_WRITER_CONSISTENCY_CHECKS
  virtual ~Json_writer_struct()
  {
    named_items_expectation.pop_back();
  }
#else
  virtual ~Json_writer_struct() = default;
#endif

  inline bool trace_started() const
  {
    return my_writer != 0;
  }

#ifdef ENABLED_JSON_WRITER_CONSISTENCY_CHECKS
  bool named_item_expected() const
  {
    return named_items_expectation.size() > 1
        && *(named_items_expectation.rbegin() + 1);
  }
#endif
};


/*
  RAII-based class to start/end writing a JSON object into the JSON document

  There is "ignore mode": one can initialize Json_writer_object with a NULL
  Json_writer argument, and then all its calls will do nothing. This is used
  by optimizer trace which can be enabled or disabled.
*/

class Json_writer_object : public Json_writer_struct
{
private:
  void add_member(const char *name)
  {
    my_writer->add_member(name);
  }
public:
  explicit Json_writer_object(Json_writer* writer, const char *str= nullptr)
  : Json_writer_struct(writer)
  {
#ifdef ENABLED_JSON_WRITER_CONSISTENCY_CHECKS
    DBUG_ASSERT(named_item_expected());
#endif
    if (unlikely(my_writer))
    {
      if (str)
        my_writer->add_member(str);
      my_writer->start_object();
    }
  }

  explicit Json_writer_object(THD* thd, const char *str= nullptr)
  : Json_writer_object(thd->opt_trace.get_current_json(), str)
  {
  }

  ~Json_writer_object()
  {
    if (my_writer && !closed)
      my_writer->end_object();
    closed= TRUE;
  }

  Json_writer_object& add(const char *name, bool value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
    {
      add_member(name);
      context.add_bool(value);
    }
    return *this;
  }

  Json_writer_object& add(const char *name, ulonglong value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
    {
      add_member(name);
      my_writer->add_ull(value);
    }
    return *this;
  }

  template<class IntT,
    typename= typename ::std::enable_if<std::is_integral<IntT>::value>::type
  >
  Json_writer_object& add(const char *name, IntT value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
    {
      add_member(name);
      context.add_ll(value);
    }
    return *this;
  }

  Json_writer_object& add(const char *name, double value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
    {
      add_member(name);
      context.add_double(value);
    }
    return *this;
  }

  Json_writer_object& add(const char *name, const char *value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
    {
      add_member(name);
      context.add_str(value);
    }
    return *this;
  }
  Json_writer_object& add(const char *name, const char *value, size_t num_bytes)
  {
    add_member(name);
    context.add_str(value, num_bytes);
    return *this;
  }
  Json_writer_object& add(const char *name, const LEX_CSTRING &value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
    {
      add_member(name);
      context.add_str(value.str, value.length);
    }
    return *this;
  }
  Json_writer_object& add(const char *name, Item *value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
    {
      add_member(name);
      context.add_str(value);
    }
    return *this;
  }
  Json_writer_object& add_null(const char*name)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
    {
      add_member(name);
      context.add_null();
    }
    return *this;
  }
  Json_writer_object& add_table_name(const JOIN_TAB *tab)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
    {
      add_member("table");
      context.add_table_name(tab);
    }
    return *this;
  }
  Json_writer_object& add_table_name(const TABLE *table)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
    {
      add_member("table");
      context.add_table_name(table);
    }
    return *this;
  }
  Json_writer_object& add_select_number(uint select_number)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
    {
      add_member("select_id");
      if (unlikely(select_number == FAKE_SELECT_LEX_ID))
        context.add_str("fake");
      else
        context.add_ll(static_cast<longlong>(select_number));
    }
    return *this;
  }
  void end()
  {
    DBUG_ASSERT(!closed);
    if (unlikely(my_writer))
      my_writer->end_object();
    closed= TRUE;
  }
};


/*
  RAII-based class to start/end writing a JSON array into the JSON document

  There is "ignore mode": one can initialize Json_writer_array with a NULL
  Json_writer argument, and then all its calls will do nothing. This is used
  by optimizer trace which can be enabled or disabled.
*/

class Json_writer_array : public Json_writer_struct
{
public:
  explicit Json_writer_array(Json_writer *writer, const char *str= nullptr)
    : Json_writer_struct(writer)
  {
#ifdef ENABLED_JSON_WRITER_CONSISTENCY_CHECKS
    DBUG_ASSERT(!named_item_expected());
#endif
    if (unlikely(my_writer))
    {
      if (str)
        my_writer->add_member(str);
      my_writer->start_array();
    }
  }

  explicit Json_writer_array(THD *thd, const char *str= nullptr)
    : Json_writer_array(thd->opt_trace.get_current_json(), str)
  {
  }

  ~Json_writer_array()
  {
    if (unlikely(my_writer && !closed))
    {
      my_writer->end_array();
      closed= TRUE;
    }
  }

  void end()
  {
    DBUG_ASSERT(!closed);
    if (unlikely(my_writer))
      my_writer->end_array();
    closed= TRUE;
  }

  Json_writer_array& add(bool value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
      context.add_bool(value);
    return *this;
  }
  Json_writer_array& add(ulonglong value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
      context.add_ll(static_cast<longlong>(value));
    return *this;
  }
  Json_writer_array& add(longlong value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
      context.add_ll(value);
    return *this;
  }
  Json_writer_array& add(double value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
      context.add_double(value);
    return *this;
  }
  #ifndef _WIN64
  Json_writer_array& add(size_t value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
      context.add_ll(static_cast<longlong>(value));
    return *this;
  }
  #endif
  Json_writer_array& add(const char *value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
      context.add_str(value);
    return *this;
  }
  Json_writer_array& add(const char *value, size_t num_bytes)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
      context.add_str(value, num_bytes);
    return *this;
  }
  Json_writer_array& add(const LEX_CSTRING &value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
      context.add_str(value.str, value.length);
    return *this;
  }
  Json_writer_array& add(Item *value)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
      context.add_str(value);
    return *this;
  }
  Json_writer_array& add_null()
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
      context.add_null();
    return *this;
  }
  Json_writer_array& add_table_name(const JOIN_TAB *tab)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
      context.add_table_name(tab);
    return *this;
  }
  Json_writer_array& add_table_name(const TABLE *table)
  {
    DBUG_ASSERT(!closed);
    if (my_writer)
      context.add_table_name(table);
    return *this;
  }
};

/*
  RAII-based class to disable writing into the JSON document
  The tracing is disabled as soon as the object is created.
  The destuctor is called as soon as we exit the scope of the object
  and the tracing is enabled back.
*/

class Json_writer_temp_disable
{
public:
  Json_writer_temp_disable(THD *thd_arg);
  ~Json_writer_temp_disable();
  THD *thd;
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

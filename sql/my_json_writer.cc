/* Copyright (C) 2014, 2020, MariaDB Corporation.

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

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_string.h"
#include "my_json_writer.h"

void Json_writer::append_indent()
{
  if (!document_start)
    output.append('\n');
  for (int i=0; i< indent_level; i++)
    output.append(' ');
}

void Json_writer::start_object()
{
  fmt_helper.on_start_object();

  if (!element_started)
    start_element();

  output.append('{');
  indent_level+=INDENT_SIZE;
  first_child=true;
  element_started= false;
  document_start= false;
}

void Json_writer::start_array()
{
  if (fmt_helper.on_start_array())
    return;

  if (!element_started)
    start_element();

  output.append('[');
  indent_level+=INDENT_SIZE;
  first_child=true;
  element_started= false;
  document_start= false;
}


void Json_writer::end_object()
{
  indent_level-=INDENT_SIZE;
  if (!first_child)
    append_indent();
  first_child= false;
  output.append('}');
}


void Json_writer::end_array()
{
  if (fmt_helper.on_end_array())
    return;
  indent_level-=INDENT_SIZE;
  if (!first_child)
    append_indent();
  output.append(']');
}


Json_writer& Json_writer::add_member(const char *name)
{
  size_t len= strlen(name);
  if (fmt_helper.on_add_member(name, len))
    return *this; // handled

  // assert that we are in an object
  DBUG_ASSERT(!element_started);
  start_element();

  output.append('"');
  output.append(name, len);
  output.append("\": ", 3);
  return *this;
}

Json_writer& Json_writer::add_member(const char *name, size_t len)
{
  if (fmt_helper.on_add_member(name, len))
    return *this; // handled

  // assert that we are in an object
  DBUG_ASSERT(!element_started);
  start_element();

  output.append('"');
  output.append(name, len);
  output.append(STRING_WITH_LEN("\": "));
  return *this;
}


/* 
  Used by formatting helper to print something that is formatted by the helper.
  We should only separate it from the previous element.
*/

void Json_writer::start_sub_element()
{
  //element_started= true;
  if (first_child)
    first_child= false;
  else
    output.append(',');

  append_indent();
}


void Json_writer::start_element()
{
  element_started= true;

  if (first_child)
    first_child= false;
  else
    output.append(',');

  append_indent();
}

void Json_writer::add_ll(longlong val)
{
  char buf[64];
  my_snprintf(buf, sizeof(buf), "%lld", val);
  add_unquoted_str(buf);
}

void Json_writer::add_ull(ulonglong val)
{
  char buf[64];
  my_snprintf(buf, sizeof(buf), "%llu", val);
  add_unquoted_str(buf);
}


/* Add a memory size, printing in Kb, Kb, Gb if necessary */
void Json_writer::add_size(longlong val)
{
  char buf[64];
  size_t len;
  if (val < 1024) 
    len= my_snprintf(buf, sizeof(buf), "%lld", val);
  else if (val < 1024*1024*16)
  {
    /* Values less than 16MB are specified in KB for precision */
    len= my_snprintf(buf, sizeof(buf), "%lld", val/1024);
    strcpy(buf + len, "Kb");
    len+= 2;
  }
  else
  {
    len= my_snprintf(buf, sizeof(buf), "%lld", val/(1024*1024));
    strcpy(buf + len, "Mb");
    len+= 2;
  }
  add_str(buf, len);
}


void Json_writer::add_double(double val)
{
  char buf[64];
  size_t len= my_snprintf(buf, sizeof(buf), "%-.11lg", val);
  add_unquoted_str(buf, len);
}


void Json_writer::add_bool(bool val)
{
  add_unquoted_str(val? "true" : "false");
}


void Json_writer::add_null()
{
  add_unquoted_str("null", (size_t) 4);
}


void Json_writer::add_unquoted_str(const char* str)
{
  size_t len= strlen(str);
  if (fmt_helper.on_add_str(str, len))
    return;

  if (!element_started)
    start_element();

  output.append(str, len);
  element_started= false;
}

void Json_writer::add_unquoted_str(const char* str, size_t len)
{
  if (fmt_helper.on_add_str(str, len))
    return;

  if (!element_started)
    start_element();

  output.append(str, len);
  element_started= false;
}

void Json_writer::add_str(const char *str)
{
  size_t len= strlen(str);
  if (fmt_helper.on_add_str(str, len))
    return;

  if (!element_started)
    start_element();

  output.append('"');
  output.append(str, len);
  output.append('"');
  element_started= false;
}

/*
  This function is used to add only num_bytes of str to the output string
*/

void Json_writer::add_str(const char* str, size_t num_bytes)
{
  if (fmt_helper.on_add_str(str, num_bytes))
    return;

  if (!element_started)
    start_element();

  output.append('"');
  output.append(str, num_bytes);
  output.append('"');
  element_started= false;
}

void Json_writer::add_str(const String &str)
{
  add_str(str.ptr(), str.length());
}

Json_writer_temp_disable::Json_writer_temp_disable(THD *thd_arg)
{
  thd= thd_arg;
  thd->opt_trace.disable_tracing_if_required();
}
Json_writer_temp_disable::~Json_writer_temp_disable()
{
  thd->opt_trace.enable_tracing_if_required();
}

bool Single_line_formatting_helper::on_add_member(const char *name,
                                                  size_t len)
{
  DBUG_ASSERT(state== INACTIVE || state == DISABLED);
  if (state != DISABLED)
  {
    // remove everything from the array
    buf_ptr= buffer;

    //append member name to the array
    if (len < MAX_LINE_LEN)
    {
      memcpy(buf_ptr, name, len);
      buf_ptr+=len;
      *(buf_ptr++)= 0;

      line_len= owner->indent_level + (uint)len + 1;
      state= ADD_MEMBER;
      return true; // handled
    }
  }
  return false; // not handled
}


bool Single_line_formatting_helper::on_start_array()
{
  if (state == ADD_MEMBER)
  {
    state= IN_ARRAY;
    return true; // handled
  }
  else
  {
    if (state != DISABLED)
      state= INACTIVE;
    // TODO: what if we have accumulated some stuff already? shouldn't we
    // flush it?
    return false; // not handled
  }
}


bool Single_line_formatting_helper::on_end_array()
{
  if (state == IN_ARRAY)
  {
    flush_on_one_line();
    state= INACTIVE;
    return true; // handled
  }
  return false; // not handled
}


void Single_line_formatting_helper::on_start_object()
{
  // Nested objects will not be printed on one line
  disable_and_flush();
}


bool Single_line_formatting_helper::on_add_str(const char *str,
                                               size_t len)
{
  if (state == IN_ARRAY)
  {
    // New length will be:
    //  "$string", 
    //  quote + quote + comma + space = 4
    if (line_len + len + 4 > MAX_LINE_LEN)
    {
      disable_and_flush();
      return false; // didn't handle the last element
    }

    //append string to array
    memcpy(buf_ptr, str, len);
    buf_ptr+=len;
    *(buf_ptr++)= 0;
    line_len += (uint)len + 4;
    return true; // handled
  }

  disable_and_flush();
  return false; // not handled
}


/*
  Append everything accumulated to the output on one line
*/

void Single_line_formatting_helper::flush_on_one_line()
{
  owner->start_sub_element();
  char *ptr= buffer;
  int nr= 0;
  while (ptr < buf_ptr)
  {
    char *str= ptr;

    if (nr == 0)
    {
      owner->output.append('"');
      owner->output.append(str);
      owner->output.append(STRING_WITH_LEN("\": "));
      owner->output.append('[');
    }
    else
    {
      if (nr != 1)
        owner->output.append(STRING_WITH_LEN(", "));
      owner->output.append('"');
      owner->output.append(str);
      owner->output.append('"');
    }
    nr++;

    while (*ptr!=0)
      ptr++;
    ptr++;
  }
  owner->output.append(']');
  /* We've printed out the contents of the buffer, mark it as empty */
  buf_ptr= buffer;
}


void Single_line_formatting_helper::disable_and_flush()
{
  if (state == DISABLED)
    return;

  bool start_array= (state == IN_ARRAY);
  state= DISABLED;
  // deactivate ourselves and flush all accumulated calls.
  char *ptr= buffer;
  int nr= 0;
  while (ptr < buf_ptr)
  {
    char *str= ptr;
    size_t len= strlen(str);

    if (nr == 0)
    {
      owner->add_member(str, len);
      if (start_array)
        owner->start_array();
    }
    else
    {
      //if (nr == 1)
      //  owner->start_array();
      owner->add_str(str, len);
    }
    
    nr++;
    ptr+= len+1;
  }
  buf_ptr= buffer;
  state= INACTIVE;
}


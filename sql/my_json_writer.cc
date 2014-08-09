/* Todo: SkySQL copyrights */

#include <my_global.h>
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
  if (!element_started)
    start_element();

  output.append("{");
  indent_level+=INDENT_SIZE;
  first_child=true;
  element_started= false;
  document_start= false;
}

void Json_writer::start_array()
{
  if (!element_started)
    start_element();

  output.append("[");
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
  output.append("}");
}


void Json_writer::end_array()
{
  indent_level-=INDENT_SIZE;
  if (!first_child)
    append_indent();
  output.append("]");
}


Json_writer& Json_writer::add_member(const char *name)
{
  // assert that we are in an object
  DBUG_ASSERT(!element_started);
  start_element();

  output.append('"');
  output.append(name);
  output.append("\": ");
  return *this;
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
  if (!element_started)
    start_element();

  char buf[64];
  my_snprintf(buf, sizeof(buf), "%ld", val);
  output.append(buf);
  element_started= false;
}


void Json_writer::add_double(double val)
{
  if (!element_started)
    start_element();

  char buf[64];
  my_snprintf(buf, sizeof(buf), "%lg", val);
  output.append(buf);
  element_started= false;
}


void Json_writer::add_str(const char *str)
{
  if (!element_started)
    start_element();

  output.append('"');
  output.append(str);
  output.append('"');
  element_started= false;
}

void Json_writer::add_bool(bool val)
{
  add_str(val? "true" : "false");
}

void Json_writer::add_str(const String &str)
{
  add_str(str.ptr());
}


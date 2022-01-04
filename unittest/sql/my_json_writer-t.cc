/*
   Copyright (c) 2021, MariaDB Corporation.

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

#include <my_global.h>
#include <my_pthread.h>
#include <my_sys.h>
#include <stdio.h>
#include <tap.h>

/*
  Unit tests for class Json_writer. At the moment there are only tests for the
  "Fail an assertion if one attempts to produce invalid JSON" feature.
*/

struct TABLE;
class Json_writer;


/* Several fake objects */
class Opt_trace 
{
public:
  void enable_tracing_if_required() {}
  void disable_tracing_if_required() {}
  Json_writer *get_current_json() { return nullptr; }
};

class THD 
{
public:
  Opt_trace opt_trace;
};

constexpr uint FAKE_SELECT_LEX_ID= UINT_MAX;

#define sql_print_error printf

#define JSON_WRITER_UNIT_TEST
#include "../sql/my_json_writer.h"
#include "../sql/my_json_writer.cc"

int main(int args, char **argv)
{
  plan(NO_PLAN);
  diag("Testing Json_writer checks");

  {
    Json_writer w;
    w.start_object();
    w.add_member("foo"); 
    w.end_object();
    ok(w.invalid_json, "Started a name but didn't add a value");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_ull(123);
    ok(w.invalid_json, "Unnamed value in an object");
  }

  {
    Json_writer w;
    w.start_array();
    w.add_member("bebebe").add_ull(345);
    ok(w.invalid_json, "Named member in array");
  }

  {
    Json_writer w;
    w.start_object();
    w.start_array();
    ok(w.invalid_json, "Unnamed array in an object");
  }

  {
    Json_writer w;
    w.start_object();
    w.start_object();
    ok(w.invalid_json, "Unnamed object in an object");
  }

  {
    Json_writer w;
    w.start_array();
    w.add_member("zzz");
    w.start_object();
    ok(w.invalid_json, "Named object in an array");
  }
  {
    Json_writer w;
    w.start_array();
    w.add_member("zzz");
    w.start_array();
    ok(w.invalid_json, "Named array in an array");
  }

  {
    Json_writer w;
    w.start_array();
    w.end_object();
    ok(w.invalid_json, "JSON object end of array");
  }

  {
    Json_writer w;
    w.start_object();
    w.end_array();
    ok(w.invalid_json, "JSON array end of object");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("name").add_ll(1);
    w.add_member("name").add_ll(2);
    w.end_object();
    ok(w.invalid_json, "JSON object member name collision");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("name").start_object();
    w.add_member("name").add_ll(2);
    ok(!w.invalid_json, "Valid JSON: nested object member name is the same");
  }

  diag("Done");

  return exit_status();
}


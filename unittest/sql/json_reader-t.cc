/*
   Copyright (c) 2026, MariaDB Corporation.

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
  Unit tests for json_read_object() 
*/

#include "../sql/sql_json_lib.h"
#include "../sql/sql_json_lib.cc"

using namespace json_reader;

int main(int args, char **argv)
{
  MY_INIT(argv[0]);

  plan(NO_PLAN);

  diag("Testing json_read_object checks");
  MEM_ROOT alloc;
  json_engine_t je;
  int rc;
  init_alloc_root(0, &alloc, 32768, 0, 0);
  mem_root_dynamic_array_init(&alloc, 0, &je.stack,
                              sizeof(int), NULL, JSON_DEPTH_DEFAULT,
                              JSON_DEPTH_INC, MYF(0));
  system_charset_info= &my_charset_utf8mb3_bin;
  String err_buf;

  /* Basic json_read_object test */
  {
    const char *js_doc="{ \"str_val\": \"abc\", \"double_val\": 1234.5 }";
    json_scan_start(&je, &my_charset_utf8mb3_bin, (const uchar *) js_doc,
                    (const uchar *) js_doc + strlen(js_doc));

    char *parsed_name;
    double parsed_dbl;
    Read_named_member array[]= {
        {"str_val",    Read_string(&alloc, &parsed_name), false},
        {"double_val", Read_double(&parsed_dbl), false},
        {NULL, Read_double(NULL), false }
    };
    rc= json_read_object(&je, array, &err_buf);
    ok(!rc, "Basic object read");
  }

  /* Test that reading handles characters correctly. */
  {
    /* utf8mb4 character, U+1F600 Grinning Face: */
    const char *field_val="\xF0\x9F\x98\x80";
    const char *js_doc="{ \"str\" : \"\xF0\x9F\x98\x80\"}";
    json_scan_start(&je, &my_charset_utf8mb4_bin, (const uchar *) js_doc,
                    (const uchar *) js_doc + strlen(js_doc));
    char *str;
    Read_named_member array[]= {
        {"str", Read_string(&alloc, &str), false},
        {NULL,  Read_double(NULL), false }
    };
    rc= json_read_object(&je, array, &err_buf);
    ok(!rc, "Success reading UTF-8 grinning face (with wchar_t > 255)");
    ok(!strcmp(str, field_val), "Got correct value for grinning face");
  }

  free_root(&alloc, 0);
  diag("Done");

  my_end(MY_CHECK_ERROR);
  return exit_status();
}

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
#include "json_lib.h"

using namespace json_reader;

/**************************************************************************
 * Nested objects/arrays parsing tests
 **************************************************************************/

class Level1_struct
{
public:
  char *idx_name;
};

/*
  Parse a JSON object into Level1_struct.
*/
Level1_struct* parse_level1_struct(MEM_ROOT *mem_root, json_engine_t *je,
                                   String *err_buf)
{
  //const char *err_msg= "Expected an object in the indexes array";
  char *str;

  Read_named_member array[]= {
      {"index_name", Read_string(mem_root, &str), false},
      {NULL, Read_double(NULL), true}};

  int rc;
  rc= json_read_object(je, array, err_buf);
  if (rc)
    return NULL;

  return new Level1_struct{str};
}

class Level2_struct
{
public:
  Level1_struct *child1;
  Level1_struct *child2;
};


/*
  Parse JSON into Level2_struct.

  Level2_struct is structure with another two structures in it.
*/
static
Level2_struct* parse_level2_struct(MEM_ROOT *mem_root,
                                   json_engine_t *je,
                                   String *err_buf)
{
  Level1_struct *ps1;
  Level1_struct *ps2;
  Read_named_member memb[]=
  {
    {"object1",
     Read_object<Level1_struct>(mem_root, &ps1, parse_level1_struct),
     false
    },
    {"object2",
     Read_object<Level1_struct>(mem_root, &ps2, parse_level1_struct),
     false
    },
    {NULL, Read_double(NULL), true}
  };

  int rc;
  rc= json_read_object(je, memb, err_buf);
  if (rc)
    return NULL;

  return new Level2_struct{ps1, ps2};
}


/*
  Parse a JSON object that has an array of Level1_struct objects.
*/
int parse_array_of_level1_struct(MEM_ROOT *mem_root, json_engine_t *je,
                                 String *err_buf, const char *js_doc,
                                 List<Level1_struct> *index_list)
{
  json_scan_start(je, &my_charset_utf8mb4_bin, (const uchar *) js_doc,
                  (const uchar *) js_doc + strlen(js_doc));

  Read_named_member array[]= {
      {"indexes",
       Read_object_array<Level1_struct>(mem_root, index_list,
                                        parse_level1_struct),
       true},
      {NULL, Read_double(NULL), true}};

  int rc= json_read_object(je, array, err_buf);
  return rc;
}


void test_parse_array_of_objects(MEM_ROOT *mem_root, json_engine_t *je)
{
  String err_buf;
  int rc;
  const char *two_elems_doc=
    "{                                  \n"
    "   \"indexes\" : [                 \n"
    "      {                            \n"
    "        \"index_name\":\"index1\"  \n"
    "      },                           \n"
    "      {                            \n"
    "        \"index_name\":\"index2\"  \n"
    "      }                            \n"
    "   ]                               \n"
    "}";
  List<Level1_struct> two_elems_list;
  rc= parse_array_of_level1_struct(mem_root, je, &err_buf, two_elems_doc,
                                   &two_elems_list);
  ok(!rc, "Array of two objects parsed successfully");
  ok(two_elems_list.elements == 2, ".. Got two objects");

  const char *no_elems_doc=
    "{                                  \n"
    "   \"indexes\" : [                 \n"
    "   ]                               \n"
    "}";
  List<Level1_struct> no_elems_list;
  rc= parse_array_of_level1_struct(mem_root, je, &err_buf, no_elems_doc,
                                   &no_elems_list);
  ok(!rc, "Empty array of objects parsed successfully");
  ok(two_elems_list.elements == 2, ".. Got an empty list");
}


void test_parse_nested_object(MEM_ROOT *mem_root, json_engine_t *je)
{
  String err_buf;
  const char *js_doc=
    "{                                  \n"
    "   \"object1\" :                   \n"
    "      {                            \n"
    "        \"index_name\":\"index1\"  \n"
    "      },                           \n"
    "   \"object2\" :                   \n"
    "      {                            \n"
    "        \"index_name\":\"index2\"  \n"
    "      }                            \n"
    "   }                               \n"
    "}";
  json_scan_start(je, &my_charset_utf8mb4_bin, (const uchar *) js_doc,
                  (const uchar *) js_doc + strlen(js_doc));

  Level2_struct *data= parse_level2_struct(mem_root, je, &err_buf);

  ok(data!=NULL, "Nested object successfully");
  ok(!strcmp(data->child1->idx_name, "index1"), "Child object 1 is ok");
  ok(!strcmp(data->child2->idx_name, "index2"), "Child object 2 is ok");
}


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

  diag("Test reading array-of-strings");
  {
    const char *js_doc="{ \"arr\" : [\"foo\", \"bar\", \"baz\"] }";
    json_scan_start(&je, &my_charset_utf8mb4_bin, (const uchar *) js_doc,
                    (const uchar *) js_doc + strlen(js_doc));
    List<char> str_array;
    Read_named_member array[]= {
        {"arr", Read_array_of_strings(&alloc, &str_array), false},
        {NULL,  Read_double(NULL), false }
    };
    rc= json_read_object(&je, array, &err_buf);
    ok(!rc, "Success parsing 3-element array");
    ok(str_array.elements == 3, "Got 3-element array");
  }

  {
    const char *js_doc="{ \"arr\" : [] }";
    json_scan_start(&je, &my_charset_utf8mb4_bin, (const uchar *) js_doc,
                    (const uchar *) js_doc + strlen(js_doc));
    List<char> str_array;
    Read_named_member array[]= {
        {"arr", Read_array_of_strings(&alloc, &str_array), false},
        {NULL,  Read_double(NULL), false }
    };
    rc= json_read_object(&je, array, &err_buf);
    ok(!rc, "Success parsing empty array");
    ok(str_array.elements == 0, "Got an empty array");
  }

  test_parse_array_of_objects(&alloc, &je);
  test_parse_nested_object(&alloc, &je);

  free_root(&alloc, 0);
  diag("Done");

  my_end(MY_CHECK_ERROR);
  return exit_status();
}

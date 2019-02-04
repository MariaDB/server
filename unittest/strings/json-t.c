/* Copyright (c) 2019, MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include <tap.h>
#include <my_sys.h>
#include <json_lib.h>

int json_locate_key(const char *js, const char *js_end, const char *kname,
                    const char **key_start, const char **key_end,
                    int *comma_pos);
int main()
{
  const char *json="{\"int\":1, \"str\":\"foo bar\", "
                   "\"array\":[10,20,{\"c\":\"d\"}],\"bool\":false}";
  const char *json_ar="[1, \"foo bar\", " "[10,20,{\"c\":\"d\"}], false]";
  const char *json_w="{\"int\" : 1 , \"str\" : \"foo bar\" , "
                   "\"array\" : [10,20,{\"c\":\"d\"}]  , \"bool\" : false  }";
  const char *json_1="{ \"str\" : \"foo bar\"   }";
  enum json_types value_type;
  const char *value_start;
  int value_len;
  const char *key_start, *key_end;
  int result, comma_pos;

  plan(15);

#define do_json(V)                                                      \
  do {                                                                  \
    value_type= json_get_object_key(json, json+strlen(json),            \
                  V, &value_start, &value_len);                         \
    ok(value_type != JSV_BAD_JSON, V);                                  \
    diag("type=%d, value=\"%.*s\"", value_type, (int)value_len, value_start); \
  } while(0)
#define do_json_ar(N)                                                 \
  do {                                                                \
    value_type= json_get_array_item(json_ar, json_ar+strlen(json_ar), \
                  N, &value_start, &value_len);                       \
    ok(value_type != JSV_BAD_JSON, #N);                      \
    diag("type=%d, value=\"%.*s\"", value_type, (int)value_len, value_start); \
  } while(0)
#define do_json_locate(J, V)                              \
  do {                                                    \
    result= json_locate_key(J, J+strlen(J),               \
                  V, &key_start, &key_end, &comma_pos);   \
    ok(result == 0, V);                                   \
    if (key_start)                                        \
      diag("key_str=\"%.*s\" comma_pos= %d", (int)(key_end - key_start), key_start, comma_pos); \
    else                                                                                    \
      diag("no key found"); \
  } while(0)

  do_json("int");
  do_json("str");
  do_json("bool");
  do_json("c");
  do_json("array");

  do_json_ar(0);
  do_json_ar(1);
  do_json_ar(2);
  do_json_ar(3);
  do_json_ar(4);

  do_json_locate(json_w, "bool");
  do_json_locate(json_w, "int");
  do_json_locate(json_w, "array");
  do_json_locate(json_1, "str");
  do_json_locate(json_w, "c");
  return exit_status();
}

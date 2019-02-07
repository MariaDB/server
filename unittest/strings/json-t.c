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

const char *json="{\"int\":1,\"str\":\"foo bar\","
                 "\"array\":[10,20,{\"c\":\"d\"}],\"bool\":false}";

const char *json_ar="[1,\"foo bar\",[10,20,{\"c\":\"d\"}],false]";

const char *json_w="{\"int\" : 1 ,  "
                 "\"array\" : [10,20,{\"c\":\"d\"}]  , \"bool\" : false  }";
const char *json_1="{ \"str\" : \"foo bar\"   }";

void do_json(const char *key, int type, const char *value)
{
  enum json_types value_type;
  const char *value_start;
  int value_len;

  value_type= json_get_object_key(json, json + strlen(json),
                key, &value_start, &value_len);
  if (type)
    ok(value_type == type && value_len == (int)strlen(value) && !memcmp(value_start, value, value_len),
       "%s: type=%u, value(%d)=\"%.*s\"", key, value_type, value_len, value_len, value_start);
  else
    ok(value_type == type && value_len == (int)strlen(value),
       "%s: type=%u keys=%u end=\"%s\"", key, value_type, value_len, value_start);
}

void do_json_ar(int n, int type, const char *value)
{
  enum json_types value_type;
  const char *value_start;
  int value_len;

  value_type= json_get_array_item(json_ar, json_ar + strlen(json_ar),
                                  n, &value_start, &value_len);
  if (type)
    ok(value_type == type && value_len == (int)strlen(value) && !memcmp(value_start, value, value_len),
       "%i: type=%u, value(%d)=\"%.*s\"", n, value_type, value_len, value_len, value_start);
  else
    ok(value_type == type && value_len == (int)strlen(value),
       "%i: type=%u keys=%u end=\"%s\"", n, value_type, value_len, value_start);
}

void do_json_locate(const char *json, const char *key, int from, int to, int cp)
{
  const char *key_start, *key_end;
  int res, comma_pos;

  res= json_locate_key(json, json + strlen(json),
                       key, &key_start, &key_end, &comma_pos);
  if (key_start)
    ok(res == 0 && key_start - json == from && key_end - json == to &&
       comma_pos == cp, "%s: [%d,%d,%d] %.*s%s", key, (int)(key_start-json),
       (int)(key_end-json), comma_pos, (int)(key_start - json), json, key_end);
  else
    ok(res == 0 && from == -1, "%s: key not found", key);
}

int main()
{
  plan(18);

  diag("%s", json);
  do_json("int", 4, "1");
  do_json("str", 3, "foo bar");
  do_json("bool", 6, "false");
  do_json("c", 0, "1234");
  do_json("array", 2, "[10,20,{\"c\":\"d\"}]");
  diag("%s", json_ar);
  do_json_ar(0, 4, "1");
  do_json_ar(1, 3, "foo bar");
  do_json_ar(2, 2, "[10,20,{\"c\":\"d\"}]");
  do_json_ar(3, 6, "false");
  do_json_ar(4, 0, "1234");

  do_json_locate(json, "bool", 50, 63, 1);
  do_json_locate(json, "int", 1, 9, 2);
  do_json_locate(json, "array", 24, 50, 1);
  do_json_locate(json_w, "bool", 43, 61, 1);
  do_json_locate(json_w, "int", 1, 12, 2);
  do_json_locate(json_w, "array", 11, 43, 1);
  do_json_locate(json_w, "c", -1, -1, -1);
  do_json_locate(json_1, "str", 1, 22, 0);

  return exit_status();
}

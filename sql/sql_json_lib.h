/*
   Copyright (c) 2025, MariaDB
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA */

#ifndef SQL_JSON_LIB
#define SQL_JSON_LIB

/*
  A syntax sugar interface to json_string_t
*/
class Json_string
{
  json_string_t str;

public:
  explicit Json_string(const char *name)
  {
    json_string_set_str(&str, (const uchar *) name,
                        (const uchar *) name + strlen(name));
    json_string_set_cs(&str, system_charset_info);
  }
  json_string_t *get() { return &str; }
};

/*
  This [partially] saves the JSON parser state and then can rollback the parser
  to it.
  The goal of this is to be able to make multiple json_key_matches() calls:
    Json_saved_parser_state save(je);
    if (json_key_matches(je, KEY_NAME_1)) {
      ...
      return;
    }
    save.restore_to(je);
    if (json_key_matches(je, KEY_NAME_2)) {
      ...
    }
  This allows one to parse JSON objects where [optional] members come in any
  order.
*/
class Json_saved_parser_state
{
  const uchar *c_str;
  my_wc_t c_next;
  int state;

public:
  explicit Json_saved_parser_state(const json_engine_t *je)
      : c_str(je->s.c_str), c_next(je->s.c_next), state(je->state)
  {
  }
  void restore_to(json_engine_t *je)
  {
    je->s.c_str= c_str;
    je->s.c_next= c_next;
    je->state= state;
  }
};

/*
  @brief
    Un-escape a JSON string and save it into *out.
*/
bool json_unescape_to_string(const char *val, int val_len, String *out);

#endif
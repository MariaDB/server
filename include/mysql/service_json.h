/* Copyright (C) 2018 MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#ifndef MYSQL_SERVICE_JSON
#define MYSQL_SERVICE_JSON

/**
  @file
  json service

  Exports JSON parsing methods for plugins to use.

  Functions of the service:
    json_type - returns the type of the JSON argument,
       and the parsed value if it's scalar (not object or array)

    json_get_array_item - expects JSON array as an argument,
       and returns the type of the element at index `n_item`.
       Returns JSV_NOTHING type if the array is shorter
       than n_item and the actual length of the array in value_len.
       If successful, then `value` up till `value[value_len]` contains the array
       element at the desired index (n_item).

    json_get_object_key - expects JSON object as an argument,
       searches for a key in the object, return it's type and stores its value in `value`.
       JSV_NOTHING if no such key found, the number of keys
       in v_len.

    json_get_object_nkey - expects JSON object as an argument.
      finds n_key's key in the object, returns it's name, type and value.
      JSV_NOTHING if object has less keys than n_key.
*/


#ifdef __cplusplus
extern "C" {
#endif

enum json_types
{
  JSV_BAD_JSON=-1,
  JSV_NOTHING=0,
  JSV_OBJECT=1,
  JSV_ARRAY=2,
  JSV_STRING=3,
  JSV_NUMBER=4,
  JSV_TRUE=5,
  JSV_FALSE=6,
  JSV_NULL=7
};

extern struct json_service_st {
  enum json_types (*json_type)(const char *js, const char *js_end,
                               const char **value, int *value_len);
  enum json_types (*json_get_array_item)(const char *js, const char *js_end,
                                         int n_item,
                                         const char **value, int *value_len);
  enum json_types (*json_get_object_key)(const char *js, const char *js_end,
                                         const char *key,
                                         const char **value, int *value_len);
  enum json_types (*json_get_object_nkey)(const char *js,const char *js_end,
                             int nkey,
                             const char **keyname, const char **keyname_end,
                             const char **value, int *value_len);
  int (*json_escape_string)(const char *str,const char *str_end,
                          char *json, char *json_end);
  int (*json_unescape_json)(const char *json_str, const char *json_end,
                          char *res, char *res_end);
} *json_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define json_type json_service->json_type
#define json_get_array_item json_service->json_get_array_item
#define json_get_object_key json_service->json_get_object_key
#define json_get_object_nkey json_service->json_get_object_nkey
#define json_escape_string json_service->json_escape_string
#define json_unescape_json json_service->json_unescape_json

#else

enum json_types json_type(const char *js, const char *js_end,
                          const char **value, int *value_len);
enum json_types json_get_array_item(const char *js, const char *js_end,
                                    int n_item,
                                    const char **value, int *value_len);
enum json_types json_get_object_key(const char *js, const char *js_end,
                                    const char *key,
                                    const char **value, int *value_len);
enum json_types json_get_object_nkey(const char *js,const char *js_end, int nkey,
                       const char **keyname, const char **keyname_end,
                       const char **value, int *value_len);
int json_escape_string(const char *str,const char *str_end,
                       char *json, char *json_end);
int json_unescape_json(const char *json_str, const char *json_end,
                       char *res, char *res_end);

#endif /*MYSQL_DYNAMIC_PLUGIN*/


#ifdef __cplusplus
}
#endif

#endif /*MYSQL_SERVICE_JSON */



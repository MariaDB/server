/*
   Copyright (c) 2019, MariaDB Corporation

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

#define MYSQL_SERVER

#include <my_global.h>
#include <sql_class.h>
#include <mysql/plugin_function.h>
#include <curl/curl.h>
#include "sql_type_vector.h"
#include "item_jsonfunc.cc"

static char *host, *api_key;

static MYSQL_THDVAR_STR(host,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "OpenAI API host, can be set to 'https://api.openai.com/v1/embeddings' or a custom endpoint",
  NULL, NULL, "My Default Host"); // TODO: Change this

static MYSQL_THDVAR_STR(api_key,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "OpenAI API key",
  NULL, NULL, "My Default API Key"); // TODO: Change this

static struct st_mysql_sys_var* system_variables[]= {
  MYSQL_SYSVAR(host),
  MYSQL_SYSVAR(api_key),
  NULL
};

class Item_func_gen_embedding: public Item_str_func 
{
  String tmp_js, api_response;

  int make_openai_request() {
    /* 
      Make the request to OpenAI API to generate the embedding
      Store the result in api_response
      This is a placeholder for the actual implementation
    */
    
    // std::cout << "Host " << host << std::endl;
    // std::cout <<  "API KEY" << api_key << std::endl;

    String *value = args[0]->val_json(&api_response); // For now just pass the input text
    api_response.copy(*value);
    if (!value) {
      null_value= true;
      return 1;
    }
    return 0;
  }

  
  int parse_vector(String *buf, json_engine_t je, CHARSET_INFO *cs, const uchar *start, const uchar *end);
  String *read_json(String *str, json_value_types *type, char **out_val);
  json_path_with_flags *paths;
  
  public:
    using Item_str_func::Item_str_func;
    bool fix_length_and_dec(THD *thd) override
    {
      // TODO: I am grabbing the session variables here, because this is the function that has access to the THD
      // Is there a better place to do this?
      host = THDVAR(thd, host);
      api_key = THDVAR(thd, api_key);

      decimals= 0;
      uint max_dimensions = 3072;
      // TODO: What is the best way/place to do this allocation?
      MEM_ROOT *root= thd->active_stmt_arena_to_use()->mem_root;
      uint n_paths = 1;
      paths= (json_path_with_flags *) alloc_root(root, sizeof(json_path_with_flags) * n_paths);
      // We want to store max_dimensions floats, each 4 bytes 
      fix_length_and_charset(max_dimensions * 4, &my_charset_bin);
      set_maybe_null();
      return false;
    }
    
    String *val_str(String *buf) override
    {
      json_value_types type;
      char *value;

      if (make_openai_request())
      {
        null_value= true;
        return NULL;
      }   
      return read_json(buf, &type, &value);
    }
    
    LEX_CSTRING func_name_cstring() const override
    {
      static LEX_CSTRING name= "GENERATE_EMBEDDING_OPENAI"_LEX_CSTRING;
      return name;
    }
    Item *do_get_copy(THD *thd) const override
    { return get_item_copy<Item_func_gen_embedding>(thd, this); }
  
  class Create_func : public Create_func_arg2
  {
  public:
    Item *create_2_arg(THD *thd, Item *arg1, Item* arg2) override
    {
      // Specific argument parsing logic should go here ? (TODO)
      return new (thd->mem_root) Item_func_gen_embedding(thd, arg1, arg2);
    }
  };

  static Plugin_function *plugin_descriptor()
  {
    static Create_func creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};


String *Item_func_gen_embedding::read_json(String *str,
                                          json_value_types *type,
                                          char **out_val)
{
  String *js= &api_response;
  json_engine_t je;
  json_path_t p;
  const uchar *value;
  int not_first_value= 0, count_path= 0;
  size_t v_len;
  int array_size_counter[JSON_DEPTH_LIMIT];
  json_path_with_flags *c_path= paths;

  if ((null_value= args[0]->null_value))
    return 0;

  c_path->p.types_used= JSON_PATH_KEY_NULL;
  // TODO: This path constant needs to be defined in another place probably
  const uchar* s_p = (const uchar*) "$.data[0].embedding";
  if (s_p)
  {
    if (json_path_setup(&c_path->p,&my_charset_utf8mb3_general_ci, s_p,
                      s_p + strlen((const char *) s_p)))
    {
    //  report_path_error(s_p, &c_path->p, n_arg);
      goto return_null;
    }
  }

  *type= JSON_VALUE_NULL;

  if (str)
  {
    str->set_charset(js->charset());
    str->length(0);
  }

  json_get_path_start(&je, js->charset(),(const uchar *) js->ptr(),
                      (const uchar *) js->ptr() + js->length(), &p);

  while (json_get_path_next(&je, &p) == 0)
  {

    if (!(count_path= path_exact(paths, arg_count-1, &p, je.value_type,
                                 array_size_counter)))
      continue;

    value= je.value_begin;

    if (*type == JSON_VALUE_NULL)
    {
      *type= je.value_type;
      *out_val= (char *) je.value;
      // value_len= je.value_len;
    }
    if (!str)
    {
      /* If str is NULL, we only care about the first found value. */
      goto return_ok;
    }

    if (json_value_scalar(&je))
      v_len= je.value_end - value;
    else
    {
      if (json_skip_level(&je))
        goto error;
      v_len= je.s.c_str - value;
    }

    if ((not_first_value && str->append(", ", 2)))
      goto error;
    // The path is found, starting at position value, with length v_len
    if (parse_vector(str, je, js->charset(), value, value + v_len))
      goto error;
    return str;
  }

  // if (unlikely(je.s.error))
  //   goto error;

  if (!not_first_value)
  {
    /* Nothing was found. */
    goto return_null;
  }


return_ok:
// return &tmp_js;
// My return, result is in js
  return str;

error:
  // report_json_error(js, &je, 0);
return_null:
  null_value= 1;
  return 0;
}

int Item_func_gen_embedding::parse_vector(String *buf, json_engine_t je, CHARSET_INFO *cs, const uchar *start, const uchar *end)
{
  /* 
    This function is used to parse the vector from the JSON response
    It should be called after the embedding is located, for the start and end positions 
    of the json_engine to be known
    The vector is expected to be in binary format
    Returns 0 on success, 1 on error
  */
   bool end_ok= false;
  if (json_scan_start(&je, cs, start, end) ||
      json_read_value(&je))
    goto error;
  
  if (je.value_type != JSON_VALUE_ARRAY)
    goto error; // Error, not an array

  do {
    switch (je.state)
    {
      case JST_ARRAY_START:
        continue;
      case JST_ARRAY_END:
        end_ok = true;
        break;
      case JST_VALUE:
      {
        if (json_read_value(&je))
          goto error;

        if (je.value_type != JSON_VALUE_NUMBER)
          goto error_format;

        int error;
        char *start= (char *)je.value_begin, *end;
        float f= (float)cs->strntod(start, je.value_len, &end, &error);
        if (unlikely(error))
            goto error_format;

        char f_bin[4];
        float4store(f_bin, f);
        buf->append(f_bin, sizeof(f_bin));
        break;
      }
      default:
        goto error_format;
    }
  } while (json_scan_next(&je) == 0);

  if (!end_ok)
    goto error;
  if (Type_handler_vector::is_valid(buf->ptr(), buf->length()))
    return 0;
error_format: // TODO: Add error logging
error:
  return 1;
}

/*************************************************************************/

maria_declare_plugin(type_test)
{
  MariaDB_FUNCTION_PLUGIN,       // the plugin type
  Item_func_gen_embedding::plugin_descriptor(),
  "GENERATE_EMBEDDING_OPENAI",// plugin name
  "Apostolis Stamatis",        // plugin author
  "Function GENERATE_EMBEDDING_OPENAI()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  system_variables,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL // Maturity
}
maria_declare_plugin_end;

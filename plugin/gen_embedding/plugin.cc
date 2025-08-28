/*
   Copyright (c) 2025, MariaDB Corporation

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
#include <sstream>
#include "sql_type_vector.h"
#include "item_jsonfunc.h"
#include <unordered_map>
#include "json_lib.h"

static char *host, *api_key;
ulonglong curl_requests= 0; /* Number of total curl requests */
ulonglong successful_curl_requests= 0; /* Number of successful curl requests */
static const char *JSON_EMBEDDING_PATH = "$.data[0].embedding";

static MYSQL_THDVAR_STR(host,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "OpenAI API host, can be set to 'https://api.openai.com/v1/embeddings' or a custom endpoint",
  NULL, NULL, "");

static MYSQL_THDVAR_STR(api_key,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "OpenAI API key",
  NULL, NULL, "");

static struct st_mysql_sys_var* system_variables[]= {
  MYSQL_SYSVAR(host),
  MYSQL_SYSVAR(api_key),
  NULL
};

static SHOW_VAR status_variables[]= {
  {"successful_http_requests", (char *) &successful_curl_requests, SHOW_LONGLONG},
  {"total_http_requests", (char *) &curl_requests, SHOW_LONGLONG},
  {NullS, NullS, SHOW_LONG}
};

/*
  Utility functions
*/

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) 
{
  size_t realsize= size * nmemb;
  std::ostringstream *read_data= static_cast<std::ostringstream *>(userp);
  read_data->write(static_cast<char *>(contents), realsize);
  return realsize;
}


class Item_func_gen_embedding: public Item_str_func 
{
  String tmp_js, api_response, tmp_str, post_fields;
  json_path_with_flags *json_path;
  std::unordered_map<std::string, int> model_dimensions= {
    {"text-embedding-3-small", 1536},
    {"text-embedding-ada-002", 1536},
    {"text-embedding-3-large", 3072}
  };

  int make_openai_request()
  {
    /* 
      Make the request to OpenAI API to generate the embedding
      Store the result in api_response
    */
    CHARSET_INFO *cs_openai= &my_charset_utf8mb4_general_ci;
    CURLcode ret;
    CURL *hnd=NULL;
    std::ostringstream read_data_stream;
    struct curl_slist *slist1= NULL;
    long http_response_code;
    size_t max_chars;
    int strLen;
    std::string response, model_name, authorization= std::string("Authorization: Bearer ") + api_key;
    
    // Input for the OpenAI API
    String *input= args[0]->val_str(&api_response);
    if (!input)
    {
      null_value= true;
      return 1;
    }
    // Model for the OpenAI API
    String *model= args[1]->val_str(&tmp_str);
    if (!model) 
    {
      null_value= true;
      return 1;
    }

    model_name= (std::string) args[1]->val_str()->c_ptr();
    /* 
      Before performing the request, check if the model is supported
      This might allow us to avoid a request if the model is not supported
    */
    if (model_dimensions.find(model_name) == model_dimensions.end())
    {
      my_printf_error(1, "GENERATE_EMBEDDING_OPENAI: "
        "Model %s is not supported", ME_WARNING, model_name.c_str());
      null_value= true;
      return 1;
    }
    
    /*
      post_fields contains the JSON string to be passed to CURL
      It is in the format:
      {"input": "escaped_input_str", "model": "model_name", "encoding_format": "float"}
      where escaped_input_str is the input string escaped for JSON and converted to UTF-8
      and model_name is the name of the model to be used for embedding generation.
      We need to allocate enough space for the JSON string
      At worst case, each input character needs to be escaped, hence the 2 multiplier below
    */
    max_chars= (2 * input->length()/input->charset()->mbminlen +
                      model->length()/model->charset()->mbminlen +
                      strlen("{\"input\": \"") + strlen("\", \"model\": \"") +
                      strlen("\",\"encoding_format\": \"float\"}") + 1);
    strLen= max_chars * cs_openai->mbmaxlen;
    post_fields.realloc_with_extra_if_needed(strLen);
    post_fields.length(0);
    post_fields.append(STRING_WITH_LEN("{\"input\": \""));
    /*
      It is essential to escape the input string to ensure the input of the API call is a valid JSON
      json_escape also handles conversion to cs_openai (UTF-8) charset
      The model name does not need escaping as it is a valid string
    */
    if ((strLen= json_escape(input->charset(), (const uchar *) input->ptr(), (const uchar *) input->ptr() + input->length(),
                             cs_openai, (uchar *) post_fields.end(), (uchar *) post_fields.ptr() + strLen)) < 0) 
    {
      if (strLen == JSON_ERROR_ILLEGAL_SYMBOL)
      {
        my_printf_error(1, "GENERATE_EMBEDDING_OPENAI: "
                        "Error converting input string from %s to UTF-8 charset", 
                        ME_ERROR_LOG | ME_WARNING, input->charset()->cs_name.str);
      }
      null_value= true;
      goto cleanup;
    }
    post_fields.length(post_fields.length()+strLen); // We need to manually update the length
    post_fields.append(STRING_WITH_LEN("\", \"model\": \""));
    post_fields.append(model->c_ptr(), model->length());
    post_fields.append(STRING_WITH_LEN("\",\"encoding_format\": \"float\"}"));
    
    // Prepare the request to OpenAI API
    slist1= NULL;
    slist1= curl_slist_append(slist1, authorization.c_str());
    slist1= curl_slist_append(slist1, "Content-Type: application/json; charset=utf-8");
    hnd= curl_easy_init();
    curl_easy_setopt(hnd, CURLOPT_BUFFERSIZE, 102400L);
    curl_easy_setopt(hnd, CURLOPT_URL, host);
    curl_easy_setopt(hnd, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(hnd, CURLOPT_POSTFIELDS, post_fields.ptr());
    curl_easy_setopt(hnd, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)post_fields.length());
    curl_easy_setopt(hnd, CURLOPT_HTTPHEADER, slist1);
    curl_easy_setopt(hnd, CURLOPT_USERAGENT, "curl/8.5.0");
    curl_easy_setopt(hnd, CURLOPT_MAXREDIRS, 50L);
    curl_easy_setopt(hnd, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(hnd, CURLOPT_FTP_SKIP_PASV_IP, 1L);
    curl_easy_setopt(hnd, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(hnd, CURLOPT_WRITEDATA, &read_data_stream);
    
    ret= curl_easy_perform(hnd);
    curl_requests++; // Once we call curl_easy_perform, we count the request
    if (ret != CURLE_OK)
    {
      my_printf_error(1, "GENERATE_EMBEDDING_OPENAI: "
        "curl returned this error code: %u "
        "with the following error message: %s", ME_ERROR_LOG | ME_WARNING, ret,
        curl_easy_strerror(ret));
      goto cleanup;
    }
    curl_easy_getinfo(hnd, CURLINFO_RESPONSE_CODE, &http_response_code);
    if (http_response_code != 200) // The only valid response code for OpenAI API is 200
    {
      // TODO: We could grab the error message from response
      my_printf_error(1, "GENERATE_EMBEDDING_OPENAI: "
        "Bad http response code: %lu", ME_ERROR_LOG | ME_WARNING, http_response_code); 
      goto cleanup;
    }
    successful_curl_requests++; // If we reach this point, the request was successful
    curl_easy_cleanup(hnd);
    hnd= NULL;
    curl_slist_free_all(slist1);
    slist1= NULL;
    response= read_data_stream.str();

    api_response.copy(response.c_str(), response.length(), &my_charset_utf8mb4_general_ci);
    return 0;
cleanup:
    curl_easy_cleanup(hnd);
    hnd= NULL;
    curl_slist_free_all(slist1);
    slist1= NULL;
    return 1;
  }

  
  int parse_vector(String *buf, json_engine_t je, CHARSET_INFO *cs, const uchar *start, const uchar *end);
  String *read_json(String *str, json_value_types *type, char **out_val);

  
public:
  using Item_str_func::Item_str_func;

  bool is_expensive() override { return true; }

  bool fix_length_and_dec(THD *thd) override
  {
    host= THDVAR(thd, host);
    api_key= THDVAR(thd, api_key);
    std::string model_name;
    uint max_dimensions= 3072; // Default to the largest embedding size
    if (args[1]->const_item() && !args[1]->is_null()) // If a const, we can parse the model name here and determine the max dimensions
    {
      model_name= (std::string) args[1]->val_str()->c_ptr();
      if (model_dimensions.find(args[1]->val_str()->c_ptr()) != model_dimensions.end()) 
      {
        max_dimensions= model_dimensions[model_name];
      }
      else
      {
        // TODO: Can we also early exit here?
        my_printf_error(1, "GENERATE_EMBEDDING_OPENAI: "
          "Model %s is not supported", ME_ERROR_LOG | ME_WARNING, model_name.c_str());
      }
    }

    decimals= 0;
    MEM_ROOT *root= thd->mem_root;
    json_path= (json_path_with_flags *) alloc_root(root, sizeof(json_path_with_flags));
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
    Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
    {
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
  
  if ((null_value= args[0]->null_value))
    return 0;

  json_path->p.types_used= JSON_PATH_KEY_NULL;
  const uchar *s_p= (const uchar *) JSON_EMBEDDING_PATH; 
  if (json_path_setup(&json_path->p,&my_charset_utf8mb4_general_ci, s_p,
                    s_p + strlen((const char *) s_p)))
    goto return_null;

  *type= JSON_VALUE_NULL;

  // str will never be NULL here, since it comes from val_str()
  str->set_charset(&my_charset_bin);
  str->length(0);

  json_get_path_start(&je, js->charset(),(const uchar *) js->ptr(),
                      (const uchar *) js->ptr() + js->length(), &p);

  while (json_get_path_next(&je, &p) == 0)
  {

    if (!(count_path= path_exact(json_path, arg_count-1, &p, je.value_type,
                                 array_size_counter)))
      continue;

    value= je.value_begin;

    if (*type == JSON_VALUE_NULL)
    {
      *type= je.value_type;
      *out_val= (char *) je.value;
      // value_len= je.value_len;
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

  if (unlikely(je.s.error))
    goto error;

  if (!not_first_value)
  {
    /* Nothing was found. */
    goto return_null;
  }

error:
  report_json_error_ex(js->ptr(), &je, func_name(), 0, Sql_condition::WARN_LEVEL_WARN);
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
    The vector being parsed is a JSON array of numbers
    The output vector is in binary format, will be stored in buf by this function
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
        char *start= (char *) je.value_begin, *end;
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
  MariaDB_FUNCTION_PLUGIN,                      // the plugin type
  Item_func_gen_embedding::plugin_descriptor(),
  "GENERATE_EMBEDDING_OPENAI",                  // plugin name
  "Apostolis Stamatis",                         // plugin author
  "Function GENERATE_EMBEDDING_OPENAI()",       // the plugin description
  PLUGIN_LICENSE_GPL,                           // the plugin license
  0,                                            // Pointer to plugin initialization function
  0,                                            // Pointer to plugin deinitialization function
  0x0100,                                       // Numeric version 0xAABB means AA.BB version
  status_variables,                             // Status variables
  system_variables,                             // System variables
  "1.0",                                        // String version representation
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL          // Maturity
}
maria_declare_plugin_end;

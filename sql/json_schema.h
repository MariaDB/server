#ifndef JSON_SCHEMA_INCLUDED
#define JSON_SCHEMA_INCLUDED

/* Copyright (c) 2016, 2021, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


/* This file defines all json schema classes. */

#include "sql_class.h"
#include "sql_type_json.h"
#include "json_schema_helper.h"

struct st_json_schema_keyword_map;

class Json_schema_keyword : public Sql_alloc
{
  public:
    Json_schema_keyword *alternate_schema;
    st_json_schema_keyword_map *keyword_map;
    double value;
    uint priority;
    bool allowed;

    Json_schema_keyword() : alternate_schema(NULL), keyword_map(NULL),
                            value(0), priority(0), allowed(true)
    {
    }
    virtual ~Json_schema_keyword() = default;

    /*
     Called for each keyword on the current level.
    */
    virtual bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                          const uchar *k_end= NULL)
    { return false; }
    virtual bool handle_keyword(THD *thd, json_engine_t *je,
                                const char* key_start,
                                const char* key_end,
                                List<Json_schema_keyword> *all_keywords)
    {
      return false;
    }
    virtual List<Json_schema_keyword>* get_validation_keywords()
    {
      return NULL;
    }
    void set_alternate_schema(Json_schema_keyword *schema)
    {
      alternate_schema= schema;
    }
    virtual bool fall_back_on_alternate_schema(const json_engine_t *je,
                                               const uchar* k_start= NULL,
                                               const uchar* k_end= NULL);
    virtual bool validate_as_alternate(const json_engine_t *je,
                                               const uchar* k_start= NULL,
                                               const uchar* k_end= NULL)
    {
      return false;
    }
    virtual bool validate_schema_items(const json_engine_t *je,
                                       List<Json_schema_keyword>*schema_items);
    virtual void set_alternate_schema_choice(Json_schema_keyword *schema1,
                                             Json_schema_keyword *schema2)
    {
      return;
    }
    virtual void set_dependents(Json_schema_keyword *schema1,
                                Json_schema_keyword *schema2)
    {
      return;
    }
};

/*
  Additional and unvaluated keywords and items handle
  keywords and validate schema in same way, so it makes sense
  to have a base class for them.
*/
class Json_schema_additional_and_unevaluated : public Json_schema_keyword
{
  public:
    List<Json_schema_keyword> schema_list;
    Json_schema_additional_and_unevaluated()
    {
      allowed= true;
    }
    void set_allowed(bool allowed_val)
    {
      allowed= allowed_val;
    }
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
    bool validate(const json_engine_t *je,
                  const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override
    {
      return false;
    }
    bool validate_as_alternate(const json_engine_t *je, const uchar *k_start,
                               const uchar *k_end) override;
};


class Json_schema_annotation : public Json_schema_keyword
{
  public:
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_format : public Json_schema_keyword
{
  public:
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

typedef List<Json_schema_keyword> List_schema_keyword;

class Json_schema_type : public Json_schema_keyword
{
  private:
    uint type;

  public:
    bool validate(const json_engine_t *je,
                  const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd,
                        json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
    Json_schema_type()
    {
      type= 0;
    }
};

class Json_schema_const : public Json_schema_keyword
{
  private:
    char *const_json_value;

  public:
    enum json_value_types type;
    bool validate(const json_engine_t *je,
                  const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
    Json_schema_const()
    {
      const_json_value= NULL;
    }
};


class Json_schema_enum : public  Json_schema_keyword
{
  private:
    HASH enum_values;
    uint enum_scalar;

  public:
    bool validate(const json_engine_t *je,
                  const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
    Json_schema_enum()
    {
      enum_scalar= 0;
    }
    ~Json_schema_enum()
    {
      my_hash_free(&enum_values);
    }
};

class Json_schema_maximum : public Json_schema_keyword
{
  public:
    bool validate(const json_engine_t *je,
                  const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_minimum : public Json_schema_keyword
{
  public:
    bool validate(const json_engine_t *je,
                  const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_multiple_of : public Json_schema_keyword
{
  private:
    longlong multiple_of;

  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_ex_maximum : public Json_schema_keyword
{
  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_ex_minimum : public Json_schema_keyword
{
  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_max_len : public Json_schema_keyword
{
  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_min_len : public Json_schema_keyword
{
  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_pattern : public Json_schema_keyword
{
  private:
    Regexp_processor_pcre re;
    Item *pattern;
    Item_string *str;

  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
    Json_schema_pattern()
    {
      str= NULL;
      pattern= NULL;
    }
    ~Json_schema_pattern() { re.cleanup(); }
};

class Json_schema_max_items : public Json_schema_keyword
{
  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_min_items : public Json_schema_keyword
{
  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_max_contains : public Json_schema_keyword
{
  public:
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_min_contains : public Json_schema_keyword
{
  public:
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};
/*
  The value of max_contains and min_contains is only
  relevant when contains keyword is present.
  Hence the pointers to access them directly.
*/
class Json_schema_contains : public Json_schema_keyword
{
  public:
    List <Json_schema_keyword> contains;
    Json_schema_keyword *max_contains;
    Json_schema_keyword *min_contains;

    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
    void set_dependents(Json_schema_keyword *min, Json_schema_keyword *max) override
    {
      min_contains= min;
      max_contains= max;
    }
};

class Json_schema_unique_items : public Json_schema_keyword
{
  private:
    bool is_unique;

  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};


class Json_schema_prefix_items : public Json_schema_keyword
{
  public:
    List <List_schema_keyword> prefix_items;
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
    Json_schema_prefix_items()
    {
      priority= 1;
    }
};

class Json_schema_unevaluated_items :
      public Json_schema_additional_and_unevaluated
{
  public:
    Json_schema_unevaluated_items()
    {
      priority= 4;
    }
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
};

class Json_schema_additional_items :
      public Json_schema_additional_and_unevaluated
{
  public:
    Json_schema_additional_items()
    {
      priority= 3;
    }
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
};

class Json_schema_items : public Json_schema_keyword
{
  private:
    List<Json_schema_keyword> items_schema;
  public:
    Json_schema_items()
    {
      priority= 2;
    }
    void set_allowed(bool allowed_val) { allowed= allowed_val; }
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
    bool validate_as_alternate(const json_engine_t *je, const uchar *k_start,
                           const uchar *k_end) override;
};


class Json_schema_property_names : public Json_schema_keyword
{
  protected:
    List <Json_schema_keyword> property_names;

  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

typedef struct property
{
  List<Json_schema_keyword> *curr_schema;
  char *key_name;
} st_property;

class Json_schema_properties : public Json_schema_keyword
{
  private:
    HASH properties;
    bool is_hash_inited;

  public:
    Json_schema_properties()
    {
      priority= 1;
    }
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
    ~Json_schema_properties()
    {
      if (is_hash_inited)
        my_hash_free(&properties);
    }
    bool validate_as_alternate(const json_engine_t *je, const uchar *k_start,
                               const uchar *k_end) override;
  };

class Json_schema_dependent_schemas : public Json_schema_keyword
{
  private:
    HASH properties;
    bool is_hash_inited;

  public:
    ~Json_schema_dependent_schemas()
    {
      if (is_hash_inited)
       my_hash_free(&properties);
    }
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};


class Json_schema_additional_properties :
      public Json_schema_additional_and_unevaluated
{
  public:
    Json_schema_additional_properties()
    {
      priority= 3;
    }
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
};

class Json_schema_unevaluated_properties :
      public Json_schema_additional_and_unevaluated
{
  public:
    Json_schema_unevaluated_properties()
    {
      priority= 4;
    }
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
};

typedef struct pattern_to_property : public Sql_alloc
{
  Regexp_processor_pcre re;
  Item *pattern;
  List<Json_schema_keyword> *curr_schema;
}st_pattern_to_property;

class Json_schema_pattern_properties : public Json_schema_keyword
{
  private:
    Item_string *str;
    List<st_pattern_to_property> pattern_properties;

  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
    Json_schema_pattern_properties()
    {
      priority= 2;
    }
    ~Json_schema_pattern_properties()
    {
      str= NULL;
      if (!pattern_properties.is_empty())
      {
        st_pattern_to_property *curr_pattern_to_property= NULL;
        List_iterator<st_pattern_to_property> it(pattern_properties);
        while((curr_pattern_to_property= it++))
        {
          curr_pattern_to_property->re.cleanup();
          curr_pattern_to_property->pattern= NULL;
          delete curr_pattern_to_property;
          curr_pattern_to_property= nullptr;
        }
      }
    }
    bool validate_as_alternate(const json_engine_t *je, const uchar *k_start,
                               const uchar *k_end) override;
};


class Json_schema_max_prop : public Json_schema_keyword
{
  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_min_prop : public Json_schema_keyword
{
  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_required : public Json_schema_keyword
{
  private:
    List <String> required_properties;

  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

typedef struct dependent_keyowrds
{
  String *property;
  List <String> dependents;
} st_dependent_keywords;

class Json_schema_dependent_required : public Json_schema_keyword
{
  private:
    List<st_dependent_keywords> dependent_required;

  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

enum logic_enum { HAS_ALL_OF= 2, HAS_ANY_OF= 4, HAS_ONE_OF= 8, HAS_NOT= 16};
class Json_schema_logic : public Json_schema_keyword
{
  protected:
    uint logic_flag;
    List <List_schema_keyword> schema_items;
    Json_schema_keyword *alternate_choice1, *alternate_choice2;
  public:
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
    Json_schema_logic()
    {
      logic_flag= 0;
      alternate_choice1= alternate_choice2= NULL;
      priority= 1;
    }
    virtual bool validate_count(uint* count, uint* total) { return false; }
    void set_alternate_schema_choice(Json_schema_keyword *schema1,
                                     Json_schema_keyword* schema2) override
    {
      alternate_choice1= schema1;
      alternate_choice2= schema2;
    }
    bool check_validation(const json_engine_t *je, const uchar *k_start= NULL,
                          const uchar *k_end= NULL);
};

class Json_schema_not : public Json_schema_logic
{
  private:
  List <Json_schema_keyword> schema_list;
  public:
    Json_schema_not()
    {
      logic_flag= HAS_NOT;
    }
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
    bool validate_count(uint *count, uint *total) override
    {
      return *count !=0;
    }
};

class Json_schema_one_of : public Json_schema_logic
{
  public:
    Json_schema_one_of()
    {
      logic_flag= HAS_ONE_OF;
    }
    bool validate_count(uint *count, uint *total) override
    {
      return !(*count == 1);
    }
};

class Json_schema_any_of : public Json_schema_logic
{
  public:
    Json_schema_any_of()
    {
      logic_flag= HAS_ANY_OF;
    }
    bool validate_count(uint *count, uint *total) override
    {
      return *count == 0;
    }
};

class Json_schema_all_of : public Json_schema_logic
{
  public:
    Json_schema_all_of()
    {
      logic_flag= HAS_ALL_OF;
    }
    bool validate_count(uint *count, uint *total) override
    {
      return *count != *total;
    }
};

class Json_schema_conditional : public Json_schema_keyword
{
  private:
    Json_schema_keyword *if_cond, *else_cond, *then_cond;

  public:
    List<Json_schema_keyword> conditions_schema;
    Json_schema_conditional()
    {
      if_cond= NULL;
      then_cond= NULL;
      else_cond= NULL;
    }
    bool validate(const json_engine_t *je, const uchar *k_start= NULL,
                  const uchar *k_end= NULL) override;
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
    void set_conditions(Json_schema_keyword *if_val,
                        Json_schema_keyword* then_val,
                        Json_schema_keyword *else_val)
    {
      if_cond= if_val;
      then_cond= then_val;
      else_cond= else_val;
    }
    List<Json_schema_keyword>* get_validation_keywords() override
    {
      return &conditions_schema;
    }

};

class Json_schema_if : public Json_schema_conditional
{
};

class Json_schema_else : public Json_schema_conditional
{
};

class Json_schema_then : public Json_schema_conditional
{
};

class Json_schema_media_string : public Json_schema_keyword
{
  public:
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

class Json_schema_reference : public Json_schema_keyword
{
  public:
    bool handle_keyword(THD *thd, json_engine_t *je,
                        const char* key_start,
                        const char* key_end,
                        List<Json_schema_keyword> *all_keywords) override;
};

bool create_object_and_handle_keyword(THD *thd, json_engine_t *je,
                                      List<Json_schema_keyword> *keyword_list,                           
                                      List<Json_schema_keyword> *all_keywords);
uchar* get_key_name_for_property(const char *key_name, size_t *length,
                    my_bool /* unused */);
uchar* get_key_name_for_func(const char *key_name, size_t *length,
                    my_bool /* unused */);

enum keyword_flag
{
  JSON_SCHEMA_COMMON_KEYWORD= 0,
  JSON_SCHEMA_NUMBER_KEYWORD= 1,
  JSON_SCHEMA_STRING_KEYWORD= 2,
  JSON_SCHEMA_ARRAY_KEYWORD= 3,
  JSON_SCHEMA_OBJECT_KEYWORD= 4,
  JSON_SCHEMA_LOGIC_KEYWORD= 5,
  JSON_SCHEMA_CONDITION_KEYWORD= 6,
  JSON_SCHEMA_ANNOTATION_KEYWORD= 7,
  JSON_SCHEMA_FORMAT_KEYWORD= 8,
  JSON_SCHEMA_MEDIA_KEYWORD= 9,
  JSON_SCHEMA_REFERENCE_KEYWORD= 10,
  JSON_SCHEMA_EMPTY_KEYWORD= 11
};

typedef struct st_json_schema_keyword_map
{
  LEX_CSTRING func_name;
  Json_schema_keyword*(*func)(THD*);
  enum keyword_flag flag;
} json_schema_keyword_map;

bool setup_json_schema_keyword_hash();
void cleanup_json_schema_keyword_hash();

#endif

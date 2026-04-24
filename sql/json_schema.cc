/* Copyright (c) 2016, 2022, MariaDB Corporation.

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


#include "mariadb.h"
#include "sql_class.h"
#include "sql_parse.h" // For check_stack_overrun
#include <m_string.h>
#include "json_schema.h"
#include "json_schema_helper.h"
#define PCRE2_STATIC 1
#include "pcre2.h"
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#ifndef DBUG_OFF
int dbug_json_check_min_stack_requirement();
#endif

static HASH all_keywords_hash;

static Json_schema_keyword *create_json_schema_keyword(THD *thd)
{
   return new (thd->mem_root) Json_schema_keyword();
}
static Json_schema_keyword *create_json_schema_type(THD *thd)
{
  return new (thd->mem_root) Json_schema_type();
}
static Json_schema_keyword *create_json_schema_enum(THD *thd)
{
  return new (thd->mem_root) Json_schema_enum();
}
static Json_schema_keyword *create_json_schema_const(THD *thd)
{
   return new (thd->mem_root) Json_schema_const();
}
static Json_schema_keyword *create_json_schema_maximum(THD *thd)
{
   return new (thd->mem_root) Json_schema_maximum();
}
static Json_schema_keyword *create_json_schema_minimum(THD *thd)
{
   return new (thd->mem_root) Json_schema_minimum();
}
static Json_schema_keyword *create_json_schema_ex_max(THD *thd)
{
   return new (thd->mem_root) Json_schema_ex_maximum();
}
static Json_schema_keyword *create_json_schema_ex_min(THD *thd)
{
   return new (thd->mem_root) Json_schema_ex_minimum();
}
static Json_schema_keyword *create_json_schema_multiple_of(THD *thd)
{
   return new (thd->mem_root) Json_schema_multiple_of();
}
static Json_schema_keyword *create_json_schema_max_len(THD *thd)
{
   return new (thd->mem_root) Json_schema_max_len();
}
static Json_schema_keyword *create_json_schema_min_len(THD *thd)
{
   return new (thd->mem_root) Json_schema_min_len();
}
static Json_schema_keyword *create_json_schema_pattern(THD *thd)
{
   return new (thd->mem_root) Json_schema_pattern();
}
static Json_schema_keyword *create_json_schema_items(THD *thd)
{
   return new (thd->mem_root) Json_schema_items();
}
static Json_schema_keyword *create_json_schema_max_items(THD *thd)
{
   return new (thd->mem_root) Json_schema_max_items();
}
static Json_schema_keyword *create_json_schema_min_items(THD *thd)
{
   return new (thd->mem_root) Json_schema_min_items();
}
static Json_schema_keyword *create_json_schema_prefix_items(THD *thd)
{
   return new (thd->mem_root) Json_schema_prefix_items();
}
static Json_schema_keyword *create_json_schema_contains(THD *thd)
{
   return new (thd->mem_root) Json_schema_contains();
}
static Json_schema_keyword *create_json_schema_max_contains(THD *thd)
{
   return new (thd->mem_root) Json_schema_max_contains();
}
static Json_schema_keyword *create_json_schema_min_contains(THD *thd)
{
   return new (thd->mem_root) Json_schema_min_contains();
}
static Json_schema_keyword *create_json_schema_unique_items(THD *thd)
{
   return new (thd->mem_root) Json_schema_unique_items();
}
static Json_schema_keyword *create_json_schema_additional_items(THD *thd)
{
   return new (thd->mem_root) Json_schema_additional_items();
}
static Json_schema_keyword *create_json_schema_unevaluated_items(THD *thd)
{
   return new (thd->mem_root) Json_schema_unevaluated_items();
}
static Json_schema_keyword *create_json_schema_properties(THD *thd)
{
   return new (thd->mem_root) Json_schema_properties();
}
static Json_schema_keyword *create_json_schema_pattern_properties(THD *thd)
{
   return new (thd->mem_root) Json_schema_pattern_properties();
}
static Json_schema_keyword *create_json_schema_additional_properties(THD *thd)
{
   return new (thd->mem_root) Json_schema_additional_properties();
}
static Json_schema_keyword *create_json_schema_unevaluated_properties(THD *thd)
{
   return new (thd->mem_root) Json_schema_unevaluated_properties();
}
static Json_schema_keyword *create_json_schema_property_names(THD *thd)
{
   return new (thd->mem_root) Json_schema_property_names();
}
static Json_schema_keyword *create_json_schema_max_prop(THD *thd)
{
   return new (thd->mem_root) Json_schema_max_prop();
}
static Json_schema_keyword *create_json_schema_min_prop(THD *thd)
{
   return new (thd->mem_root) Json_schema_min_prop();
}
static Json_schema_keyword *create_json_schema_required(THD *thd)
{
   return new (thd->mem_root) Json_schema_required();
}
static Json_schema_keyword *create_json_schema_dependent_required(THD *thd)
{
   return new (thd->mem_root) Json_schema_dependent_required();
}
static Json_schema_keyword *create_json_schema_dependent_schemas(THD *thd)
{
   return new (thd->mem_root) Json_schema_dependent_schemas();
}
static Json_schema_keyword *create_json_schema_not(THD *thd)
{
   return new (thd->mem_root) Json_schema_not();
}
static Json_schema_keyword *create_json_schema_all_of(THD *thd)
{
   return new (thd->mem_root) Json_schema_all_of();
}
static Json_schema_keyword *create_json_schema_any_of(THD *thd)
{
   return new (thd->mem_root) Json_schema_any_of();
}
static Json_schema_keyword *create_json_schema_one_of(THD *thd)
{
   return new (thd->mem_root) Json_schema_one_of();
}
static Json_schema_keyword *create_json_schema_if(THD *thd)
{
   return new (thd->mem_root) Json_schema_if();
}
static Json_schema_keyword *create_json_schema_then(THD *thd)
{
   return new (thd->mem_root) Json_schema_then();
}
static Json_schema_keyword *create_json_schema_else(THD *thd)
{
   return new (thd->mem_root) Json_schema_else();
}
static Json_schema_keyword *create_json_schema_annotation(THD *thd)
{
   return new (thd->mem_root) Json_schema_annotation();
}
static Json_schema_keyword *create_json_schema_format(THD *thd)
{
   return new (thd->mem_root) Json_schema_format();
}
static Json_schema_keyword *create_json_schema_media_string(THD *thd)
{
   return new (thd->mem_root) Json_schema_media_string();
}
static Json_schema_keyword *create_json_schema_reference(THD *thd)
{
  return new (thd->mem_root) Json_schema_reference();
}

 static st_json_schema_keyword_map json_schema_func_array[]=
{
  {{ STRING_WITH_LEN("type") },  create_json_schema_type, JSON_SCHEMA_COMMON_KEYWORD},
  {{ STRING_WITH_LEN("const") },  create_json_schema_const, JSON_SCHEMA_COMMON_KEYWORD},
  {{ STRING_WITH_LEN("enum") },  create_json_schema_enum, JSON_SCHEMA_COMMON_KEYWORD},

  {{ STRING_WITH_LEN("maximum") },  create_json_schema_maximum, JSON_SCHEMA_NUMBER_KEYWORD},
  {{ STRING_WITH_LEN("minimum") },  create_json_schema_minimum, JSON_SCHEMA_NUMBER_KEYWORD},
  {{ STRING_WITH_LEN("exclusiveMaximum") },  create_json_schema_ex_max, JSON_SCHEMA_NUMBER_KEYWORD},
  {{ STRING_WITH_LEN("exclusiveMinimum") },  create_json_schema_ex_min, JSON_SCHEMA_NUMBER_KEYWORD},
  {{ STRING_WITH_LEN("multipleOf") },  create_json_schema_multiple_of, JSON_SCHEMA_NUMBER_KEYWORD},

  {{ STRING_WITH_LEN("maxLength") },  create_json_schema_max_len, JSON_SCHEMA_STRING_KEYWORD},
  {{ STRING_WITH_LEN("minLength") },  create_json_schema_min_len, JSON_SCHEMA_STRING_KEYWORD},
  {{ STRING_WITH_LEN("pattern") },  create_json_schema_pattern, JSON_SCHEMA_STRING_KEYWORD},

  {{ STRING_WITH_LEN("items") },  create_json_schema_items, JSON_SCHEMA_ARRAY_KEYWORD},
  {{ STRING_WITH_LEN("maxItems") },  create_json_schema_max_items, JSON_SCHEMA_ARRAY_KEYWORD},
  {{ STRING_WITH_LEN("minItems") },  create_json_schema_min_items, JSON_SCHEMA_ARRAY_KEYWORD},
  {{ STRING_WITH_LEN("additionalItems") },  create_json_schema_additional_items, JSON_SCHEMA_ARRAY_KEYWORD},
  {{ STRING_WITH_LEN("unevaluatedItems") },  create_json_schema_unevaluated_items, JSON_SCHEMA_ARRAY_KEYWORD},
  {{ STRING_WITH_LEN("prefixItems") },  create_json_schema_prefix_items, JSON_SCHEMA_ARRAY_KEYWORD},
  {{ STRING_WITH_LEN("uniqueItems") },  create_json_schema_unique_items, JSON_SCHEMA_ARRAY_KEYWORD},
  {{ STRING_WITH_LEN("contains") },  create_json_schema_contains, JSON_SCHEMA_ARRAY_KEYWORD},
  {{ STRING_WITH_LEN("maxContains") },  create_json_schema_max_contains, JSON_SCHEMA_ARRAY_KEYWORD},
  {{ STRING_WITH_LEN("minContains") },  create_json_schema_min_contains, JSON_SCHEMA_ARRAY_KEYWORD},

  {{ STRING_WITH_LEN("properties") },  create_json_schema_properties, JSON_SCHEMA_OBJECT_KEYWORD},
  {{ STRING_WITH_LEN("patternProperties") },  create_json_schema_pattern_properties, JSON_SCHEMA_OBJECT_KEYWORD},
  {{ STRING_WITH_LEN("propertyNames") },  create_json_schema_property_names, JSON_SCHEMA_OBJECT_KEYWORD},
  {{ STRING_WITH_LEN("maxProperties") },  create_json_schema_max_prop, JSON_SCHEMA_OBJECT_KEYWORD},
  {{ STRING_WITH_LEN("minProperties") },  create_json_schema_min_prop, JSON_SCHEMA_OBJECT_KEYWORD},
  {{ STRING_WITH_LEN("dependentRequired") },  create_json_schema_dependent_required, JSON_SCHEMA_OBJECT_KEYWORD},
  {{ STRING_WITH_LEN("dependentSchemas") },  create_json_schema_dependent_schemas, JSON_SCHEMA_OBJECT_KEYWORD},
  {{ STRING_WITH_LEN("required") },  create_json_schema_required, JSON_SCHEMA_OBJECT_KEYWORD},
  {{ STRING_WITH_LEN("additionalProperties") },  create_json_schema_additional_properties, JSON_SCHEMA_OBJECT_KEYWORD},
  {{ STRING_WITH_LEN("unevaluatedProperties") },  create_json_schema_unevaluated_properties, JSON_SCHEMA_OBJECT_KEYWORD},

  {{ STRING_WITH_LEN("not") },  create_json_schema_not, JSON_SCHEMA_LOGIC_KEYWORD},
  {{ STRING_WITH_LEN("allOf") },  create_json_schema_all_of, JSON_SCHEMA_LOGIC_KEYWORD},
  {{ STRING_WITH_LEN("anyOf") },  create_json_schema_any_of, JSON_SCHEMA_LOGIC_KEYWORD},
  {{ STRING_WITH_LEN("oneOf") },  create_json_schema_one_of, JSON_SCHEMA_LOGIC_KEYWORD},

  {{ STRING_WITH_LEN("if") },  create_json_schema_if, JSON_SCHEMA_CONDITION_KEYWORD},
  {{ STRING_WITH_LEN("then") },  create_json_schema_then, JSON_SCHEMA_CONDITION_KEYWORD},
  {{ STRING_WITH_LEN("else") },  create_json_schema_else, JSON_SCHEMA_CONDITION_KEYWORD},

  {{ STRING_WITH_LEN("title") },  create_json_schema_annotation, JSON_SCHEMA_ANNOTATION_KEYWORD},
  {{ STRING_WITH_LEN("description") },  create_json_schema_annotation, JSON_SCHEMA_ANNOTATION_KEYWORD},
  {{ STRING_WITH_LEN("comment") },  create_json_schema_annotation, JSON_SCHEMA_ANNOTATION_KEYWORD},
  {{ STRING_WITH_LEN("$schema") },  create_json_schema_annotation, JSON_SCHEMA_ANNOTATION_KEYWORD},
  {{ STRING_WITH_LEN("deprecated") }, create_json_schema_annotation, JSON_SCHEMA_ANNOTATION_KEYWORD},
  {{ STRING_WITH_LEN("readOnly") }, create_json_schema_annotation, JSON_SCHEMA_ANNOTATION_KEYWORD},
  {{ STRING_WITH_LEN("writeOnly") }, create_json_schema_annotation, JSON_SCHEMA_ANNOTATION_KEYWORD},
  {{ STRING_WITH_LEN("example") }, create_json_schema_annotation, JSON_SCHEMA_ANNOTATION_KEYWORD},
  {{ STRING_WITH_LEN("default") }, create_json_schema_annotation, JSON_SCHEMA_ANNOTATION_KEYWORD},
  {{ STRING_WITH_LEN("$vocabulary") }, create_json_schema_annotation, JSON_SCHEMA_ANNOTATION_KEYWORD},

  {{ STRING_WITH_LEN("format") }, create_json_schema_format, JSON_SCHEMA_FORMAT_KEYWORD},

  {{ STRING_WITH_LEN("contentMediaType") }, create_json_schema_media_string, JSON_SCHEMA_MEDIA_KEYWORD},
  {{ STRING_WITH_LEN("conentEncoding") }, create_json_schema_media_string, JSON_SCHEMA_MEDIA_KEYWORD},
  {{ STRING_WITH_LEN("contentSchema") }, create_json_schema_media_string, JSON_SCHEMA_MEDIA_KEYWORD},

  {{ STRING_WITH_LEN("$ref") }, create_json_schema_reference, JSON_SCHEMA_REFERENCE_KEYWORD},
  {{ STRING_WITH_LEN("$id") }, create_json_schema_reference, JSON_SCHEMA_REFERENCE_KEYWORD},
  {{ STRING_WITH_LEN("$anchor") }, create_json_schema_reference, JSON_SCHEMA_REFERENCE_KEYWORD},
  {{ STRING_WITH_LEN("$defs") }, create_json_schema_reference, JSON_SCHEMA_REFERENCE_KEYWORD},
  {{ STRING_WITH_LEN("$dynamicRef") }, create_json_schema_reference, JSON_SCHEMA_REFERENCE_KEYWORD},
  {{ STRING_WITH_LEN("$dynamicAnchor") }, create_json_schema_reference, JSON_SCHEMA_REFERENCE_KEYWORD},
};

static st_json_schema_keyword_map empty_func_map=
       {{ STRING_WITH_LEN("") }, create_json_schema_keyword, JSON_SCHEMA_EMPTY_KEYWORD};

/*
  When some schemas dont validate, we want to check the annotation
  for an alternate schema. Example, when we have "properties" and
  "patternProperties", if "properties" does not validate for a certain
  keyname, then we want to check if it validates for "patternProperties".
  In this case "patterProperties" will be alternate schema for "properties".
*/
bool Json_schema_keyword::
                    fall_back_on_alternate_schema(const json_engine_t *je,
                                                  MEM_ROOT *current_mem_root,
                                                  const uchar* k_start,
                                                  const uchar* k_end)
{
  if (alternate_schema)
  {
    if (alternate_schema->allowed)
    {
      if (alternate_schema->validate_as_alternate(je, k_start, k_end,
                                                  current_mem_root))
        return true;
    }
    else
      return true;
  }
  return false;
}

bool Json_schema_annotation::handle_keyword(THD *thd,
                                            MEM_ROOT *current_mem_root,
                                            json_engine_t *je,
                                            const char* key_start,
                                            const char* key_end,
                                            List<Json_schema_keyword>
                                                 *all_keywords)
{
  bool is_invalid_value_type= false, res= false;

  if (this->keyword_map == &(json_schema_func_array[38]) ||
      this->keyword_map == &(json_schema_func_array[39]) ||
      this->keyword_map == &(json_schema_func_array[40]) ||
      this->keyword_map == &(json_schema_func_array[41]))
  {
    if (je->value_type != JSON_VALUE_STRING)
      is_invalid_value_type= true;
  }
  else if (this->keyword_map == &(json_schema_func_array[42]) ||
           this->keyword_map == &(json_schema_func_array[43]) ||
           this->keyword_map == &(json_schema_func_array[44]))
  {
    if (je->value_type != JSON_VALUE_TRUE &&
        je->value_type != JSON_VALUE_FALSE)
      is_invalid_value_type= true;
  }
  else if (this->keyword_map == &(json_schema_func_array[45]))
  {
    if (je->value_type != JSON_VALUE_ARRAY)
      is_invalid_value_type= true;
    if (json_skip_level(je))
      return true;
  }
  else if (this->keyword_map == &(json_schema_func_array[46]))
    return false;

  if (is_invalid_value_type)
  {
    res= true;
    String keyword(0);
    keyword.append((const char*)key_start, (int)(key_end-key_start));
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), keyword.ptr());
  }
  return res;
}

bool Json_schema_format::handle_keyword(THD *thd,
                                        MEM_ROOT *current_mem_root,
                                        json_engine_t *je,
                                        const char* key_start,
                                        const char* key_end,
                                        List<Json_schema_keyword>
                                             *all_keywords)
{
  if (je->value_type != JSON_VALUE_STRING)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "format");
    return true;
  }

  static const struct
  {
    LEX_CSTRING name;
    enum json_schema_format_type type;
  } formats[]=
  {
    { { STRING_WITH_LEN("date-time") }, JSON_SCHEMA_FORMAT_DATETIME },
    { { STRING_WITH_LEN("date") }, JSON_SCHEMA_FORMAT_DATE },
    { { STRING_WITH_LEN("time") }, JSON_SCHEMA_FORMAT_TIME },
    { { STRING_WITH_LEN("duration") }, JSON_SCHEMA_FORMAT_DURATION },
    { { STRING_WITH_LEN("email") }, JSON_SCHEMA_FORMAT_EMAIL },
    { { STRING_WITH_LEN("idn-email") }, JSON_SCHEMA_FORMAT_IDN_EMAIL },
    { { STRING_WITH_LEN("hostname") }, JSON_SCHEMA_FORMAT_HOSTNAME },
    { { STRING_WITH_LEN("idn-hostname") }, JSON_SCHEMA_FORMAT_IDN_HOSTNAME },
    { { STRING_WITH_LEN("ipv4") }, JSON_SCHEMA_FORMAT_IPV4 },
    { { STRING_WITH_LEN("ipv6") }, JSON_SCHEMA_FORMAT_IPV6 },
    { { STRING_WITH_LEN("uri") }, JSON_SCHEMA_FORMAT_URI },
    { { STRING_WITH_LEN("uri-reference") }, JSON_SCHEMA_FORMAT_URI_REFERENCE },
    { { STRING_WITH_LEN("iri") }, JSON_SCHEMA_FORMAT_IRI },
    { { STRING_WITH_LEN("iri-reference") }, JSON_SCHEMA_FORMAT_IRI_REFERENCE },
    { { STRING_WITH_LEN("uuid") }, JSON_SCHEMA_FORMAT_UUID },
    { { STRING_WITH_LEN("json-pointer") }, JSON_SCHEMA_FORMAT_JSON_POINTER },
    { { STRING_WITH_LEN("relative-json-pointer") },
      JSON_SCHEMA_FORMAT_RELATIVE_JSON_POINTER },
    { { STRING_WITH_LEN("regex") }, JSON_SCHEMA_FORMAT_REGEX }
  };

  const char *val= (const char *) je->value;
  size_t len= je->value_len;

  format_type= JSON_SCHEMA_FORMAT_NONE;
  for (size_t i= 0; i < array_elements(formats); i++)
  {
    if (len == formats[i].name.length &&
        !memcmp(val, formats[i].name.str, len))
    {
      format_type= formats[i].type;
      break;
    }
  }

  return false;
}

static inline bool is_digit(char c)
{
  return my_isdigit(&my_charset_latin1, c);
}

static inline bool is_hex(char c)
{
  return my_isxdigit(&my_charset_latin1, c);
}

static inline bool is_alpha(char c)
{
  return my_isalpha(&my_charset_latin1, c);
}

static int parse_2digits(const char *s)
{
  if (!is_digit(s[0]) || !is_digit(s[1]))
    return -1;
  return (s[0] - '0') * 10 + (s[1] - '0');
}

static int parse_4digits(const char *s)
{
  if (!is_digit(s[0]) || !is_digit(s[1]) ||
      !is_digit(s[2]) || !is_digit(s[3]))
    return -1;
  return (s[0] - '0') * 1000 + (s[1] - '0') * 100 +
         (s[2] - '0') * 10 + (s[3] - '0');
}

static bool validate_format_date(const char *val, int len)
{
  if (len != 10) return false;
  if (val[4] != '-' || val[7] != '-') return false;
  int year= parse_4digits(val);
  int month= parse_2digits(val + 5);
  int day= parse_2digits(val + 8);
  if (year < 0 || month < 1 || month > 12) return false;
  if (day < 1 || day > (int) calc_days_in_month(year, month)) return false;
  return true;
}

/* Parse time: HH:MM:SS[.frac](Z|+HH:MM|-HH:MM) */
static bool validate_format_time_part(const char *val, int len)
{
  if (len < 9) return false;
  if (val[2] != ':' || val[5] != ':') return false;
  int hour= parse_2digits(val);
  int min= parse_2digits(val + 3);
  int sec= parse_2digits(val + 6);
  if (hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60)
    return false;
  int pos= 8;
  if (pos < len && val[pos] == '.')
  {
    pos++;
    if (pos >= len || !is_digit(val[pos])) return false;
    while (pos < len && is_digit(val[pos])) pos++;
  }
  if (pos >= len) return false;
  if (val[pos] == 'Z' || val[pos] == 'z')
  {
    pos++;
    return pos == len;
  }
  if (val[pos] == '+' || val[pos] == '-')
  {
    pos++;
    if (pos + 5 != len) return false;
    if (val[pos + 2] != ':') return false;
    int tz_h= parse_2digits(val + pos);
    int tz_m= parse_2digits(val + pos + 3);
    if (tz_h < 0 || tz_h > 23 || tz_m < 0 || tz_m > 59) return false;
    return true;
  }
  return false;
}

static bool validate_format_datetime(const char *val, int len)
{
  if (len < 20) return false;
  if (val[10] != 'T' && val[10] != 't') return false;
  if (!validate_format_date(val, 10)) return false;
  return validate_format_time_part(val + 11, len - 11);
}

static bool validate_format_duration(const char *val, int len)
{
  if (len < 2 || val[0] != 'P') return false;
  int pos= 1;
  bool has_time= false, has_any= false;

  while (pos < len && val[pos] != 'T')
  {
    if (!is_digit(val[pos])) return false;
    while (pos < len && is_digit(val[pos])) pos++;
    if (pos >= len) return false;
    if (val[pos] == 'Y' || val[pos] == 'M' ||
        val[pos] == 'W' || val[pos] == 'D')
    {
      has_any= true;
      pos++;
    }
    else
      return false;
  }
  if (pos < len && val[pos] == 'T')
  {
    has_time= true;
    pos++;
    if (pos >= len) return false;
    bool has_time_component= false;
    while (pos < len)
    {
      if (!is_digit(val[pos])) return false;
      while (pos < len && is_digit(val[pos])) pos++;
      if (pos >= len) return false;
      if (val[pos] == '.')
      {
        pos++;
        if (pos >= len || !is_digit(val[pos])) return false;
        while (pos < len && is_digit(val[pos])) pos++;
        if (pos >= len || val[pos] != 'S') return false;
      }
      if (val[pos] == 'H' || val[pos] == 'M' || val[pos] == 'S')
      {
        has_time_component= true;
        pos++;
      }
      else
        return false;
    }
    if (!has_time_component) return false;
  }
  return pos == len && (has_any || has_time);
}

/* Position of '@' separating Local-part from Domain (RFC 5321 4.1.2). */
static int find_email_split(const char *val, int len)
{
  const char *at= (const char *) memchr(val, '@', len);
  if (!at) return -1;
  int at_pos= (int) (at - val);

  if (val[0] == '"')
  {
    int i= 1;
    while (i < len && val[i] != '"')
    {
      if (val[i] == '\\' && i + 1 < len) i+= 2;
      else i++;
    }
    if (i >= len || val[i] != '"') return -1;
    i++;
    if (i >= len || val[i] != '@') return -1;
    at_pos= i;
  }
  else if (memchr(at + 1, '@', len - at_pos - 1))
    return -1;
  return at_pos;
}

/* 'email' format: RFC 5321 Mailbox (ASCII only, RFC 5321 4.1.2). */
static bool validate_format_email(const char *val, int len)
{
  int at_pos= find_email_split(val, len);
  if (at_pos < 1 || at_pos > 64) return false;
  int domain_len= len - at_pos - 1;
  if (domain_len < 1 || domain_len > 253) return false;
  const char *domain= val + at_pos + 1;
  bool has_dot= false;
  for (int i= 0; i < domain_len; i++)
  {
    if (domain[i] == '.')
    {
      has_dot= true;
      if (i == 0 || i == domain_len - 1) return false;
      if (domain[i - 1] == '.') return false;
    }
    else if (!is_alpha(domain[i]) && !is_digit(domain[i]) && domain[i] != '-')
      return false;
  }
  return has_dot;
}

/* 'idn-email' format: RFC 6531 extended Mailbox (UTF-8 permitted). */
static bool validate_format_idn_email(const char *val, int len)
{
  int at_pos= find_email_split(val, len);
  if (at_pos < 1) return false;
  int domain_len= len - at_pos - 1;
  if (domain_len < 1) return false;
  const char *domain= val + at_pos + 1;
  bool has_dot= false;
  for (int i= 0; i < domain_len; i++)
  {
    if (domain[i] == '.')
    {
      has_dot= true;
      if (i == 0 || i == domain_len - 1) return false;
      if (domain[i - 1] == '.') return false;
    }
  }
  return has_dot;
}

static bool validate_format_hostname(const char *val, int len)
{
  if (len < 1 || len > 253) return false;
  int label_start= 0;
  for (int i= 0; i <= len; i++)
  {
    if (i == len || val[i] == '.')
    {
      int label_len= i - label_start;
      if (label_len < 1 || label_len > 63) return false;
      if (val[label_start] == '-' || val[i - 1] == '-') return false;
      for (int j= label_start; j < i; j++)
      {
        if (!is_alpha(val[j]) && !is_digit(val[j]) && val[j] != '-')
          return false;
      }
      label_start= i + 1;
    }
  }
  return true;
}

static bool validate_format_idn_hostname(const char *val, int len)
{
  if (len < 1 || len > 253) return false;
  int label_start= 0;
  for (int i= 0; i <= len; i++)
  {
    if (i == len || val[i] == '.')
    {
      int label_len= i - label_start;
      if (label_len < 1 || label_len > 63) return false;
      if (val[label_start] == '-' || val[i - 1] == '-') return false;
      label_start= i + 1;
    }
  }
  return true;
}

static bool validate_format_ipv4(const char *val, int len)
{
  if (len < 7 || len > 15) return false;
  char buf[16];
  memcpy(buf, val, len);
  buf[len]= '\0';
  struct in_addr addr;
  return inet_pton(AF_INET, buf, &addr) == 1;
}

static bool validate_format_ipv6(const char *val, int len)
{
  if (len < 2 || len > 45) return false;
  char buf[46];
  memcpy(buf, val, len);
  buf[len]= '\0';
  struct in6_addr addr;
  return inet_pton(AF_INET6, buf, &addr) == 1;
}

/* RFC 3986 unreserved + gen-delims + sub-delims (unescaped URI bytes). */
static inline bool is_uri_char(char c)
{
  if (is_alpha(c) || is_digit(c)) return true;
  if (c == '-' || c == '.' || c == '_' || c == '~') return true;
  if (c == ':' || c == '/' || c == '?' || c == '#' ||
      c == '[' || c == ']' || c == '@') return true;
  if (c == '!' || c == '$' || c == '&' || c == '\'' ||
      c == '(' || c == ')' || c == '*' || c == '+' ||
      c == ',' || c == ';' || c == '=') return true;
  return false;
}

/* URI body: valid char or pct-encoded. allow_non_ascii permits IRI bytes. */
static bool validate_uri_tail(const char *val, int len, bool allow_non_ascii)
{
  int i= 0;
  while (i < len)
  {
    unsigned char c= (unsigned char) val[i];
    if (c == '%')
    {
      if (i + 2 >= len) return false;
      if (!is_hex(val[i + 1]) || !is_hex(val[i + 2])) return false;
      i+= 3;
      continue;
    }
    if (c >= 0x80)
    {
      if (!allow_non_ascii) return false;
      i++;
      continue;
    }
    if (!is_uri_char((char) c)) return false;
    i++;
  }
  return true;
}

/* scheme ":" then URI body (RFC 3986, syntactic check only). */
static bool validate_format_uri_ex(const char *val, int len,
                                   bool allow_non_ascii)
{
  if (len < 1 || !is_alpha(val[0])) return false;
  int i= 1;
  while (i < len && (is_alpha(val[i]) || is_digit(val[i]) ||
         val[i] == '+' || val[i] == '-' || val[i] == '.'))
    i++;
  if (i >= len || val[i] != ':') return false;
  return validate_uri_tail(val + i + 1, len - i - 1, allow_non_ascii);
}

static bool validate_format_uri(const char *val, int len)
{
  return validate_format_uri_ex(val, len, false);
}

static bool validate_format_iri(const char *val, int len)
{
  return validate_format_uri_ex(val, len, true);
}

/* URI-reference: absolute URI or relative-ref (RFC 3986 4.1). */
static bool validate_format_uri_reference_ex(const char *val, int len,
                                             bool allow_non_ascii)
{
  if (len == 0) return true;
  return validate_uri_tail(val, len, allow_non_ascii);
}

static bool validate_format_uri_reference(const char *val, int len)
{
  return validate_format_uri_reference_ex(val, len, false);
}

static bool validate_format_iri_reference(const char *val, int len)
{
  return validate_format_uri_reference_ex(val, len, true);
}

static bool validate_format_uuid(const char *val, int len)
{
  if (len != 36) return false;
  for (int i= 0; i < 36; i++)
  {
    if (i == 8 || i == 13 || i == 18 || i == 23)
    {
      if (val[i] != '-') return false;
    }
    else
    {
      if (!is_hex(val[i])) return false;
    }
  }
  return true;
}

static bool validate_format_json_pointer(const char *val, int len)
{
  if (len == 0) return true;
  if (val[0] != '/') return false;
  for (int i= 1; i < len; i++)
  {
    if (val[i] == '~')
    {
      if (i + 1 >= len) return false;
      if (val[i + 1] != '0' && val[i + 1] != '1') return false;
      i++;
    }
  }
  return true;
}

static bool validate_format_relative_json_pointer(const char *val, int len)
{
  if (len < 1) return false;
  if (!is_digit(val[0])) return false;
  int pos= 0;
  while (pos < len && is_digit(val[pos])) pos++;
  if (pos == len) return true;
  if (val[pos] == '#') return pos + 1 == len;
  return validate_format_json_pointer(val + pos, len - pos);
}

static bool validate_format_regex(const char *val, int len)
{
  int errcode;
  PCRE2_SIZE erroffset;
  pcre2_code *re= pcre2_compile((PCRE2_SPTR) val, (PCRE2_SIZE) len,
                                 0, &errcode, &erroffset, NULL);
  if (!re) return false;
  pcre2_code_free(re);
  return true;
}

bool Json_schema_format::validate(const json_engine_t *je,
                                  MEM_ROOT *current_mem_root,
                                  const uchar *k_start,
                                  const uchar *k_end)
{
  if (je->value_type != JSON_VALUE_STRING)
    return false;
  if (!current_thd->variables.json_schema_format_validation)
    return false;
  if (format_type == JSON_SCHEMA_FORMAT_NONE)
    return false;

  const char *val= (const char *) je->value;
  int len= (int) je->value_len;

  switch (format_type)
  {
  case JSON_SCHEMA_FORMAT_DATETIME:
    return !validate_format_datetime(val, len);
  case JSON_SCHEMA_FORMAT_DATE:
    return !validate_format_date(val, len);
  case JSON_SCHEMA_FORMAT_TIME:
    return !validate_format_time_part(val, len);
  case JSON_SCHEMA_FORMAT_DURATION:
    return !validate_format_duration(val, len);
  case JSON_SCHEMA_FORMAT_EMAIL:
    return !validate_format_email(val, len);
  case JSON_SCHEMA_FORMAT_IDN_EMAIL:
    return !validate_format_idn_email(val, len);
  case JSON_SCHEMA_FORMAT_HOSTNAME:
    return !validate_format_hostname(val, len);
  case JSON_SCHEMA_FORMAT_IDN_HOSTNAME:
    return !validate_format_idn_hostname(val, len);
  case JSON_SCHEMA_FORMAT_IPV4:
    return !validate_format_ipv4(val, len);
  case JSON_SCHEMA_FORMAT_IPV6:
    return !validate_format_ipv6(val, len);
  case JSON_SCHEMA_FORMAT_URI:
    return !validate_format_uri(val, len);
  case JSON_SCHEMA_FORMAT_IRI:
    return !validate_format_iri(val, len);
  case JSON_SCHEMA_FORMAT_URI_REFERENCE:
    return !validate_format_uri_reference(val, len);
  case JSON_SCHEMA_FORMAT_IRI_REFERENCE:
    return !validate_format_iri_reference(val, len);
  case JSON_SCHEMA_FORMAT_UUID:
    return !validate_format_uuid(val, len);
  case JSON_SCHEMA_FORMAT_JSON_POINTER:
    return !validate_format_json_pointer(val, len);
  case JSON_SCHEMA_FORMAT_RELATIVE_JSON_POINTER:
    return !validate_format_relative_json_pointer(val, len);
  case JSON_SCHEMA_FORMAT_REGEX:
    return !validate_format_regex(val, len);
  case JSON_SCHEMA_FORMAT_NONE:
    break;
  }
  return false;
}

bool Json_schema_type::validate(const json_engine_t *je,
                                MEM_ROOT *current_mem_root,
                                const uchar *k_start,
                                const uchar* k_end)
{
  return !((1 << je->value_type) & type);
}

bool Json_schema_type::handle_keyword(THD *thd,
                                      MEM_ROOT *current_mem_root,
                                      json_engine_t *je,
                                      const char* key_start,
                                      const char* key_end,
                                      List<Json_schema_keyword>
                                           *all_keywords)
{
  if (je->value_type == JSON_VALUE_ARRAY)
  {
    int level= je->stack_p;
    while (json_scan_next(je)==0 && je->stack_p >= level)
    {
      if (json_read_value(je))
        return true;
      json_assign_type(&type, je);
    }
    return je->s.error ? true : false;
  }
  else if (je->value_type == JSON_VALUE_STRING)
  {
    return json_assign_type(&type, je);
  }
  else
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "type");
    return true;
  }
}

bool Json_schema_const::validate(const json_engine_t *je,
                                 MEM_ROOT *current_mem_root,
                                 const uchar *k_start,
                                 const uchar* k_end)
{
  json_engine_t curr_je;
  curr_je= *je;
  const char *start= (char*)curr_je.value;
  const char *end= (char*)curr_je.value+curr_je.value_len;
  json_engine_t temp_je= *je;
  String a_res("", 0, curr_je.s.cs);
  int err= 0;

  if (type != curr_je.value_type)
   return true;

  if (curr_je.value_type <= JSON_VALUE_NUMBER)
  {
    if (!json_value_scalar(&temp_je))
    {
      if (json_skip_level(&temp_je))
      {
        curr_je= temp_je;
        return true;
      }
      end= (char*)temp_je.s.c_str;
    }
    String val((char*)temp_je.value, end-start, temp_je.s.cs);

    json_scan_start(&temp_je_2, temp_je.s.cs, (const uchar *) val.ptr(),
                    (const uchar *) val.end());

    if (temp_je.value_type != JSON_VALUE_STRING)
    {
      if (json_read_value(&temp_je_2))
      {
        curr_je= temp_je;
        return true;
      }
      json_get_normalized_string(&temp_je_2, &a_res, &err, current_mem_root, &temp_je_arg, &stack);
      if (err)
      {
        return true;
      }
    }
    else
      a_res.append(val.ptr(), val.length(), temp_je.s.cs);

    if (a_res.length() == strlen(const_json_value) &&
        !strncmp((const char*)const_json_value, a_res.ptr(),
                  a_res.length()))
    {
      return false;
    }
    return true;
  }

  return false;
}

bool Json_schema_const::handle_keyword(THD *thd,
                                       MEM_ROOT *current_mem_root,
                                       json_engine_t *je,
                                       const char* key_start,
                                       const char* key_end,
                                       List<Json_schema_keyword>
                                            *all_keywords)
{
  const char *start= (char*)je->value, *end= (char*)je->value+je->value_len;
  json_engine_t temp_je;
  String a_res("", 0, je->s.cs);
  int err;

  mem_root_dynamic_array_init(current_mem_root, PSI_INSTRUMENT_MEM,
                                &temp_je.stack,
                                 sizeof(int), NULL,
                                 JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));
  mem_root_dynamic_array_init(current_mem_root, PSI_INSTRUMENT_MEM,
                              &temp_je_arg.stack, sizeof(int), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));
  mem_root_dynamic_array_init(current_mem_root, PSI_INSTRUMENT_MEM,
                              &stack, sizeof(struct json_norm_value*), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));
  mem_root_dynamic_array_init(current_mem_root, PSI_INSTRUMENT_MEM,
                              &temp_je_2.stack, sizeof(int), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));

  type= je->value_type;

  if (!json_value_scalar(je))
  {
    if (json_skip_level(je))
      return true;
    end= (char*)je->s.c_str;
  }

  String val((char*)je->value, end-start, je->s.cs);

  json_scan_start(&temp_je, je->s.cs, (const uchar *) val.ptr(),
                 (const uchar *) val.end());
  if (je->value_type != JSON_VALUE_STRING)
  {
    if (json_read_value(&temp_je))
    {
      return true;
    }
    json_get_normalized_string(&temp_je, &a_res, &err, current_mem_root, &temp_je_arg, &stack);
    if (err)
    {
      return true;
    }
  }
  else
    a_res.append(val.ptr(), val.length(), je->s.cs);

  this->const_json_value= (char*)alloc_root(thd->mem_root,
                                            a_res.length()+1);
  if (!const_json_value)
  {
    return 1;
  }

  const_json_value[a_res.length()]= '\0';
  strncpy(const_json_value, (const char*)a_res.ptr(), a_res.length());

  return false;
}

bool Json_schema_enum::validate(const json_engine_t *je,
                                MEM_ROOT *current_mem_root,
                                const uchar *k_start,
                                const uchar* k_end)
{
  json_engine_t temp_je;
  temp_je= *je;

  String norm_str((char*)"",0, je->s.cs);

  String a_res("", 0, je->s.cs);
  int err= 1;

  if (temp_je.value_type > JSON_VALUE_NUMBER)
  {
    if (!(enum_scalar & (1 << temp_je.value_type)))
      return true;
    else
      return false;
  }
  json_get_normalized_string(&temp_je, &a_res, &err, current_mem_root, &temp_je_arg, &stack);
  if (err)
    return true;

  norm_str.append((const char*)a_res.ptr(), a_res.length(), je->s.cs);

  if (my_hash_search(&this->enum_values, (const uchar*)(norm_str.ptr()),
                       strlen((const char*)(norm_str.ptr()))))
    return false;
  else
    return true;
}

bool Json_schema_enum::handle_keyword(THD *thd, MEM_ROOT *current_mem_root,
                                      json_engine_t *je,
                                      const char* key_start,
                                      const char* key_end,
                                      List<Json_schema_keyword>
                                           *all_keywords)
{
  int count= 0;

  mem_root_dynamic_array_init(current_mem_root, PSI_INSTRUMENT_MEM,
                              &temp_je_arg.stack, sizeof(int), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));
  mem_root_dynamic_array_init(current_mem_root, PSI_INSTRUMENT_MEM,
                              &stack, sizeof(struct json_norm_value*), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));

  if (my_hash_init(PSI_INSTRUMENT_ME,
                   &this->enum_values,
                   je->s.cs, 1024, 0, 0, get_key_name,
                   NULL, 0))
    return true;

  if (je->value_type == JSON_VALUE_ARRAY)
  {
    int curr_level= je->stack_p;
    while(json_scan_next(je) == 0 && curr_level <= je->stack_p)
    {
      if (json_read_value(je))
        return true;
      count++;
      if (je->value_type > JSON_VALUE_NUMBER)
      {
        if (!(enum_scalar & (1 << je->value_type)))
          enum_scalar|= 1 << je->value_type;
        else
        {
          my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "enum");
          return true;
        }
      }
      else
      {
        char *norm_str;
        int err= 1;
        String a_res("", 0, je->s.cs);

        json_get_normalized_string(je, &a_res, &err, current_mem_root, &temp_je_arg, &stack);
        if (err)
          return true;

        norm_str= (char*)alloc_root(thd->mem_root,
                                    a_res.length()+1);
        if (!norm_str)
          return true;
        else
        {
          norm_str[a_res.length()]= '\0';
          strncpy(norm_str, (const char*)a_res.ptr(), a_res.length());
          if (!my_hash_search(&this->enum_values, (uchar*)norm_str,
                              strlen(norm_str)))
          {
            if (my_hash_insert(&this->enum_values, (uchar*)norm_str))
              return true;
          }
          else
          {
            my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "enum");
            return true;
          }
        }
      }
    }
    if (!count)
    {
     my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "enum");
     return true;
    }
    return false;
  }
  else
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "enum");
    return true;
  }
}

bool Json_schema_maximum::validate(const json_engine_t *je,
                                   MEM_ROOT *current_mem_root,
                                   const uchar *k_start,
                                   const uchar* k_end)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
    return false;

  double val= je->s.cs->strntod((char *) je->value,
                                  je->value_len, &end, &err);
  return (val <= value) ? false : true;
}

bool Json_schema_maximum::handle_keyword(THD *thd, MEM_ROOT *current_mem_root,
                                         json_engine_t *je,
                                         const char* key_start,
                                         const char* key_end,
                                         List<Json_schema_keyword>
                                              *all_keywords)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "maximum");
    return true;
  }

  double val= je->s.cs->strntod((char *) je->value,
                                 je->value_len, &end, &err);
  value= val;

  return false;
}

bool Json_schema_minimum::validate(const json_engine_t *je,
                                   MEM_ROOT *current_mem_root,
                                   const uchar *k_start,
                                   const uchar* k_end)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
    return false;

  double val= je->s.cs->strntod((char *) je->value,
                                  je->value_len, &end, &err);
  return val >= value ? false : true;
}

bool Json_schema_minimum::handle_keyword(THD *thd, MEM_ROOT *current_mem_root,
                                         json_engine_t *je,
                                         const char* key_start,
                                         const char* key_end,
                                         List<Json_schema_keyword>
                                              *all_keywords)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "minimum");
    return true;
  }

  double val= je->s.cs->strntod((char *) je->value,
                                 je->value_len, &end, &err);
  value= val;

  return false;
}

bool Json_schema_ex_minimum::validate(const json_engine_t *je,
                                      MEM_ROOT *current_mem_root,
                                      const uchar *k_start,
                                      const uchar* k_end)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
    return false;

  double val= je->s.cs->strntod((char *) je->value,
                                je->value_len, &end, &err);
  return (val > value) ? false : true;
}

bool Json_schema_ex_minimum::handle_keyword(THD *thd,
                                            MEM_ROOT *current_mem_root,
                                            json_engine_t *je,
                                            const char* key_start,
                                            const char* key_end,
                                            List<Json_schema_keyword>
                                                 *all_keywords)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "exclusiveMinimum");
    return true;
  }

  double val= je->s.cs->strntod((char *) je->value,
                                 je->value_len, &end, &err);
  value= val;

  return false;
}

bool Json_schema_ex_maximum::validate(const json_engine_t *je,
                                      MEM_ROOT *current_mem_root,
                                      const uchar *k_start,
                                      const uchar* k_end)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
    return false;

  double val= je->s.cs->strntod((char *) je->value,
                                  je->value_len, &end, &err);
  return (val < value) ? false : true;
}

bool Json_schema_ex_maximum::handle_keyword(THD *thd,
                                            MEM_ROOT *current_mem_root,
                                            json_engine_t *je,
                                            const char* key_start,
                                            const char* key_end,
                                            List<Json_schema_keyword>
                                                 *all_keywords)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "exclusiveMaximum");
    return true;
  }
  double val= je->s.cs->strntod((char *) je->value,
                                 je->value_len, &end, &err);
  value= val;

  return false;
}

bool Json_schema_multiple_of::validate(const json_engine_t *je,
                                       MEM_ROOT *current_mem_root,
                                       const uchar *k_start,
                                       const uchar* k_end)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
    return false;
  if (je->num_flags & JSON_NUM_FRAC_PART)
    return true;

  longlong val= je->s.cs->strntoll((char *) je->value,
                                  je->value_len, 10, &end, &err);

  return val % multiple_of;
}

bool Json_schema_multiple_of::handle_keyword(THD *thd,
                                             MEM_ROOT *current_mem_root,
                                             json_engine_t *je,
                                             const char* key_start,
                                             const char* key_end,
                                             List<Json_schema_keyword>
                                                  *all_keywords)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER || (je->num_flags & JSON_NUM_FRAC_PART))
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "multipleOf");
    return true;
  }

  longlong val= je->s.cs->strntoll((char *) je->value,
                                 je->value_len, 10, &end, &err);
  if (val <= 0)
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "multipleOf");
  multiple_of= val;

  return false;
}


bool Json_schema_max_len::validate(const json_engine_t *je,
                                  MEM_ROOT *current_mem_root,
                                   const uchar *k_start,
                                   const uchar* k_end)
{
  if (je->value_type != JSON_VALUE_STRING)
    return false;
  return (uint)(je->value_len) <= value ? false : true;
}

bool Json_schema_max_len::handle_keyword(THD *thd,
                                         MEM_ROOT *current_mem_root,
                                         json_engine_t *je,
                                         const char* key_start,
                                         const char* key_end,
                                         List<Json_schema_keyword>
                                             *all_keywords)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "maxLength");
    return true;
  }
  double val= je->s.cs->strntod((char *) je->value,
                                 je->value_len, &end, &err);
  if (val < 0)
  {
   my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "maxLength");
   return true;
  }
  value= (int)val;

  return false;
}

bool Json_schema_min_len::validate(const json_engine_t *je,
                                   MEM_ROOT *current_mem_root,
                                   const uchar *k_start,
                                   const uchar* k_end)
{
  if (je->value_type != JSON_VALUE_STRING)
    return false;
  return (uint)(je->value_len) >= value ? false : true;
}

bool Json_schema_min_len::handle_keyword(THD *thd, MEM_ROOT *current_mem_root,
                                         json_engine_t *je,
                                         const char* key_start,
                                         const char* key_end,
                                         List<Json_schema_keyword>
                                              *all_keywords)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "minLength");
    return true;
  }

  double val= je->s.cs->strntod((char *) je->value,
                                 je->value_len, &end, &err);
  if (val < 0)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "minLength");
    return true;
  }
  value= (int)val;

  return false;
}

bool Json_schema_pattern::validate(const json_engine_t *je,
                                   MEM_ROOT *current_mem_root,
                                   const uchar *k_start,
                                   const uchar* k_end)
{
  bool pattern_matches= false;

  /*
    We want to match a single pattern against multiple
    string when (see below):
    1) with "pattern" when there are different json strings
       to be validated against one pattern.
    2) with "propertyNames", where there is one pattern but
       multiple property names to be validated against one
       pattern
  */
  if (!k_start && !k_end)
  {
    /* 1) */
    if (je->value_type != JSON_VALUE_STRING)
      return false;
     str->str_value.set_or_copy_aligned((const char*)je->value,
                                        (size_t)je->value_len, je->s.cs);
  }
  else
  {
    /* 2) */
    str->str_value.set_or_copy_aligned((const char*)k_start,
                                        (size_t)(k_end-k_start), je->s.cs);
  }
  if (re.recompile(pattern))
    return true;
  if (re.exec(str, 0, 0))
    return true;
  pattern_matches= re.match();

  return pattern_matches ? false : true;
}

bool Json_schema_pattern::handle_keyword(THD *thd, MEM_ROOT *current_mem_root,
                                         json_engine_t *je,
                                         const char* key_start,
                                         const char* key_end,
                                         List<Json_schema_keyword>
                                              *all_keywords)
{
  if (je->value_type != JSON_VALUE_STRING)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "pattern");
    return true;
  }

  my_repertoire_t repertoire= my_charset_repertoire(je->s.cs);
  pattern= thd->make_string_literal((const char*)je->value,
                                             je->value_len, repertoire);
  str= new (thd->mem_root) Item_string(thd, "", (uint) 0, je->s.cs);
  re.init(je->s.cs, 0);
  re.unset_flag(PCRE2_CASELESS);

  return false;
}

bool Json_schema_max_items::validate(const json_engine_t *je,
                                     MEM_ROOT *current_mem_root,
                                     const uchar *k_start,
                                     const uchar* k_end)
{
  uint count= 0;
  json_engine_t curr_je;

  curr_je= *je;

  if (curr_je.value_type != JSON_VALUE_ARRAY)
    return false;

  int level= curr_je.stack_p;
  while(json_scan_next(&curr_je)==0 && level <= curr_je.stack_p)
  {
    if (json_read_value(&curr_je))
      return true;
    count++;
    if (!json_value_scalar(&curr_je))
    {
      if (json_skip_level(&curr_je))
        return true;
    }
  }
  return count > value ? true : false;
}

bool Json_schema_max_items::handle_keyword(THD *thd,
                                           MEM_ROOT *current_mem_root,
                                           json_engine_t *je,
                                           const char* key_start,
                                           const char* key_end,
                                           List<Json_schema_keyword>
                                                *all_keywords)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "maxItems");
    return true;
  }

  double val= je->s.cs->strntod((char *) je->value,
                                 je->value_len, &end, &err);
  if (val < 0)
  {
   my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "maxItems");
   return true;
  }
  value= (int)val;

  return false;
}

bool Json_schema_min_items::validate(const json_engine_t *je,
                                     MEM_ROOT *current_mem_root,
                                     const uchar *k_start,
                                     const uchar* k_end)
{
  uint count= 0;
  json_engine_t  curr_je;

  curr_je= *je;

  if (curr_je.value_type != JSON_VALUE_ARRAY)
    return false;

  int level= curr_je.stack_p;
  while(json_scan_next(&curr_je)==0 && level <= curr_je.stack_p)
  {
    if (json_read_value(&curr_je))
      return true;
    count++;
    if (!json_value_scalar(&curr_je))
    {
      if (json_skip_level(&curr_je))
        return true;
    }
  }
  return count < value ? true : false;
}

bool Json_schema_min_items::handle_keyword(THD *thd,
                                           MEM_ROOT *current_mem_root,
                                           json_engine_t *je,
                                           const char* key_start,
                                           const char* key_end,
                                           List<Json_schema_keyword>
                                                *all_keywords)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "minItems");
    return true;
  }

  double val= je->s.cs->strntod((char *) je->value,
                                 je->value_len, &end, &err);
  if (val < 0)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "maxLength");
    return true;
  }
  value= (int)val;

  return false;
}

bool Json_schema_max_contains::handle_keyword(THD *thd,
                                              MEM_ROOT *current_mem_root,
                                              json_engine_t *je,
                                              const char* key_start,
                                              const char* key_end,
                                              List<Json_schema_keyword>
                                                 *all_keywords)
{

  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "maxContains");
    return true;
  }

  double val= je->s.cs->strntod((char *) je->value,
                                   je->value_len, &end, &err);
  if (val < 0)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "maxContains");
    return true;
  }
  value= val;
  return false;
}


bool Json_schema_min_contains::handle_keyword(THD *thd,
                                              MEM_ROOT *current_mem_root,
                                              json_engine_t *je,
                                              const char* key_start,
                                              const char* key_end,
                                              List<Json_schema_keyword>
                                                 *all_keywords)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "minContains");
    return true;
  }

  double val= je->s.cs->strntod((char *) je->value,
                                   je->value_len, &end, &err);
  value= val;
  if (val < 0)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "minContains");
    return true;
  }

  return false;
}


bool Json_schema_contains::validate(const json_engine_t *je,
                                    MEM_ROOT *current_mem_root,
                                    const uchar *k_start,
                                    const uchar* k_end)
{
  uint contains_count=0;
  json_engine_t curr_je;
  curr_je= *je;
  int level= je->stack_p;
  bool validated= true;

  if (curr_je.value_type != JSON_VALUE_ARRAY)
    return false;

  while(json_scan_next(&curr_je)==0 && level <= curr_je.stack_p)
  {
    if (json_read_value(&curr_je))
     return true;
    validated= true;
    if (validate_schema_items(&curr_je, current_mem_root, &contains))
      validated= false;
    if (!json_value_scalar(&curr_je))
    {
      if (json_skip_level(&curr_je))
        return true;
    }
    if (validated)
      contains_count++;
  }

  if ((max_contains ? contains_count <= max_contains->value :
                      contains_count>0) &&
      (min_contains ? contains_count >= min_contains->value :
                      contains_count>0))
    return false;

  return true;
}

bool Json_schema_contains::handle_keyword(THD *thd,
                                          MEM_ROOT *current_mem_root,
                                          json_engine_t *je,
                                          const char* key_start,
                                          const char* key_end,
                                          List<Json_schema_keyword>
                                               *all_keywords)
{
  if (je->value_type != JSON_VALUE_OBJECT)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "contains");
    return true;
  }
  return create_object_and_handle_keyword(thd, current_mem_root, je,
                                         &contains, all_keywords);
}


bool Json_schema_items::handle_keyword(THD *thd, MEM_ROOT *current_mem_root,
                                       json_engine_t *je,
                                       const char* key_start,
                                       const char* key_end,
                                       List<Json_schema_keyword>
                                            *all_keywords)
{
  if (je->value_type == JSON_VALUE_FALSE)
  {
    set_allowed(false);
    return false;
  }
  if (je->value_type == JSON_VALUE_OBJECT)
  {
    return create_object_and_handle_keyword(thd, current_mem_root, je,
                              &items_schema,
                                            all_keywords);
  }
  else if (je->value_type != JSON_VALUE_TRUE)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "items");
    return true;
  }
  return false;
}

bool Json_schema_items::validate_as_alternate(const json_engine_t *je,
                                              const uchar *k_start,
                                              const uchar* k_end,
                                              MEM_ROOT *current_mem_root)
{
  /*
    The indexes in prefix array were less than that in the json array.
    So validate remaining using the json schema
  */
  return validate_schema_items(je, current_mem_root, &items_schema);
}

bool Json_schema_items::validate(const json_engine_t *je,
                                 MEM_ROOT *current_mem_root,
                                 const uchar *k_start,
                                 const uchar* k_end)
{

 /*
    There was no "prefixItesm", so we validate all values in the
    array using one schema.
  */
  int level= je->stack_p, count=0;
  bool is_false= false;
  json_engine_t curr_je= *je;

  if (je->value_type != JSON_VALUE_ARRAY)
    return false;

  if (!allowed)
    is_false= true;

  while (json_scan_next(&curr_je)==0 && level <= curr_je.stack_p)
  {
    if (json_read_value(&curr_je))
      return true;
    count++;
    if (validate_schema_items(&curr_je, current_mem_root, &items_schema))
      return true;
    if (!json_value_scalar(&curr_je))
    {
      if (json_skip_level(&curr_je))
        return true;
    }
  }

  return is_false ? (!count ? false : true) : false;
}

bool Json_schema_prefix_items::validate(const json_engine_t *je,
                                        MEM_ROOT *current_mem_root,
                                        const uchar *k_start,
                                        const uchar* k_end)
{
  int level= je->stack_p;
  json_engine_t curr_je= *je;
  List_iterator <List<Json_schema_keyword>> it1 (prefix_items);
  List<Json_schema_keyword> *curr_prefix;

  if (curr_je.value_type != JSON_VALUE_ARRAY)
    return false;

  while(curr_je.s.c_str < curr_je.s.str_end && json_scan_next(&curr_je)==0 &&
        curr_je.stack_p >= level)
  {
    if (json_read_value(&curr_je))
      return true;
    if (!(curr_prefix=it1++))
    {
      if (fall_back_on_alternate_schema(&curr_je, current_mem_root))
        return true;
      else
      {
        if (!json_value_scalar(&curr_je))
        {
          if (json_skip_level(&curr_je))
            return true;
        }
      }
    }
    else
    {
      if (validate_schema_items(&curr_je, current_mem_root,
                                &(*curr_prefix)))
        return true;
      if (!json_value_scalar(&curr_je))
      {
        if (json_skip_level(&curr_je))
          return true;
      }
    }
  }
  return false;
}

bool Json_schema_prefix_items::handle_keyword(THD *thd,
                                              MEM_ROOT *current_mem_root,
                                              json_engine_t *je,
                                              const char* key_start,
                                              const char* key_end,
                                              List<Json_schema_keyword>
                                                   *all_keywords)
{
  json_engine_t temp_je;
  int level= je->stack_p;

  if (je->value_type != JSON_VALUE_ARRAY)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "prefixItems");
    return true;
  }

  mem_root_dynamic_array_init(current_mem_root, PSI_INSTRUMENT_MEM,
                              &temp_je.stack, sizeof(int), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));

  while(json_scan_next(je)==0 && je->stack_p >= level)
  {
    char *begin, *end;
    int len;

      if (json_read_value(je))
      {
        return true;
      }
      if (je->value_type != JSON_VALUE_OBJECT)
      {
       my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "items");
       return true;
      }

      begin= (char*)je->value;

    if (json_skip_level(je))
    {
      return true;
    }

    end= (char*)je->s.c_str;
    len= (int)(end-begin);

    json_scan_start(&temp_je, je->s.cs, (const uchar *) begin,
                    (const uchar *)begin+len);
      List<Json_schema_keyword> *keyword_list=
                        new (thd->mem_root) List<Json_schema_keyword>;

      if (!keyword_list)
      {
        return true;
      }
      if (create_object_and_handle_keyword(thd, current_mem_root,
                                        &temp_je, keyword_list,
                                           all_keywords))
      {
        return true;
      }

      prefix_items.push_back(keyword_list, thd->mem_root);
  }

  return false;
}

bool Json_schema_unique_items::validate(const json_engine_t *je,
                                        MEM_ROOT *current_mem_root,
                                        const uchar *k_start,
                                        const uchar* k_end)
{
  HASH unique_items;
  List <char> norm_str_list;
  json_engine_t curr_je= *je;
  int res= true, level= curr_je.stack_p, scalar_val= 0;

  if (curr_je.value_type != JSON_VALUE_ARRAY)
    return false;

  if (my_hash_init(PSI_INSTRUMENT_ME, &unique_items, curr_je.s.cs,
                   1024, 0, 0, get_key_name, NULL, 0))
    return true;

  while(json_scan_next(&curr_je)==0 && level <= curr_je.stack_p)
  {
    int err= 1;
    char *norm_str;
    String a_res("", 0, curr_je.s.cs);

    if (json_read_value(&curr_je))
      goto end;

    json_get_normalized_string(&curr_je, &a_res, &err, current_mem_root, &temp_je_arg, &stack);

    if (err)
      goto end;

    norm_str= (char*)malloc(a_res.length()+1);
    if (!norm_str)
      goto end;

    norm_str[a_res.length()]= '\0';
    strncpy(norm_str, (const char*)a_res.ptr(), a_res.length());
    norm_str_list.push_back(norm_str);

   if (curr_je.value_type > JSON_VALUE_NUMBER)
   {
    if (scalar_val & 1 << curr_je.value_type)
      goto end;
    scalar_val|= 1 << curr_je.value_type;
   }
    else
    {
      if (!my_hash_search(&unique_items, (uchar*)norm_str,
                          strlen(((const char*)norm_str))))
      {
        if (my_hash_insert(&unique_items, (uchar*)norm_str))
          goto end;
      }
      else
        goto end;
    }
    a_res.set("", 0, curr_je.s.cs);
  }

  res= false;

  end:
  if (!norm_str_list.is_empty())
  {
    List_iterator<char> it(norm_str_list);
    char *curr_norm_str;
    while ((curr_norm_str= it++))
      free(curr_norm_str);
    norm_str_list.empty();
  }
  my_hash_free(&unique_items);
  return res;
}

bool Json_schema_unique_items::handle_keyword(THD *thd,
                                              MEM_ROOT *current_mem_root,
                                              json_engine_t *je,
                                              const char* key_start,
                                              const char* key_end,
                                              List<Json_schema_keyword>
                                                  *all_keywords)
{
  mem_root_dynamic_array_init(current_mem_root, PSI_INSTRUMENT_MEM,
                              &temp_je_arg.stack, sizeof(int), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));
  mem_root_dynamic_array_init(current_mem_root, PSI_INSTRUMENT_MEM,
                              &stack, sizeof(struct json_norm_value*), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));

  if (je->value_type == JSON_VALUE_TRUE)
    is_unique= true;
  else if (je->value_type == JSON_VALUE_FALSE)
    is_unique= false;
  else
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "uniqueItems");
    return true;
  }
  return false;
}


bool Json_schema_max_prop::validate(const json_engine_t *je,
                                    MEM_ROOT *current_mem_root,
                                    const uchar *k_start,
                                    const uchar* k_end)
{
  uint properties_count= 0;
  json_engine_t curr_je= *je;
  int curr_level= je->stack_p;

  if (curr_je.value_type != JSON_VALUE_OBJECT)
    return false;

  while (json_scan_next(&curr_je)== 0 && je->stack_p >= curr_level)
  {
    switch (curr_je.state)
    {
      case JST_KEY:
      {
        if (json_read_value(&curr_je))
          return true;
        properties_count++;

        if (!json_value_scalar(&curr_je))
        {
          if (json_skip_level(&curr_je))
            return true;
        }
      }
    }
  }
  return properties_count > value ? true : false;
}

bool Json_schema_max_prop::handle_keyword(THD *thd,
                                          MEM_ROOT *current_mem_root,
                                          json_engine_t *je,
                                          const char* key_start,
                                          const char* key_end,
                                          List<Json_schema_keyword>
                                               *all_keywords)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "maxProperties");
    return true;
  }

  double val= je->s.cs->strntod((char *) je->value,
                                 je->value_len, &end, &err);
  if (val < 0)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "maxProperties");
    return true;
  }
  value= (int)val;

  return false;
}

bool Json_schema_min_prop::validate(const json_engine_t *je,
                                    MEM_ROOT *current_mem_root,
                                    const uchar *k_start,
                                    const uchar* k_end)
{
  uint properties_count= 0;
  int curr_level= je->stack_p;
  json_engine_t curr_je= *je;

  if (curr_je.value_type != JSON_VALUE_OBJECT)
    return false;

  while (json_scan_next(&curr_je)== 0 && je->stack_p >= curr_level)
  {
    switch (curr_je.state)
    {
      case JST_KEY:
      {
        if (json_read_value(&curr_je))
          return true;
        properties_count++;

        if (!json_value_scalar(&curr_je))
        {
          if (json_skip_level(&curr_je))
            return true;
        }
      }
    }
  }
  return properties_count < value ? true : false;
}

bool Json_schema_min_prop::handle_keyword(THD *thd,
                                          MEM_ROOT *current_mem_root,
                                          json_engine_t *je,
                                          const char* key_start,
                                          const char* key_end,
                                          List<Json_schema_keyword>
                                              *all_keywords)
{
  int err= 0;
  char *end;

  if (je->value_type != JSON_VALUE_NUMBER)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "minProperties");
    return true;
  }

  double val= je->s.cs->strntod((char *) je->value,
                                 je->value_len, &end, &err);
  if (val < 0)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "minProperties");
    return true;
  }
  value= (int)val;

  return false;
}

bool Json_schema_required::validate(const json_engine_t *je,
                                    MEM_ROOT *current_mem_root,
                                    const uchar *k_start,
                                    const uchar* k_end)
{
  json_engine_t curr_je= *je;
  List<char> malloc_mem_list;
  HASH required;
  int res= true, curr_level= curr_je.stack_p;
  List_iterator<String> it(required_properties);
  String *curr_str;

  if (curr_je.value_type != JSON_VALUE_OBJECT)
    return false;

  if(my_hash_init(PSI_INSTRUMENT_ME, &required,
               curr_je.s.cs, 1024, 0, 0, get_key_name,
               NULL, 0))
      return true;
  while (json_scan_next(&curr_je)== 0 && curr_je.stack_p >= curr_level)
  {
    switch (curr_je.state)
    {
      case JST_KEY:
      {
        const uchar *key_end, *key_start;
        int key_len;
        char *str;

        key_start= curr_je.s.c_str;
        do
        {
          key_end= curr_je.s.c_str;
        } while (json_read_keyname_chr(&curr_je) == 0);

        key_len= (int)(key_end-key_start);
        str= (char*)malloc((size_t)(key_len)+1);
        strncpy(str, (const char*)key_start, key_len);
        str[key_len]='\0';

        if (my_hash_insert(&required, (const uchar*)str))
          goto error;
        malloc_mem_list.push_back(str);
      }
    }
  }
  while ((curr_str= it++))
  {
    if (!my_hash_search(&required, (const uchar*)curr_str->ptr(),
                        curr_str->length()))
      goto error;
  }
  res= false;
  error:
  if (!malloc_mem_list.is_empty())
  {
    List_iterator<char> it(malloc_mem_list);
    char *curr_ptr;
    while ((curr_ptr= it++))
      free(curr_ptr);
    malloc_mem_list.empty();
  }
  my_hash_free(&required);
  return res;
}

bool Json_schema_required::handle_keyword(THD *thd,
                                          MEM_ROOT *current_mem_root,
                                          json_engine_t *je,
                                          const char* key_start,
                                          const char* key_end,
                                          List<Json_schema_keyword>
                                               *all_keywords)
{
  int level= je->stack_p;

  if (je->value_type != JSON_VALUE_ARRAY)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "required");
    return true;
  }
  while(json_scan_next(je)==0 && level <= je->stack_p)
  {
    if (json_read_value(je))
      return true;
    if (je->value_type != JSON_VALUE_STRING)
    {
      my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "required");
      return true;
    }
    else
    {
      String *str= new (thd->mem_root)String((char*)je->value,
                                                    je->value_len, je->s.cs);
      this->required_properties.push_back(str, thd->mem_root);
    }
  }
  return je->s.error ? true : false;
}

bool Json_schema_dependent_required::validate(const json_engine_t *je,
                                              MEM_ROOT *current_mem_root,
                                              const uchar *k_start,
                                              const uchar* k_end)
{
  json_engine_t curr_je= *je;
  HASH properties;
  bool res= true;
  int curr_level= curr_je.stack_p;
  List <char> malloc_mem_list;
  List_iterator<st_dependent_keywords> it(dependent_required);
  st_dependent_keywords *curr_keyword= NULL;

  if (curr_je.value_type != JSON_VALUE_OBJECT)
    return false;

  if (my_hash_init(PSI_INSTRUMENT_ME, &properties,
                 curr_je.s.cs, 1024, 0, 0, get_key_name,
                 NULL, 0))
    return true;

  while (json_scan_next(&curr_je)== 0 && curr_je.stack_p >= curr_level)
  {
    switch (curr_je.state)
    {
      case JST_KEY:
      {
        const uchar *key_end, *key_start;
        int key_len;
        char *str;

        key_start= curr_je.s.c_str;
        do
        {
          key_end= curr_je.s.c_str;
        } while (json_read_keyname_chr(&curr_je) == 0);

        key_len= (int)(key_end-key_start);
        str= (char*)malloc((size_t)(key_len)+1);
        strncpy(str, (const char*)key_start, key_len);
        str[(int)(key_end-key_start)]='\0';

        if (my_hash_insert(&properties, (const uchar*)str))
          goto error;
        malloc_mem_list.push_back(str);
      }
    }
  }
  while ((curr_keyword= it++))
  {
    if (my_hash_search(&properties,
                       (const uchar*)curr_keyword->property->ptr(),
                          curr_keyword->property->length()))
    {
      List_iterator<String> it2(curr_keyword->dependents);
      String *curr_depended_keyword;
      while ((curr_depended_keyword= it2++))
      {
        if (!my_hash_search(&properties,
                            (const uchar*)curr_depended_keyword->ptr(),
                            curr_depended_keyword->length()))
        {
          goto error;
        }
      }
    }
  }
  res= false;

  error:
  my_hash_free(&properties);
  if (!malloc_mem_list.is_empty())
  {
    List_iterator<char> it(malloc_mem_list);
    char *curr_ptr;
    while ((curr_ptr= it++))
      free(curr_ptr);
    malloc_mem_list.empty();
  }
  return res;
}

bool Json_schema_dependent_required::handle_keyword(THD *thd,
                                                    MEM_ROOT *current_mem_root,
                                                    json_engine_t *je,
                                                    const char* key_start,
                                                    const char* key_end,
                                                    List<Json_schema_keyword>
                                                     *all_keywords)
{
  if (je->value_type == JSON_VALUE_OBJECT)
  {
    int level1= je->stack_p;
    while (json_scan_next(je)==0 && level1 <= je->stack_p)
    {
      switch(je->state)
      {
        case JST_KEY:
        {
          const uchar *k_end, *k_start;
          int k_len;

          k_start= je->s.c_str;
          do
          {
            k_end= je->s.c_str;
          } while (json_read_keyname_chr(je) == 0);

          k_len= (int)(k_end-k_start);
          if (json_read_value(je))
            return true;

          if (je->value_type == JSON_VALUE_ARRAY)
          {
            st_dependent_keywords *curr_dependent_keywords=
                    (st_dependent_keywords *) alloc_root(thd->mem_root,
                                                sizeof(st_dependent_keywords));

            if (curr_dependent_keywords)
            {
              curr_dependent_keywords->property=
                          new (thd->mem_root)String((char*)k_start,
                                                     k_len, je->s.cs);
              curr_dependent_keywords->dependents.empty();
              int level2= je->stack_p;
              while (json_scan_next(je)==0 && level2 <= je->stack_p)
              {
                if (json_read_value(je) || je->value_type != JSON_VALUE_STRING)
                {
                  my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0),
                           "dependentRequired");
                  return true;
                }
                else
                {
                  String *str=
                    new (thd->mem_root)String((char*)je->value,
                                                  je->value_len, je->s.cs);
                  curr_dependent_keywords->dependents.push_back(str, thd->mem_root);
                }
              }
              dependent_required.push_back(curr_dependent_keywords, thd->mem_root);
            }
            else
            {
              return true;
            }
          }
          else
          {
            my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0),
                     "dependentRequired");
            return true;
          }
        }
      }
    }
  }
  else
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "dependentRequired");
    return true;
  }
  return je->s.error ? true : false;
}

bool Json_schema_property_names::validate(const json_engine_t *je,
                                          MEM_ROOT *current_mem_root,
                                          const uchar *k_start,
                                          const uchar* k_end)
{
  json_engine_t curr_je= *je;
  int level= curr_je.stack_p;

  if (je->value_type != JSON_VALUE_OBJECT)
    return false;

  while (json_scan_next(&curr_je)==0 && level <= curr_je.stack_p)
  {
    switch(curr_je.state)
    {
      case JST_KEY:
      {
        const uchar *k_end, *k_start;
        k_start= curr_je.s.c_str;
        do
        {
          k_end= curr_je.s.c_str;
        } while (json_read_keyname_chr(&curr_je) == 0);

        if (json_read_value(&curr_je))
          return true;
        if (!json_value_scalar(&curr_je))
        {
          if (json_skip_level(&curr_je))
            return true;
        }

        List_iterator <Json_schema_keyword> it1 (property_names);
        Json_schema_keyword *curr_schema= NULL;
        while((curr_schema= it1++))
        {
          if (curr_schema->validate(&curr_je, current_mem_root, k_start, k_end))
            return true;
        }
      }
    }
  }

  return false;
}

bool Json_schema_property_names::handle_keyword(THD *thd,
                                                MEM_ROOT *current_mem_root,
                                                json_engine_t *je,
                                                const char* key_start,
                                                const char* key_end,
                                                List<Json_schema_keyword>
                                                   *all_keywords)
{
  if (je->value_type != JSON_VALUE_OBJECT)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "propertyNames");
    return true;
  }
  if (create_object_and_handle_keyword(thd, current_mem_root, je,
                         &property_names, all_keywords))
    return true;

  return false;
}

/*
 additional_items, additional_properties, unevaluated_items,
 unevaluated_properties are all going to be schemas
 (basically of object type). So they all can be handled
 just like any other schema.
*/
bool
Json_schema_additional_and_unevaluated::handle_keyword(THD *thd,
                                                       MEM_ROOT *current_mem_root,
                                                      json_engine_t *je,
                                                      const char* key_start,
                                                      const char* key_end,
                                                      List<Json_schema_keyword>
                                                      *all_keywords)
{
  if (je->value_type == JSON_VALUE_FALSE)
  {
    set_allowed(false);
    return false;
  }
  else if (je->value_type == JSON_VALUE_OBJECT)
  {
    return create_object_and_handle_keyword(thd, current_mem_root, je,
                                            &schema_list,
                                            all_keywords);
  }
  if (je->value_type != JSON_VALUE_TRUE)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), keyword_map->func_name.str);
    return true;
  }
  return false;
}

/*
 Validating properties as an alternate schema happens only when we have
 logic keywords. Example we have allOf, and one key is not
 validated against allOf but it is present in "properties" and validates
 against it. Then the validation result should be true. So we would want that
 key to be validated against "properties", with "properties" begin
 alternate schema.
*/
bool Json_schema_properties::validate_as_alternate(const json_engine_t *je,
                                                   const uchar* k_start,
                                                   const uchar* k_end,
                                                   MEM_ROOT *current_mem_root)
{
  st_property *curr_property= NULL;
  json_engine_t curr_je= *je;
  if ((curr_property=
        (st_property*)my_hash_search(&properties,
                                (const uchar*)k_start,
                                (size_t)(k_end-k_start))))
  {
    if (validate_schema_items(&curr_je, current_mem_root,
                              curr_property->curr_schema))
      return true;
    if (!json_value_scalar(&curr_je))
    {
      if (json_skip_level(&curr_je))
        return true;
    }
  }
  else
  {
    if (alternate_schema && alternate_schema->validate_as_alternate(je, k_start,
                                                                    k_end,
                                                                    current_mem_root))
    {
      return true;
    }
  }
  return false;
}

bool
Json_schema_additional_and_unevaluated::
                               validate_as_alternate(const json_engine_t *je,
                                                     const uchar* k_start,
                                                     const uchar* k_end,
                                                     MEM_ROOT  *current_mem_root)
{
  if (!allowed)
    return true;
  return validate_schema_items(je, current_mem_root, &schema_list);
}


/*
  Makes sense on its own, without existence of additionalProperties,
  properties, patternProperties.
*/
bool Json_schema_unevaluated_properties::validate(const json_engine_t *je,
                                                  MEM_ROOT *current_mem_root,
                                                  const uchar *k_start,
                                                  const uchar* k_end)
{
  json_engine_t curr_je= *je;
  int level= curr_je.stack_p, count= 0;
  bool has_false= false;

  if (je->value_type != JSON_VALUE_OBJECT)
    return false;

  if (!allowed)
    has_false= true;

  while (json_scan_next(&curr_je)==0 && level <= curr_je.stack_p)
  {
    if (json_read_value(&curr_je))
      return true;
    count++;
    if (validate_schema_items(&curr_je, current_mem_root, &schema_list))
      return true;
  }
  return has_false ? (!count ? false: true) : false;
}

/*
  Unlike additionalItems, additionalProperties makes sense on its own
  without existence of properties and patternProperties,
*/
bool Json_schema_additional_properties::validate(const json_engine_t *je,
                                                 MEM_ROOT *current_mem_root,
                                                 const uchar *k_start,
                                                 const uchar* k_end)
{
  json_engine_t curr_je= *je;
  int level= curr_je.stack_p;

  if (je->value_type != JSON_VALUE_OBJECT)
    return false;

  while (json_scan_next(&curr_je)==0 && level <= curr_je.stack_p)
  {
    switch(curr_je.state)
    {
      case JST_KEY:
        if (json_read_value(&curr_je))
          return true;
        if (validate_schema_items(&curr_je, current_mem_root, &schema_list))
         return true;
      }
  }

  return false;
}

/*
  When items/prefix Items is present and if a key is not validated
  against it, then additionalItems is validated as an "alternate validation".
  It will be present/linked as alternate keyword and will not be present
  in the schema list for that level. This function called when
  "items"/"prefixItems" is absent, i.e when additionalItems appears
  in the schema list for that level.
  So additional Properties on its own will not make sense.
*/
bool Json_schema_additional_items::validate(const json_engine_t *je,
                                            MEM_ROOT *current_mem_root,
                                            const uchar *k_start,
                                            const uchar* k_end)
{
 return false;
}
bool Json_schema_unevaluated_items::validate(const json_engine_t *je,
                                             MEM_ROOT *current_mem_root,
                                             const uchar *k_start,
                                             const uchar* k_end)
{
  /*
    Makes sense on its own without adjacent keywords.
  */
  int level= je->stack_p, count=0;
  bool is_false= false;
  json_engine_t curr_je= *je;

  if (je->value_type != JSON_VALUE_ARRAY)
    return false;

  if (!allowed)
    is_false= true;

  while (json_scan_next(&curr_je)==0 && level <= curr_je.stack_p)
  {
    if (json_read_value(&curr_je))
      return true;
    count++;
   if (validate_schema_items(&curr_je, current_mem_root, &schema_list))
     return true;
  }

  return is_false ? (!count ? false : true) : false;
}

bool Json_schema_properties::validate(const json_engine_t *je,
                                      MEM_ROOT *current_mem_root,
                                      const uchar *k_start,
                                      const uchar* k_end)
{
  json_engine_t curr_je= *je;

  if (curr_je.value_type != JSON_VALUE_OBJECT)
    return false;

  int level= curr_je.stack_p;
  while (json_scan_next(&curr_je)==0 && level <= curr_je.stack_p)
  {
    switch(curr_je.state)
    {
      case JST_KEY:
      {
        const uchar *k_end, *k_start= curr_je.s.c_str;
        do
        {
          k_end= curr_je.s.c_str;
        } while (json_read_keyname_chr(&curr_je) == 0);

        if (json_read_value(&curr_je))
          return true;

        st_property *curr_property= NULL;
        if ((curr_property=
                      (st_property*)my_hash_search(&properties,
                                                   (const uchar*)k_start,
                                                    (size_t)(k_end-k_start))))
        {
          if (validate_schema_items(&curr_je, current_mem_root,
                      curr_property->curr_schema))
            return true;
        }
        else
        {
          if (fall_back_on_alternate_schema(&curr_je, current_mem_root,
                                            k_start, k_end))
            return true;
        }
        if (!json_value_scalar(&curr_je))
        {
          if (json_skip_level(&curr_je))
            return true;
        }
      }
    }
  }

  return false;
}

bool Json_schema_properties::handle_keyword(THD *thd,
                                            MEM_ROOT *current_mem_root,
                                            json_engine_t *je,
                                            const char* key_start,
                                            const char* key_end,
                                            List<Json_schema_keyword>
                                                 *all_keywords)
{

  if (je->value_type != JSON_VALUE_OBJECT)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "properties");
    return true;
  }

  if (my_hash_init(PSI_INSTRUMENT_ME,
                   &this->properties,
                   je->s.cs, 1024, 0, 0,
                   get_key_name_for_property,
                   NULL, 0))
      return true;
  is_hash_inited= true;

  int level= je->stack_p;
  while (json_scan_next(je)==0 && level <= je->stack_p)
  {
    switch(je->state)
    {
      case JST_KEY:
      {
        const uchar *k_end, *k_start= je->s.c_str;
        do
        {
          k_end= je->s.c_str;
        } while (json_read_keyname_chr(je) == 0);

        if (json_read_value(je))
          return true;

        st_property *curr_property=
                         (st_property*)alloc_root(thd->mem_root,
                                                   sizeof(st_property));
        if (curr_property)
        {
          curr_property->key_name= (char*)alloc_root(thd->mem_root,
                                                   (size_t)(k_end-k_start)+1);
          curr_property->curr_schema=
                       new (thd->mem_root) List<Json_schema_keyword>;
          if (curr_property->key_name)
          {
            curr_property->key_name[(int)(k_end-k_start)]= '\0';
            strncpy((char*)curr_property->key_name, (const char*)k_start,
                    (size_t)(k_end-k_start));
            if (create_object_and_handle_keyword(thd, current_mem_root, je,
                                             curr_property->curr_schema,
                                             all_keywords))
              return true;
            if (my_hash_insert(&properties, (const uchar*)curr_property))
              return true;
          }
        }
      }
    }
  }
  return je->s.error ? true : false;
}

bool Json_schema_pattern_properties::
                           validate_as_alternate(const json_engine_t *curr_je,
                                                 const uchar *k_start,
                                                 const uchar* k_end,
                                                 MEM_ROOT *current_mem_root)
{
  bool match_found= false;
  List_iterator <st_pattern_to_property> it1 (pattern_properties);
  st_pattern_to_property *curr_pattern_property= NULL;

  str->str_value.set_or_copy_aligned((const char*)k_start,
                                     (size_t)(k_end-k_start), curr_je->s.cs);

  while ((curr_pattern_property= it1++))
  {
    if (curr_pattern_property->re.recompile(curr_pattern_property->pattern))
      return true;
    if (curr_pattern_property->re.exec(str, 0, 0))
      return true;
    if (curr_pattern_property->re.match())
    {
      match_found= true;
      if (validate_schema_items(curr_je, current_mem_root,
                   curr_pattern_property->curr_schema))
            return true;
        break;
    }
  }
  if (!match_found)
  {
    if (fall_back_on_alternate_schema(curr_je, current_mem_root))
     return true;
  }
  return false;
}


bool Json_schema_pattern_properties::validate(const json_engine_t *je,
                                              MEM_ROOT *current_mem_root,
                                              const uchar *k_start,
                                              const uchar* k_end)
{
  json_engine_t curr_je= *je;
  int level= je->stack_p;
  bool match_found= false;

  if (je->value_type != JSON_VALUE_OBJECT)
    return false;

  while (json_scan_next(&curr_je)==0 && level <= curr_je.stack_p)
  {
    switch(curr_je.state)
    {
      case JST_KEY:
      {
        const uchar *k_end, *k_start= curr_je.s.c_str;
        do
        {
          k_end= curr_je.s.c_str;
        } while (json_read_keyname_chr(&curr_je) == 0);

        str->str_value.set_or_copy_aligned((const char*)k_start,
                                     (size_t)(k_end-k_start),
                                           curr_je.s.cs);

        if (json_read_value(&curr_je))
          return true;

        List_iterator <st_pattern_to_property> it1 (pattern_properties);
        st_pattern_to_property *curr_pattern_property= NULL;

        while ((curr_pattern_property= it1++))
        {
          if (curr_pattern_property->re.recompile(curr_pattern_property->pattern))
            return true;
          if (curr_pattern_property->re.exec(str, 0, 0))
            return true;
          if (curr_pattern_property->re.match())
          {
            match_found= true;
            if (validate_schema_items(&curr_je, current_mem_root,
                        curr_pattern_property->curr_schema))
              return true;
          }
        }
        if (!match_found)
        {
          if (fall_back_on_alternate_schema(&curr_je,
                                            current_mem_root,
                                            k_start, k_end))
            return true;
        }
      }
    }
  }
  return false;
}



bool Json_schema_pattern_properties::handle_keyword(THD *thd,
                                                    MEM_ROOT *current_mem_root,
                                                    json_engine_t *je,
                                                    const char* key_start,
                                                    const char* key_end,
                                                    List<Json_schema_keyword>
                                                               *all_keywords)
{
  if (je->value_type != JSON_VALUE_OBJECT)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "patternProperties");
    return true;
  }

  str= new (thd->mem_root) Item_string(thd, "", (uint) 0, je->s.cs);

  int level= je->stack_p;
  while (json_scan_next(je)==0 && level <= je->stack_p)
  {
    switch(je->state)
    {
      case JST_KEY:
      {
        const uchar *k_end, *k_start= je->s.c_str;
        do
        {
          k_end= je->s.c_str;
        } while (json_read_keyname_chr(je) == 0);

        if (json_read_value(je))
          return true;

        st_pattern_to_property *curr_pattern_to_property= NULL;

        curr_pattern_to_property= new (thd->mem_root) pattern_to_property();
        if (curr_pattern_to_property)
        {
          my_repertoire_t repertoire= my_charset_repertoire(je->s.cs);
          curr_pattern_to_property->pattern=
                    thd->make_string_literal((const char*)k_start,
                                             (size_t)(k_end-k_start),
                                             repertoire);
          curr_pattern_to_property->re.init(je->s.cs, 0);
          curr_pattern_to_property->re.unset_flag(PCRE2_CASELESS);
          curr_pattern_to_property->curr_schema=
                       new (thd->mem_root) List<Json_schema_keyword>;

          if (curr_pattern_to_property->curr_schema)
          {
            if (create_object_and_handle_keyword(thd, current_mem_root, je,
                                    curr_pattern_to_property->curr_schema,
                                                 all_keywords))
              return true;
          }

          pattern_properties.push_back(curr_pattern_to_property, thd->mem_root);
        }
      }
    }
  }
  return false;
}

bool Json_schema_logic::handle_keyword(THD *thd,
                                       MEM_ROOT *current_mem_root,
                                       json_engine_t *je,
                                       const char* key_start,
                                       const char* key_end,
                                       List<Json_schema_keyword>
                                                 *all_keywords)
{
  json_engine_t temp_je;
  int level= je->stack_p;

  mem_root_dynamic_array_init(current_mem_root, PSI_INSTRUMENT_MEM,
                              &temp_je.stack, sizeof(int), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));

  if (je->value_type != JSON_VALUE_ARRAY)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), keyword_map->func_name.str);
    return true;
  }

  while(json_scan_next(je)==0 && je->stack_p >= level)
  {
    char *begin, *end;
    int len;

    if (json_read_value(je))
    {
      return true;
    }
    begin= (char*)je->value;

    if (json_skip_level(je))
    {
      return true;
    }

    end= (char*)je->s.c_str;
    len= (int)(end-begin);

    json_scan_start(&temp_je, je->s.cs, (const uchar *) begin,
                    (const uchar *)begin+len);
    List<Json_schema_keyword> *keyword_list=
                        new (thd->mem_root) List<Json_schema_keyword>;

    if (!keyword_list)
    {
      return true;
    }
    if (create_object_and_handle_keyword(thd, current_mem_root,
                                      &temp_je, keyword_list,
                                         all_keywords))
    {
      return true;
    }

    schema_items.push_back(keyword_list, thd->mem_root);
  }
  return false;
}

bool Json_schema_logic::check_validation(const json_engine_t *je,
                                         MEM_ROOT *current_mem_root,
                                         const uchar *k_start,
                                         const uchar *k_end)
{
  List_iterator <List<Json_schema_keyword>> it1 (schema_items);
  List<Json_schema_keyword> *curr_schema= NULL;
  Json_schema_keyword *curr_alternate_schema= NULL;
  uint count_validations= 0;
  bool validated= true;

  if (je->value_type == JSON_VALUE_ARRAY)
    curr_alternate_schema= alternate_choice1;
  else if (je->value_type == JSON_VALUE_OBJECT)
    curr_alternate_schema= alternate_choice2;

  while ((curr_schema= it1++))
  {
    List_iterator<Json_schema_keyword> it2(*curr_schema);
    Json_schema_keyword *curr_keyword= NULL;
    validated= true;

    while ((curr_keyword=it2++))
    {
      if (!curr_keyword->alternate_schema)
        curr_keyword->alternate_schema= curr_alternate_schema;
      if (curr_keyword->validate(je, current_mem_root))
      {
        validated= false;
        break;
      }
    }
    if (validated)
    {
      count_validations++;
      if (logic_flag & HAS_NOT)
        return true;
    }
  }

  if (validate_count(&count_validations, &schema_items.elements))
    return true;

  return false;
}
bool Json_schema_logic::validate(const json_engine_t *je,
                                 MEM_ROOT *current_mem_root,
                                 const uchar *k_start,
                                 const uchar* k_end)
{
  return check_validation(je, current_mem_root, k_start, k_end);
}

bool Json_schema_not::handle_keyword(THD *thd, MEM_ROOT *current_mem_root,
                                     json_engine_t *je,
                                     const char* key_start,
                                     const char* key_end,
                                     List<Json_schema_keyword>
                                               *all_keywords)
{
  bool res= false;

  if (je->value_type != JSON_VALUE_OBJECT)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), keyword_map->func_name.str);
    return true;
  }

  res= create_object_and_handle_keyword(thd, current_mem_root, je,
                          &schema_list, all_keywords);

  schema_items.push_back(&schema_list, thd->mem_root);

  return res;
}


bool Json_schema_keyword::validate_schema_items(const json_engine_t *je,
                                                MEM_ROOT *current_mem_root,
                                                List<Json_schema_keyword>
                                                             *schema_items)
{
  json_engine_t curr_je= *je;
  List_iterator<Json_schema_keyword> it1(*schema_items);
  Json_schema_keyword *curr_schema= NULL;

  while((curr_schema= it1++))
  {
    if (curr_schema->validate(&curr_je, current_mem_root))
      return true;
  }

  return false;
}

bool Json_schema_conditional::validate(const json_engine_t *je,
                                       MEM_ROOT *current_mem_root,
                                       const uchar *k_start,
                                       const uchar *k_end)
{
  if (if_cond)
  {
    if (!if_cond->validate_schema_items(je, current_mem_root,
                          if_cond->get_validation_keywords()))
    {
      if (then_cond)
      {
        if (then_cond->validate_schema_items(je, current_mem_root,
                                then_cond->get_validation_keywords()))
          return true;
      }
    }
    else
    {
      if (else_cond)
      {
        if (else_cond->validate_schema_items(je, current_mem_root,
                               else_cond->get_validation_keywords()))
          return true;
      }
    }
  }
  return false;
}


bool Json_schema_conditional::handle_keyword(THD *thd,
                                             MEM_ROOT *current_mem_root,
                                             json_engine_t *je,
                                             const char* key_start,
                                             const char* key_end,
                                             List<Json_schema_keyword>
                                                   *all_keywords)
{
  if (je->value_type != JSON_VALUE_OBJECT)
  {
     my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), keyword_map->func_name.str);
    return true;
  }
  return create_object_and_handle_keyword(thd, current_mem_root, je,
                            &conditions_schema,
                                          all_keywords);
}

bool Json_schema_dependent_schemas::handle_keyword(THD *thd,
                                                   MEM_ROOT *current_mem_root,
                                                   json_engine_t *je,
                                                   const char* key_start,
                                                   const char* key_end,
                                                   List<Json_schema_keyword>
                                                        *all_keywords)
{
  if (je->value_type != JSON_VALUE_OBJECT)
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "properties");
    return true;
  }

  if (my_hash_init(PSI_INSTRUMENT_ME,
                   &this->properties,
                   je->s.cs, 1024, 0, 0,
                   get_key_name_for_property,
                   NULL, 0))
      return true;
  is_hash_inited= true;

  int level= je->stack_p;
  while (json_scan_next(je)==0 && level <= je->stack_p)
  {
    switch(je->state)
    {
      case JST_KEY:
      {
        const uchar *k_end, *k_start= je->s.c_str;
        do
        {
          k_end= je->s.c_str;
        } while (json_read_keyname_chr(je) == 0);

        if (json_read_value(je))
          return true;

        st_property *curr_property=
                         (st_property*)alloc_root(thd->mem_root,
                                                   sizeof(st_property));
        if (curr_property)
        {
          curr_property->key_name= (char*)alloc_root(thd->mem_root,
                                                   (size_t)(k_end-k_start)+1);
          curr_property->curr_schema=
                       new (thd->mem_root) List<Json_schema_keyword>;
          if (curr_property->key_name)
          {
            curr_property->key_name[(int)(k_end-k_start)]= '\0';
            strncpy((char*)curr_property->key_name, (const char*)k_start,
                    (size_t)(k_end-k_start));
            if (create_object_and_handle_keyword(thd, current_mem_root, je,
                                             curr_property->curr_schema,
                                             all_keywords))
              return true;
            if (my_hash_insert(&properties, (const uchar*)curr_property))
              return true;
          }
        }
      }
    }
  }
  return false;
}

bool Json_schema_dependent_schemas::validate(const json_engine_t *je,
                                            MEM_ROOT *current_mem_root,
                                            const uchar *k_start,
                                            const uchar *k_end)
{
  json_engine_t curr_je= *je;

  if (curr_je.value_type != JSON_VALUE_OBJECT)
    return false;

  int level= curr_je.stack_p;
  while (json_scan_next(&curr_je)==0 && level <= curr_je.stack_p)
  {
    switch(curr_je.state)
    {
      case JST_KEY:
      {
        const uchar *k_end, *k_start= curr_je.s.c_str;
        do
        {
          k_end= curr_je.s.c_str;
        } while (json_read_keyname_chr(&curr_je) == 0);

        if (json_read_value(&curr_je))
          return true;

        st_property *curr_property= NULL;
        if ((curr_property=
                      (st_property*)my_hash_search(&properties,
                                                   (const uchar*)k_start,
                                                    (size_t)(k_end-k_start))))
        {
          if (validate_schema_items(je, current_mem_root, curr_property->curr_schema))
            return true;
          if (!json_value_scalar(&curr_je))
          {
            if (json_skip_level(&curr_je))
              return true;
          }
        }
      }
    }
  }

  return false;
}

bool Json_schema_media_string::handle_keyword(THD *thd,
                                              MEM_ROOT *current_mem_root,
                                              json_engine_t *je,
                                              const char* key_start,
                                              const char* key_end,
                                              List<Json_schema_keyword>
                                                        *all_keywords)
{
  if (je->value_type != JSON_VALUE_STRING)
  {
    String curr_keyword((char*)key_start, key_end-key_start, je->s.cs);
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), curr_keyword.ptr());
    return true;
  }

  return false;
}

bool Json_schema_reference::handle_keyword(THD *thd,
                                           MEM_ROOT *current_mem_root,
                                           json_engine_t *je,
                                           const char* key_start,
                                           const char* key_end,
                                           List<Json_schema_keyword>
                                                     *all_keywords)
{
  String keyword(0);
  keyword.append((const char*)key_start, (int)(key_end-key_start));
  my_error(ER_JSON_SCHEMA_KEYWORD_UNSUPPORTED, MYF(0), keyword.ptr());
  return true;
}

Json_schema_keyword* create_object(THD *thd,
                                   Json_schema_keyword *curr_keyword,
                                   const uchar* key_start,
                                   const uchar* key_end)
{
  st_json_schema_keyword_map *curr_keyword_map= NULL;
  curr_keyword_map=
         (st_json_schema_keyword_map*)
                              my_hash_search(&all_keywords_hash,
                                             key_start,
                                             (size_t)(key_end-key_start));
  if (!curr_keyword_map)
  {
   curr_keyword_map= &empty_func_map;
  }
  curr_keyword= (curr_keyword_map->func)(thd);
  curr_keyword->keyword_map= curr_keyword_map;

  return curr_keyword;
}

static int sort_by_priority(Json_schema_keyword* el1, Json_schema_keyword* el2,
                                 void *arg)
{
  return el1->priority > el2->priority;
}

void fix_keyword_list(List <Json_schema_keyword> *keyword_list)
{
  bubble_sort<Json_schema_keyword>(keyword_list, sort_by_priority, NULL);

  if (keyword_list && !keyword_list->is_empty())
  {
    int count= 1;

    List_iterator<Json_schema_keyword> it2(*keyword_list);
    Json_schema_keyword *curr_schema= NULL;

    while((curr_schema=it2++))
    {
      curr_schema->set_alternate_schema(keyword_list->elem(count));
        count++;
    }
  }
  return;
}

/*
  Some schemas are interdependent: they are evaluated only if their
  adjacent schemas fail to evaluate. So the need to be linked
  in a way that if one fails to evaluate a value, we can try
  an alternate schema.
  Hence push such keywords in a temporary list, adjust the interdependence
  and then add them to main schema list.
*/
bool
add_schema_interdependence(THD *thd, List<Json_schema_keyword> *temporary,
                           List<Json_schema_keyword> *keyword_list)
{
  List_iterator<Json_schema_keyword> temp_it(*temporary);
  List<Json_schema_keyword> array_prop, object_prop, logic_prop, conditional_prop;
  Json_schema_keyword *temp_keyword= NULL, *contains= NULL,
                       *max_contains= NULL, *min_contains= NULL,
                       *if_cond= NULL, *then_cond= NULL, *else_cond= NULL;

  while((temp_keyword= temp_it++))
  {
    size_t len= strlen(temp_keyword->keyword_map->func_name.str);
    st_json_schema_keyword_map *curr_element= NULL;
    if ((curr_element= (st_json_schema_keyword_map*) my_hash_search(&all_keywords_hash,
                       (uchar*)(temp_keyword->keyword_map->func_name.str), len)))
    {
      if (temp_keyword->priority > 0)
      {
        if (curr_element->flag == JSON_SCHEMA_ARRAY_KEYWORD)
          array_prop.push_back(temp_keyword);
        else if (curr_element->flag == JSON_SCHEMA_OBJECT_KEYWORD)
          object_prop.push_back(temp_keyword);
        else if (curr_element->flag == JSON_SCHEMA_LOGIC_KEYWORD)
          logic_prop.push_back(temp_keyword);
      }
      else if (temp_keyword->keyword_map == &(json_schema_func_array[35]))
          if_cond= temp_keyword;
      else if (temp_keyword->keyword_map == &(json_schema_func_array[36]))
          then_cond= temp_keyword;
      else if (temp_keyword->keyword_map == &(json_schema_func_array[37]))
          else_cond= temp_keyword;
      else if (temp_keyword->keyword_map == &(json_schema_func_array[18]))
          contains= temp_keyword;
      else if (temp_keyword->keyword_map == &(json_schema_func_array[20]))
          min_contains= temp_keyword;
      else if (temp_keyword->keyword_map == &(json_schema_func_array[19]))
          max_contains= temp_keyword;
      else
          keyword_list->push_back(temp_keyword, thd->mem_root);
    }
  }

  if (if_cond)
  {
    Json_schema_conditional *cond_schema=
                 new (current_thd->mem_root) Json_schema_conditional();
    if (cond_schema)
      cond_schema->set_conditions(if_cond, then_cond, else_cond);
    keyword_list->push_back(cond_schema, thd->mem_root);
  }
  if (contains)
  {
    contains->set_dependents(min_contains, max_contains);
    keyword_list->push_back(contains, thd->mem_root);
  }

  fix_keyword_list(&array_prop);
  fix_keyword_list(&object_prop);

  /*
    We want to check for alternate schema
    When a key is not validated by logic keywords, we would want to also check
    schema like properties, items etc to make sure the key is not validated by
    any schema in order to return correct result. So "link" other schemas as
    alternate when logic properties is present and only push logic keywords
    to the schema list.
  */
  if (!logic_prop.is_empty())
  {
    List_iterator<Json_schema_keyword> it(logic_prop);
    Json_schema_keyword *curr_schema= NULL;
    while((curr_schema= it++))
    {
      curr_schema->set_alternate_schema_choice(array_prop.elem(0),
                                               object_prop.elem(0));
      keyword_list->push_back(curr_schema, thd->mem_root);
    }
    array_prop.empty();
    object_prop.empty();
  }
  else
  {
    if (array_prop.elem(0))
      keyword_list->push_back(array_prop.elem(0), thd->mem_root);
    if (object_prop.elem(0))
      keyword_list->push_back(object_prop.elem(0), thd->mem_root);
  }
  return false;
}


/*
 Scan all keywords on the current level and put them in a temporary
 list. Once scanning is done, adjust the dependency if needed, and
 add the keywords in keyword_list
*/
bool create_object_and_handle_keyword(THD *thd, MEM_ROOT *current_mem_root,
                                      json_engine_t *je,
                                      List<Json_schema_keyword> *keyword_list,
                                      List<Json_schema_keyword> *all_keywords)
{
  int level= je->stack_p;
  List<Json_schema_keyword> temporary_list;

  DBUG_EXECUTE_IF("json_check_min_stack_requirement",
                  dbug_json_check_min_stack_requirement(); return true;);
  if (check_stack_overrun(thd, STACK_MIN_SIZE , NULL))
    return true;

  while (json_scan_next(je)== 0 && je->stack_p >= level)
  {
    switch(je->state)
    {
      case JST_KEY:
      {
        const uchar *key_end, *key_start;

        key_start= je->s.c_str;
        do
        {
          key_end= je->s.c_str;
        } while (json_read_keyname_chr(je) == 0);

        if (json_read_value(je))
         return true;

        Json_schema_keyword *curr_keyword= NULL;
        curr_keyword= create_object(thd, curr_keyword,
                                                         key_start, key_end);
        if (all_keywords)
            all_keywords->push_back(curr_keyword, thd->mem_root);
        if (curr_keyword->handle_keyword(thd, current_mem_root, je,
                                     (const char*)key_start,
                                     (const char*)key_end, all_keywords))
        {
          return true;
        }
        temporary_list.push_back(curr_keyword, thd->mem_root);
        break;
      }
    }
  }

  if (add_schema_interdependence(thd, &temporary_list, keyword_list))
    return true;

  return je->s.error ? true : false;
}

const uchar *get_key_name_for_property(const void *key_name, size_t *length,
                                       my_bool)
{
  auto curr_property= static_cast<const st_property *>(key_name);
  *length= strlen(curr_property->key_name);
  return reinterpret_cast<const uchar *>(curr_property->key_name);
}

const uchar *get_key_name_for_func(const void *key_name, size_t *length,
                                   my_bool)
{
  auto curr_keyword=
             static_cast<const st_json_schema_keyword_map *>(key_name);

  *length= curr_keyword->func_name.length;
  return reinterpret_cast<const uchar *>(curr_keyword->func_name.str);
}

bool setup_json_schema_keyword_hash()
{
  if (my_hash_init(PSI_INSTRUMENT_ME,
                   &all_keywords_hash,
                   system_charset_info, 1024, 0, 0,
                   get_key_name_for_func,
                   NULL, 0))
    return true;

  int size= sizeof(json_schema_func_array)/sizeof(json_schema_func_array[0]);
  for (int i= 0; i < size; i++)
  {
    if (my_hash_insert(&all_keywords_hash, (uchar*)(&json_schema_func_array[i])))
      return true;
  }
  return false;
}

void cleanup_json_schema_keyword_hash()
{
  my_hash_free(&all_keywords_hash);

  return;
}

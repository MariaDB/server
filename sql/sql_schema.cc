/*
 Copyright (c) 2020, MariaDB Corporation.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "mariadb.h"
#include "sql_type.h"
#include "sql_schema.h"
#include "sql_class.h"

class Schema_oracle: public Schema
{
public:
  Schema_oracle(const LEX_CSTRING &name)
   :Schema(name)
  { }
  const Type_handler *map_data_type(THD *thd, const Type_handler *src)
                                    const override
  {
    if (src == &type_handler_newdate)
      return thd->type_handler_for_datetime();
    return src;
  }

  Create_func *find_native_function_builder(THD *thd, const LEX_CSTRING &name)
                                                                         const override
  {
    return native_functions_hash_oracle.find(thd, name);
  }

  Item *make_item_func_replace(THD *thd,
                               Item *subj,
                               Item *find,
                               Item *replace) const override;
  Item *make_item_func_substr(THD *thd,
                              const Lex_substring_spec_st &spec) const override;
  Item *make_item_func_trim(THD *thd, const Lex_trim_st &spec) const override;
};


class Schema_maxdb: public Schema
{
public:
  Schema_maxdb(const LEX_CSTRING &name)
   :Schema(name)
  { }
  const Type_handler *map_data_type(THD *thd, const Type_handler *src)
                                    const override
  {
    if (src == &type_handler_timestamp ||
        src == &type_handler_timestamp2)
      return thd->type_handler_for_datetime();
    return src;
  }
};


Schema        mariadb_schema(Lex_cstring(STRING_WITH_LEN("mariadb_schema")));
Schema_oracle oracle_schema(Lex_cstring(STRING_WITH_LEN("oracle_schema")));
Schema_maxdb  maxdb_schema(Lex_cstring(STRING_WITH_LEN("maxdb_schema")));

const Schema &oracle_schema_ref= oracle_schema;

Schema *Schema::find_by_name(const LEX_CSTRING &name)
{
  DBUG_ASSERT(name.str);
  if (mariadb_schema.eq_name(name))
    return &mariadb_schema;
  if (oracle_schema.eq_name(name))
    return &oracle_schema;
  if (maxdb_schema.eq_name(name))
    return &maxdb_schema;
  return NULL;
}


Schema *Schema::find_implied(THD *thd)
{
  if (thd->variables.sql_mode & MODE_ORACLE)
    return &oracle_schema;
  if (thd->variables.sql_mode & MODE_MAXDB)
    return &maxdb_schema;
  return &mariadb_schema;
}


Create_func *
Schema::find_native_function_builder(THD *thd, const LEX_CSTRING &name) const
{
  return native_functions_hash.find(thd, name);
}


Item *Schema::make_item_func_call_native(THD *thd,
                                         const Lex_ident_sys &name,
                                         List<Item> *args) const
{
  Create_func *builder= find_native_function_builder(thd, name);
  if (builder)
    return builder->create_func(thd, &name, args);
  my_error(ER_FUNCTION_NOT_DEFINED, MYF(0), name.str);
  return NULL;
}



Item *Schema::make_item_func_replace(THD *thd,
                                     Item *subj,
                                     Item *find,
                                     Item *replace) const
{
  return new (thd->mem_root) Item_func_replace(thd, subj, find, replace);
}


Item *Schema::make_item_func_substr(THD *thd,
                                    const Lex_substring_spec_st &spec) const
{
  return spec.m_for ?
    new (thd->mem_root) Item_func_substr(thd, spec.m_subject, spec.m_from,
                                              spec.m_for) :
    new (thd->mem_root) Item_func_substr(thd, spec.m_subject, spec.m_from);
}


Item *Schema::make_item_func_trim(THD *thd, const Lex_trim_st &spec) const
{
  return spec.make_item_func_trim_std(thd);
}


Item *Schema_oracle::make_item_func_replace(THD *thd,
                                            Item *subj,
                                            Item *find,
                                            Item *replace) const
{
  return new (thd->mem_root) Item_func_replace_oracle(thd, subj, find, replace);
}


Item *Schema_oracle::make_item_func_substr(THD *thd,
                                    const Lex_substring_spec_st &spec) const
{
  return spec.m_for ?
    new (thd->mem_root) Item_func_substr_oracle(thd, spec.m_subject,
                                                     spec.m_from,
                                                     spec.m_for) :
    new (thd->mem_root) Item_func_substr_oracle(thd, spec.m_subject,
                                                     spec.m_from);
}


Item *Schema_oracle::make_item_func_trim(THD *thd,
                                         const Lex_trim_st &spec) const
{
  return spec.make_item_func_trim_oracle(thd);
}

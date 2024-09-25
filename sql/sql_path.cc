#include "sql_plugin.h"
#include "sql_class.h"    // class THD
#include "sp_head.h"      // class sp_name
#include "lex_ident.h"
#include "sql_db.h"
#include "sql_path.h"

static constexpr LEX_CSTRING cur_schema= {STRING_WITH_LEN("CURRENT_SCHEMA")};

Sql_path::Sql_path() : m_count(0), m_schemas_str({nullptr, 0})
{
}


Sql_path& Sql_path::operator=(const Sql_path &rhs)
{
  set(current_thd, rhs);
  return *this;
}


bool
is_package_public_routine(THD *thd,
                          const Lex_ident_db &db,
                          const LEX_CSTRING &package,
                          const LEX_CSTRING &routine,
                          enum_sp_type type);


bool Sql_path::resolve(THD *thd, sp_head *caller, sp_name *name,
                       const Sp_handler **sph,
                       Database_qualified_name *pkgname) const
{
  DBUG_ASSERT(name);
  DBUG_ASSERT(name->m_name.str[name->m_name.length] == '\0');

  // Check for fully qualified name schema.pkg.routine and exit early
  if (name->m_explicit_name && strchr(name->m_name.str, '.'))
    return false;

  if (!name->m_db.str || !name->m_explicit_name)
  {
    if (caller && caller->m_name.str)
    {
      if (!caller->get_package() && caller->m_name.bin_eq(name->m_name))
      {
        /*
          Standalone recursive routine
        */
        name->m_db= caller->m_db;
        name->m_explicit_name= true;
        return false;
      }

      sp_name tmp_name(*name);
      tmp_name.m_db= caller->m_db;
      const Sp_handler *pkg_routine_hndlr= nullptr;
      if ((*sph)->sp_resolve_package_routine_implicit(thd, caller, &tmp_name,
                                                      &pkg_routine_hndlr,
                                                      pkgname))
        return true;

      if (pkg_routine_hndlr)
      {
        *sph= pkg_routine_hndlr;
        *name= tmp_name;
        return false;
      }
    }
  }
  else if (thd->db.str)
  {
      const Sp_handler *pkg_routine_hndlr= nullptr;
      if ((*sph)->sp_resolve_package_routine_explicit(thd, caller, name,
                                                      &pkg_routine_hndlr,
                                                      pkgname))
        return true;
      
      if (pkg_routine_hndlr)
      {
        *sph= pkg_routine_hndlr;
        return false;
      }
      else if (!caller && false)
      {
        /*
          If the caller is NULL we are in a statement or an anonymous
          block. We need to check if the routine is a schema routine

          schema '.' routine_name
        */
        if (!(*sph)->sp_find_routine_quick(thd, name))
          return false;
      }
  }

  Database_qualified_name tmp_spname;
  tmp_spname.m_name= name->m_name;
  for (size_t i= 0; i < m_count; i++)
  {
    LEX_CSTRING schema= m_schemas[i];

    /*
      Resolve the schema name to the current database if required.
    */
    if (schema.length == cur_schema.length &&
        strncasecmp(schema.str, cur_schema.str, cur_schema.length) == 0)
    {
      Lex_ident_db_normalized dbn;
      if (caller && caller->m_name.str)
      {
        dbn= thd->to_ident_db_normalized_with_error(caller->m_db);
      }
      else
      {
        dbn= thd->copy_db_normalized(false);
      }

      if (unlikely(!dbn.str))
        continue;
      
      schema.str= dbn.str;
      schema.length= dbn.length;
    }

    DBUG_ASSERT(schema.str);
    
    const Lex_ident_db_normalized dbn=
        thd->to_ident_db_normalized_with_error(schema);
    if (unlikely(!dbn.str))
      continue;
    
    if (!check_db_dir_existence(dbn.str))
    {
      if (!name->m_explicit_name)
      {
        tmp_spname.m_db= dbn;
        if (!(*sph)->sp_find_routine_quick(thd, &tmp_spname))
        {
          /*
            [schema] '.' routine_name
          */
          name->m_db= dbn;
          name->m_explicit_name= true;
          return false;
        }
      }
      else
      {
        if (is_package_public_routine(thd, dbn, name->m_db, name->m_name, (*sph)->type()))
        {
          /*
            [schema] '.' pkg_name '.' routine_name routine
          */
          pkgname->m_db= dbn;
          pkgname->m_name= Lex_ident_routine(name->m_db);
          *sph= (*sph)->package_routine_handler();
          return name->make_package_routine_name(thd->mem_root, dbn,
                                                  name->m_db, name->m_name);
        }
      }
    }
  }

  return false;
}


void Sql_path::free()
{
  for (size_t i= 0; i < m_count; i++)
  {
    my_free((void*)m_schemas[i].str);
  }

  m_count= 0;

  if (m_schemas_str.str && m_schemas_str.str != cur_schema.str)
    my_free(m_schemas_str.str);
  
  m_schemas_str.str= nullptr;
  m_schemas_str.length= 0;
}


bool Sql_path::init()
{
  free();
  add_schema_direct(cur_schema.str, cur_schema.length);
  m_schemas_str.str= (char*)cur_schema.str;
  m_schemas_str.length= cur_schema.length;

  return false;
}


bool Sql_path::is_cur_schema(const LEX_CSTRING &schema) const
{
  return schema.length == cur_schema.length &&
          !memcmp(schema.str, cur_schema.str, schema.length);
}


bool Sql_path::add_schema_direct(const char *schema_str, size_t schema_len)
{
  if (unlikely(m_count >= array_elements(m_schemas)))
  {
    my_error(ER_VALUE_TOO_LONG , MYF(0), "path");
    return true;
  }

  char *tmp= my_strndup(key_memory_Sys_var_charptr_value,
                        schema_str, schema_len,
                        MYF(MY_WME));
  m_schemas[m_count++]= {tmp, schema_len};
  return false;
}


bool Sql_path::add_schema(THD *thd, const char *schema_str, size_t schema_len)
{
  DBUG_ASSERT(schema_str);

  if (unlikely(m_count >= array_elements(m_schemas)))
  {
    my_error(ER_VALUE_TOO_LONG , MYF(0), "path");
    return true;
  }
  
  char *tmp= my_strndup(key_memory_Sys_var_charptr_value,
                        schema_str, schema_len,
                        MYF(MY_WME));
  if (unlikely(!tmp))
    return true;
  size_t new_len= 0;

  /*
    Escape double backticks.
  */
  for (size_t i= 0; i < schema_len; i++)
  {
    if (schema_str[i] == '`' && i + 1 < schema_len)
      i++;

    tmp[new_len++]= schema_str[i];
  }
  tmp[new_len]= '\0';

  schema_str= tmp;
  schema_len= new_len;

  Lex_ident_db_normalized dbn= thd->
      to_ident_db_normalized_with_error(Lex_cstring(schema_str, schema_len));
  if (!dbn.str)
  {
    my_free(tmp);
    return true;
  }

  if (dbn.str != schema_str)
  {
    /*
      to_ident_db_normalized_with_error() allocates on the THD mem_root.
    */
    my_free((void*)schema_str);
    schema_str= my_strndup(key_memory_Sys_var_charptr_value,
                          dbn.str, dbn.length,
                          MYF(MY_WME));
    if (unlikely(!schema_str))
      return true;
  }
  else
  {
    schema_str= dbn.str;
    DBUG_ASSERT(schema_len == dbn.length);
  }

  schema_len= dbn.length;

  /*
    SQL:2016
    No two <schema name>s contained in <schema name list> shall be equivalent.
  */
  for (size_t i= 0; i < m_count; i++)
  {
    if (unlikely(m_schemas[i].length == schema_len &&
        !memcmp(m_schemas[i].str, schema_str, schema_len)))
    {
      my_error(ER_WRONG_VALUE_FOR_VAR , MYF(0), "path", schema_str);
      my_free((void*)schema_str);
      return true;
    }
  }

  m_schemas[m_count++]= {schema_str, schema_len};
  return false;
}


void Sql_path::set(THD *thd, const Sql_path &rhs)
{
  free();

  for (size_t i= 0; i < rhs.m_count; i++)
  {
    if (add_schema(thd, rhs.m_schemas[i].str,
                   rhs.m_schemas[i].length))
      break;
  }
  m_count= rhs.m_count;
  m_schemas_str= {
    (char *) my_strdup(PSI_INSTRUMENT_ME, rhs.m_schemas_str.str,
                       MYF(MY_WME | MY_ZEROFILL)),
    rhs.m_schemas_str.length
  };
}


void Sql_path::set(Sql_path &&rhs)
{
  size_t count= std::max(m_count, rhs.m_count);
  for (size_t i= 0; i < count; i++)
    std::swap(m_schemas[i], rhs.m_schemas[i]);
  std::swap(m_count, rhs.m_count);
  std::swap(m_schemas_str, rhs.m_schemas_str);
  rhs.free();
}


bool Sql_path::from_text(THD *thd, CHARSET_INFO *cs, const LEX_CSTRING &text)
{
  free();

  enum class tokenize_state
  {
    START,
    QUOTED_TOKEN_DOUBLE,
    QUOTED_TOKEN_BACKTICK,
    UNQUOTED_TOKEN,
    END
  } state= tokenize_state::START;
  
  auto *curr= text.str;
  auto *end= curr + text.length;
  auto token_start= curr;
  auto token_end= curr;
  auto last_non_space= curr;
  const bool ansi_quotes= thd ?
    thd->variables.sql_mode & MODE_ANSI_QUOTES : false;
   
  while (curr != end)
  {
    auto len = my_ismbchar(cs, curr, end - 1);
    if (len)
    {
      if (state == tokenize_state::START)
      {
        state= tokenize_state::UNQUOTED_TOKEN;
        token_start= curr;
      }

      curr += len;
      last_non_space= curr - 1;
      if (curr < end)
        continue;
    }
    else if (unlikely(!ansi_quotes && *curr == '"' &&
             state != tokenize_state::QUOTED_TOKEN_BACKTICK))
    {
      /*
        ANSI_QUOTES is not set, only allow double quotes within backticks.
      */
      my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), "path", text.str);
      return true;
    }

    switch (state)
    {
      case tokenize_state::START:
        if (*curr == '`' || (ansi_quotes && *curr == '"'))
        {
          state= *curr == '`' ? tokenize_state::QUOTED_TOKEN_BACKTICK :
                                tokenize_state::QUOTED_TOKEN_DOUBLE;
          token_start= ++curr;
        }
        else if (*curr == ',' || my_isspace(cs, (uchar) *curr))
        {
          curr++;
        }
        else
        {
          state= tokenize_state::UNQUOTED_TOKEN;
          last_non_space= token_start= curr++;
        }
        break;
      case tokenize_state::QUOTED_TOKEN_BACKTICK:
        if (*curr == '`')
        {
          if (curr + 1 < end && curr[1] == '`')
          {
            /* Looked-ahead and found double backtick */
            curr += 2;
            last_non_space= curr - 1;
          }
          else
          {
            state= tokenize_state::END;
            token_end= last_non_space + 1;
            curr++;
          }
        }
        else
        {
          if (!my_isspace(cs, (uchar) *curr))
            last_non_space= curr;
          curr++;
        }
        break;
      case tokenize_state::QUOTED_TOKEN_DOUBLE:
        if (*curr == '"')
        {
          state= tokenize_state::END;
          token_end= last_non_space + 1;
          curr++;
        }
        else
        {
          if (!my_isspace(cs, (uchar) *curr))
            last_non_space= curr;
          curr++;
        }
        break;
      case tokenize_state::UNQUOTED_TOKEN:
        if (*curr == ',')
        {
          state= tokenize_state::END;
          token_end= last_non_space + 1;
          curr++;
        }
        else if (unlikely(*curr == '`' || *curr == '"'))
        {
          my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), "path", text.str);
          return true;
        }
        else
        {
          if (!my_isspace(cs, (uchar) *curr))
            last_non_space= curr;
          curr++;
        }
        break;
      case tokenize_state::END:
        break;
    }

    if (state == tokenize_state::END)
    {
      if (token_end > token_start)
      {
        if (unlikely(add_schema(thd, token_start,
                                (size_t)(token_end - token_start))))
          return true;
      }

      state= tokenize_state::START;
    }
  }

  if (state == tokenize_state::UNQUOTED_TOKEN)
  {
    token_end= last_non_space + 1;
    auto len= (size_t)(token_end - token_start);

    if (len && add_schema(thd, token_start, len))
      return true;
  }
  else if (unlikely(state == tokenize_state::QUOTED_TOKEN_BACKTICK ||
                    state == tokenize_state::QUOTED_TOKEN_DOUBLE))
  {
    /*
      Unclosed quoted string.
    */
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), "path", text.str);
    return true;
  }

  // TODO
  m_schemas_str.length= text_format_nbytes_needed(thd, false);
  m_schemas_str.str= (char *) my_malloc(PSI_INSTRUMENT_ME,
                                        m_schemas_str.length,
                                        MYF(MY_WME | MY_ZEROFILL));
  m_schemas_str.length= print(thd, false,
                              m_schemas_str.str, m_schemas_str.length);

  return false;
}


size_t Sql_path::text_format_nbytes_needed(THD *thd, bool resolve) const
{
  size_t nbytes= 0;

  for (size_t i= 0; i < m_count; i++)
  {
    LEX_CSTRING schema= m_schemas[i];
    if (resolve && is_cur_schema(schema) &&
        !thd->check_if_current_db_is_set_with_error(false))
    {
      /*
        If the schema is the current schema, we need to replace it with
        the current database name.
      */
      schema.str= thd->get_db();
      schema.length= strlen(schema.str);
    }
    size_t schema_length= schema.length;
    for (size_t j= 0; j < schema.length; j++)
    {
      if (schema.str[j] == '`')
        schema_length++;
    }

    nbytes+= schema_length + 2 + 1;
  }

  if (nbytes)
    nbytes--;

  return nbytes + 1;
}


size_t Sql_path::print(THD *thd, bool resolve,
                       char *dst, size_t nbytes_available) const
{
  size_t nbytes= 0;

  for (size_t i= 0; i < m_count; i++)
  {
    LEX_CSTRING schema= m_schemas[i];
    if (resolve && is_cur_schema(schema) &&
        !thd->check_if_current_db_is_set_with_error(false))
    {
      /*
        If the schema is the current schema, we need to replace it with
        the current database name.
      */
      schema.str= thd->get_db();
      schema.length= strlen(schema.str);
    }

    size_t len= schema.length;
    if (nbytes + len + 3 > nbytes_available)
      break;

    *dst++= '`';
    for (size_t j= 0; j < schema.length; j++)
    {
      *dst++= schema.str[j];
      if (schema.str[j] == '`')
      {
        *dst++= '`';
        len++;
      }
    }
    *dst++= '`';
    *dst++= ',';
    nbytes+= len + 3;
  }

  if (nbytes)
  {
    nbytes--;
    dst--;
  }

  if (nbytes < nbytes_available)
    *dst= '\0';
    
  return nbytes;
}


LEX_CSTRING Sql_path::lex_cstring(THD *thd) const
{
  LEX_CSTRING res;
  size_t nbytes_needed= text_format_nbytes_needed(thd, true);
  char *ptr= (char *) alloc_root(thd->mem_root, nbytes_needed);
  if (res.str)
  {
    res.length= print(thd, true, ptr, nbytes_needed);
    res.str= ptr;
    DBUG_ASSERT(res.length < nbytes_needed);
  }
  else
  {
    res.str= nullptr;
    res.length= 0;
  }

  return res;
}


bool Sql_path_stack::push_path(CHARSET_INFO *cs,
                               const LEX_CSTRING &path_str,
                               bool *was_pushed)
{
  if (!path_str.length)
    return false;
  
  auto cur_path= (Sql_path *) my_malloc(PSI_INSTRUMENT_ME,
                                  sizeof(Sql_path), MYF(MY_WME | MY_ZEROFILL));
  if (unlikely(!cur_path))
    return true;
  
  *cur_path= std::move(m_thd->variables.path);
  m_path_stack.push_front(cur_path);

  if (unlikely(m_thd->variables.path.from_text(m_thd, cs, path_str)))
  {
    pop_path();
    return true;
  }

  *was_pushed= true;

  return false;
}


bool Sql_path_stack::pop_path()
{
  if (unlikely(m_path_stack.is_empty()))
    return true;

  auto prev_path= m_path_stack.pop();
  if (unlikely(!prev_path))
    return true;

  m_thd->variables.path= std::move(*prev_path);
  my_free(prev_path);

  return false;
}


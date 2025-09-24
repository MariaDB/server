#include "sql_plugin.h"
#include "sql_class.h"    // class THD
#include "sp_head.h"      // class sp_name
#include "lex_ident.h"
#include "sql_db.h"
#include "sql_path.h"

static constexpr LEX_CSTRING cur_schema= {STRING_WITH_LEN("CURRENT_SCHEMA")};

Sql_path::Sql_path() : m_count(0)
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


bool Sql_path::resolve_recursive_routine(sp_head *caller, sp_name *name) const
{
  if (!caller || !caller->m_name.str)
    return false;

  if (caller->get_package() || !caller->m_name.bin_eq(name->m_name))
    return false;

  /*
    Standalone recursive routine
  */
  name->m_db= caller->m_db;
  return true;
}


LEX_CSTRING Sql_path::resolve_current_schema(THD *thd, sp_head *caller,
                                              const LEX_CSTRING &schema) const
{
  /*
    Check if this is the CURRENT_SCHEMA token
  */
  if (!is_cur_schema(schema))
    return schema;

  /*
    Resolve CURRENT_SCHEMA to actual database name.
  */
  Lex_ident_db_normalized dbn;
  if (caller && caller->m_name.str)
    dbn= thd->to_ident_db_normalized_with_error(caller->m_db);
  else if (thd->db.str || thd->lex->sphead)
    dbn= thd->copy_db_normalized();

  // If neither condition is met or oom, dbn.str remains null
  if (unlikely(!dbn.str))
    return {nullptr, 0};

  return {dbn.str, dbn.length};
}


bool Sql_path::try_resolve_in_schema(THD *thd, const Lex_ident_db_normalized &schema,
                                      sp_name *name, const Sp_handler **sph,
                                      Database_qualified_name *pkgname,
                                      bool *resolved) const
{
  DBUG_ASSERT(resolved);

  if (check_db_dir_existence(schema.str))
    return false; // Schema doesn't exist

  Database_qualified_name tmp_spname;
  tmp_spname.m_name= name->m_name;

  if (!name->m_explicit_name)
  {
    Parser_state *oldps= thd->m_parser_state;
    thd->m_parser_state= NULL;

    // Try to find routine directly in schema
    tmp_spname.m_db= schema;
    bool found= (*sph)->sp_find_routine(thd, &tmp_spname, false) != NULL;
    thd->m_parser_state= oldps;

    if (found)
    {
      /*
        [schema] '.' routine_name
      */
      name->m_db= Lex_ident_db_normalized(thd->strmake(schema.str, schema.length), 
                                          schema.length);
      *resolved= true;
      return false;
    }
  }
  else
  {
    // Try package routine resolution
    if (is_package_public_routine(thd, schema, name->m_db, name->m_name, (*sph)->type()))
    {
      /*
        [schema] '.' pkg_name '.' routine_name routine
      */
      pkgname->m_db= schema;
      pkgname->m_name= Lex_ident_routine(name->m_db);
      *sph= (*sph)->package_routine_handler();
      *resolved= true;
      return name->make_package_routine_name(thd->mem_root, schema,
                                             name->m_db, name->m_name);
    }
  }

  return false;
}


bool Sql_path::resolve(THD *thd, sp_head *caller, sp_name *name,
                       const Sp_handler **sph,
                       Database_qualified_name *pkgname) const
{
  DBUG_ASSERT(name);
  DBUG_ASSERT(name->m_name.str[name->m_name.length] == '\0');

  struct resolving {
    THD *m_thd;
    ulonglong m_accessed_rows_and_keys;
    resolving(THD *thd) : m_thd(thd)
    {
      m_thd->m_is_resolving= true;
      m_accessed_rows_and_keys= m_thd->accessed_rows_and_keys;
    }
    ~resolving()
    {
      m_thd->m_is_resolving= false;
      m_thd->accessed_rows_and_keys= m_accessed_rows_and_keys;
    }
  } resolving(thd);

  // Check for fully qualified name schema.pkg.routine and exit early
  if (name->m_explicit_name && strchr(name->m_name.str, '.'))
    return false;

  if (!name->m_db.str || !name->m_explicit_name)
  {
    // Implicit name
    if (caller && caller->m_name.str)
    {
      if (resolve_recursive_routine(caller, name))
        return false;

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
    // Explicit name and current database is set
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
  }

  // If PATH contains only CURRENT_SCHEMA (default), skip PATH resolution
  // to avoid extra table operations that affect performance
  if (m_count == 1 && is_cur_schema(m_schemas[0]))
    return false;

  bool resolved= false;
  for (size_t i= 0; i < m_count; i++)
  {
    LEX_CSTRING schema= m_schemas[i];
    
    // Resolve CURRENT_SCHEMA if needed
    schema= resolve_current_schema(thd, caller, schema);
    if (!schema.str)
      continue; // CURRENT_SCHEMA resolution failed, skip this entry
    
    /*
      Schemas are already normalized when added to the path, except for 
      CURRENT_SCHEMA which was resolved above. We can use them directly.
    */
    const Lex_ident_db_normalized dbn(schema.str, schema.length);
    
    if (try_resolve_in_schema(thd, dbn, name, sph, pkgname, &resolved))
      return true;
    
    if (resolved)
      break;
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
}


bool Sql_path::init()
{
  free();
  add_schema_direct(cur_schema.str, cur_schema.length);

  return false;
}


bool Sql_path::is_cur_schema(const LEX_CSTRING &schema) const
{
  return schema.length == cur_schema.length &&
         !strncasecmp(schema.str, cur_schema.str, schema.length);
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

  if (is_cur_schema({schema_str, schema_len}))
    return add_schema_direct(schema_str, schema_len);

  // Step 1: Process backticks in-place using a local buffer
  char local_buf[NAME_LEN * 3];  // Sufficient for any schema name
  if (schema_len >= sizeof(local_buf))
  {
    my_error(ER_VALUE_TOO_LONG, MYF(0), "path");
    return true;
  }

  size_t processed_len = 0;
  for (size_t i = 0; i < schema_len; i++)
  {
    if (schema_str[i] == '`' && i + 1 < schema_len)
      i++; // Skip first backtick of double backtick
    local_buf[processed_len++] = schema_str[i];
  }
  local_buf[processed_len] = '\0';

  // Step 2: Normalize the processed string
  Lex_ident_db_normalized dbn = thd->to_ident_db_normalized_with_error(
      Lex_cstring(local_buf, processed_len));
  if (!dbn.str)
    return true;

  // Step 3: Check for duplicates
  for (size_t i = 0; i < m_count; i++)
  {
    if (unlikely(m_schemas[i].length == dbn.length &&
        !memcmp(m_schemas[i].str, dbn.str, dbn.length)))
    {
      my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), "path", dbn.str);
      return true;
    }
  }

  // Step 4: Make persistent copy and store
  char *persistent_copy = my_strndup(key_memory_Sys_var_charptr_value,
                                     dbn.str, dbn.length, MYF(MY_WME));
  if (unlikely(!persistent_copy))
    return true;

  m_schemas[m_count++] = {persistent_copy, dbn.length};
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
}


void Sql_path::set(Sql_path &&rhs)
{
  size_t count= std::max(m_count, rhs.m_count);
  for (size_t i= 0; i < count; i++)
    std::swap(m_schemas[i], rhs.m_schemas[i]);
  std::swap(m_count, rhs.m_count);
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


  return false;
}


size_t Sql_path::text_format_nbytes_needed(THD *thd, bool resolve) const
{
  size_t nbytes= 0;

  for (size_t i= 0; i < m_count; i++)
  {
    LEX_CSTRING schema= m_schemas[i];
    if (resolve && is_cur_schema(schema) && thd->db.str != nullptr)
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
    if (resolve && is_cur_schema(schema) && thd->db.str != nullptr)
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


LEX_CSTRING Sql_path::lex_cstring(THD *thd, MEM_ROOT *mem_root) const
{
  LEX_CSTRING res;
  size_t nbytes_needed= text_format_nbytes_needed(thd, true);
  char *ptr= (char *) alloc_root(mem_root, nbytes_needed);
  if (ptr)
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


#include "sql_plugin.h"
#include "sql_class.h"    // class THD
#include "sp_head.h"      // class sp_name
#include "lex_ident.h"
#include "sql_db.h"
#include "sql_path.h"

static constexpr Lex_ident_ci cur_schema= {STRING_WITH_LEN("CURRENT_SCHEMA")};

Sql_path::Sql_path() : m_count(0)
{
}


Sql_path& Sql_path::operator=(const Sql_path &rhs)
{
  set(current_thd, rhs);
  return *this;
}


Lex_ident_db Sql_path::resolve_current_schema(THD *thd, sp_head *caller,
                                              size_t i) const
{
  if (!is_cur_schema(i))
    return m_schemas[i];

  /*
    Resolve CURRENT_SCHEMA to actual database name.
  */
  Lex_ident_db_normalized dbn;
  if (caller && caller->m_name.str)
    dbn= thd->to_ident_db_normalized_with_error(caller->m_db);
  else if (thd->db.str || thd->lex->sphead)
    dbn= thd->copy_db_normalized();

  // If neither condition is met or oom, dbn.str remains null
  if (!dbn.str)
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

  // Check for fully qualified name schema.pkg.routine and exit early
  if (name->m_explicit_name && strchr(name->m_name.str, '.'))
    return false;

  DBUG_ASSERT(!name->m_explicit_name || name->m_db.str);
  if (!name->m_explicit_name)
  {
    // Implicit name
    if (caller && caller->m_name.str)
    {
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
  if (m_count == 1 && is_cur_schema(0))
    return false;

  bool resolved= false;
  for (size_t i= 0; i < m_count; i++)
  {
    Lex_ident_db schema= resolve_current_schema(thd, caller, i);
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
  if (m_count)
    my_free(const_cast<char*>(m_schemas[0].str));
  m_count= 0;
}


bool Sql_path::init()
{
  free();
  char *buf= my_strndup(key_memory_Sys_var_charptr_value, "", 1, MYF(MY_WME));
  m_schemas[m_count++]= {buf, 0};
  return false;
}


bool Sql_path::is_cur_schema(size_t i) const
{
  return !m_schemas[i].length;
}


bool Sql_path::add_schema(char **to, bool is_quoted)
{
  const Lex_ident_db &dbn= m_schemas[m_count];
  m_schemas[m_count].length= *to - m_schemas[m_count].str;
  *(*to)++= 0;

  // Validate
  if (Lex_ident_db::check_name_with_error(dbn))
    goto err;

  if (!is_quoted && cur_schema.streq(dbn))
  {
    for (size_t i = 0; i < m_count; i++) // Check for duplicates
      if (is_cur_schema(i))
        goto err;
    m_schemas[m_count].length= 0;
  }
  else
  {
    for (size_t i = 0; i < m_count; i++)
      if (!is_cur_schema(i) && dbn.streq(m_schemas[i]))
        goto err;
  }

  m_count++;
  return false;
err:
  return true;
}


void Sql_path::set(THD *thd, const Sql_path &rhs)
{
  free();

  if ((m_count= rhs.m_count))
  {
    auto rbuf= rhs.m_schemas[0].str;
    auto rend= rhs.m_schemas[m_count-1].str + rhs.m_schemas[m_count-1].length;
    auto buf= (const char*)my_memdup(key_memory_Sys_var_charptr_value, rbuf,
                                     rend - rbuf + 1, MYF(MY_WME));
    for (size_t i= 0; i < rhs.m_count; i++)
      m_schemas[i]= { rhs.m_schemas[i].str - rbuf + buf, rhs.m_schemas[i].length };
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


bool Sql_path::from_text(const system_variables &sv, const LEX_CSTRING &text)
{
  enum tokenize_state
  {
    START, QUOTED_TOKEN_DOUBLE='"', QUOTED_TOKEN_BACKTICK='`', UNQUOTED_TOKEN, END
  } state= START;

  const bool ansi_quotes= sv.sql_mode & MODE_ANSI_QUOTES;

  CHARSET_INFO *cs= &my_charset_utf8mb3_general_ci; // as in make_ident_casedn()
  DBUG_ASSERT(cs->cset->casedn_multiply(cs) == 1);
  char *buf= (char*)my_malloc(key_memory_Sys_var_charptr_value,
                              text.length + 1, MYF(MY_WME));
  if (!buf)
    return true;

  char *curr= buf, *to= buf, *end;

  if (lower_case_table_names > 0)
    end= buf + cs->cset->casedn(cs, text.str, text.length, buf, text.length);
  else
  {
    memcpy(buf, text.str, text.length);
    end= buf + text.length;
  }

  free();
  while (curr < end)
  {
    auto len = cs->charlen(curr, end);

    switch (state)
    {
      case START:
        if (*curr == ',' || my_isspace(cs, (uchar) *curr))
          curr++;
        else
        {
          if (m_count >= array_elements(m_schemas))
          {
            my_error(ER_VALUE_TOO_LONG, MYF(0), "PATH");
            goto err;
          }
          m_schemas[m_count].str= to;
          if (*curr == '`' || (*curr == '"' && ansi_quotes))
            state= (tokenize_state) *curr++;
          else if (*curr == '"')
            goto err_bad_val;
          else
          {
            state= UNQUOTED_TOKEN;
            while (len--)
              *to++= *curr++;
          }
        }
        break;
      case QUOTED_TOKEN_BACKTICK:
      case QUOTED_TOKEN_DOUBLE:
        if (*curr == state)
        {
          curr++;
          if (curr >= end || *curr != state)
          {
            state= END;
            if (add_schema(&to, true))
              goto err_bad_val;
            break;
          }
        }
        while (len--)
          *to++= *curr++;
        break;
      case UNQUOTED_TOKEN:
        if (*curr == ',' || my_isspace(cs, (uchar) *curr))
        {
          state= *curr++ == ',' ? START : END;
          if (add_schema(&to, false))
            goto err_bad_val;
          break;
        }
        else if (*curr == '`' || *curr == '"')
          goto err_bad_val;
        else
          while (len--)
            *to++= *curr++;
        break;
      case END:
        if (*curr == ',')
          state= START;
        else if (!my_isspace(cs, (uchar) *curr))
          goto err_bad_val;
        curr++;
        break;
    }
  }

  switch (state)
  {
    case START:
    case END:
      break;
    case QUOTED_TOKEN_BACKTICK:
    case QUOTED_TOKEN_DOUBLE:
      goto err_bad_val;
    case UNQUOTED_TOKEN:
      if (add_schema(&to, false))
        goto err_bad_val;
      break;
  }
  if (!m_count)
    my_free(buf);
  return false;

err_bad_val:
  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), "PATH", text.str);
err:
  m_count= 0;
  my_free(buf);
  return true;
}


LEX_CSTRING Sql_path::get_schema_for_print(size_t num, const LEX_CSTRING &db,
                                           bool resolve, bool *seen_current) const
{
  if (is_cur_schema(num))
  {
    if (!resolve)
      return cur_schema;
    if (*seen_current || !db.length)
      return null_clex_str;
    *seen_current= true;
    return db;
  }
  if (resolve && db.length && Lex_ident_db(db).streq(m_schemas[num]))
  {
    if (*seen_current)
      return null_clex_str;
    *seen_current= true;
  }
  return m_schemas[num];
}


size_t Sql_path::text_format_nbytes_needed(THD *thd, bool resolve) const
{
  size_t nbytes= 0;
  bool seen= false;

  for (size_t i= 0; i < m_count; i++)
  {
    LEX_CSTRING schema= get_schema_for_print(i, thd->db, resolve, &seen);
    if (!schema.length)
      continue;

    size_t len= schema.length;
    for (size_t j= 0; j < schema.length; j++)
    {
      if (schema.str[j] == '`')
        len++;
    }

    nbytes+= len + 2 + 1;
  }

  if (nbytes)
    nbytes--;

  return nbytes + 1;
}


size_t Sql_path::print(THD *thd, bool resolve,
                       char *dst, size_t nbytes_available) const
{
  char *start= dst;
  bool seen= false;

  for (size_t i= 0; i < m_count; i++)
  {
    LEX_CSTRING schema= get_schema_for_print(i, thd->db, resolve, &seen);
    if (!schema.length)
      continue;

    if (dst - start + schema.length + 3 > nbytes_available)
      break;

    if (!resolve && !seen && schema.str == cur_schema.str)
    {
      memcpy(dst, schema.str, schema.length);
      dst+= schema.length;
      *dst++= ',';
      seen= true;
      continue;
    }

    *dst++= '`';
    for (size_t j= 0; j < schema.length; j++)
    {
      *dst++= schema.str[j];
      if (schema.str[j] == '`')
        *dst++= '`';
    }
    *dst++= '`';
    *dst++= ',';
  }

  if (dst > start)
    dst--;

  if (dst < start + nbytes_available)
    *dst= '\0';

  return dst - start;
}


LEX_CSTRING Sql_path::lex_cstring(THD *thd, MEM_ROOT *mem_root) const
{
  LEX_CSTRING res;
  const bool resolve= false;
  size_t nbytes_needed= text_format_nbytes_needed(thd, resolve);
  char *ptr= (char *) alloc_root(mem_root, nbytes_needed);
  if (ptr)
  {
    res.length= print(thd, resolve, ptr, nbytes_needed);
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

Sql_path_instant_set::Sql_path_instant_set(THD *thd, const LEX_CSTRING &str)
  : m_thd(thd), m_path(std::move(thd->variables.path))
{
  if (thd->variables.path.from_text(thd->variables, str))
  {
    thd->variables.path.set(std::move(m_path));
    m_thd= NULL;
  }
}

Sql_path_instant_set::Sql_path_instant_set(THD *thd, const Sql_path &new_path)
  : m_thd(thd), m_path(std::move(thd->variables.path))
{
  thd->variables.path.set(thd, new_path);
}

Sql_path_instant_set::~Sql_path_instant_set()
{
  m_thd->variables.path.set(std::move(m_path));
}

#include "sql_plugin.h"
#include "sql_class.h"    // class THD
#include "sp_head.h"      // class sp_name
#include "lex_ident.h"
#include "sql_db.h"
#include "sql_path.h"

static constexpr LEX_CSTRING cur_schema= {STRING_WITH_LEN(".")};

Sql_path::Sql_path() :
  m_schemas(m_schemas_array,
            sizeof(m_schemas_array) / sizeof(m_schemas_array[0])),
  m_count(0),
  m_buffer(nullptr),
  m_buffer_length(0)
{
}


bool Sql_path::find_db_unqualified(THD *thd, const LEX_CSTRING &name,
                                   const Sp_handler *sph,
                                   Lex_ident_db_normalized *dbn_out,
                                   sp_name **spname_out) const
{
  for (size_t i= 0; i < m_count; i++)
  {
    LEX_CSTRING schema= m_schemas[i];

    /*
      Resolve the schema name to the current database if required.
    */
    if (schema.length == cur_schema.length &&
                        !memcmp(schema.str, cur_schema.str, cur_schema.length))
      schema= thd->db;

    if (!schema.str)
      continue;

    if (likely(!check_db_dir_existence(schema.str)))
    {
      const Lex_ident_db_normalized dbn=
        thd->to_ident_db_normalized_with_error(schema);
      if (dbn.str)
      {
        sp_name *spname= new (thd->mem_root) sp_name(dbn, name, false);
        if (unlikely(!spname))
          return true;

        if (!sph->sp_find_routine_quick(thd, spname))
        {
          // found
          if (dbn_out)
            *dbn_out= dbn;
          if (spname_out)
            *spname_out= spname;

          break;
        }
      }
    }
  }

  return false;
}


bool Sql_path::find_db_qualified(THD *thd,
                                 sp_name *name,
                                 const Sp_handler **sph,
                                 Database_qualified_name *pkgname) const
{
  for (size_t i= 0; i < m_count; i++)
  {
    LEX_CSTRING schema= m_schemas[i];
    /*
      Resolve the schema name to the current database if required.
    */
    if (schema.length == cur_schema.length &&
        !memcmp(schema.str, cur_schema.str, cur_schema.length))
      schema= thd->db;

    if (unlikely(!schema.str))
      continue;
    
    if (likely(!check_db_dir_existence(schema.str)))
    {
      const Lex_ident_db_normalized dbn=
        thd->to_ident_db_normalized_with_error(schema);
      if (dbn.str)
      {
        if (!((*sph)->sp_find_qualified_routine(thd, dbn, name)))
        {
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


Schema *Sql_path::find_first_internal_schema() const
{
  for (size_t i= 0; i < m_count; i++)
  {
    if (Schema *schema= Schema::find_by_name(m_schemas[i]))
      return schema;
  }

  return nullptr;
}


void Sql_path::free()
{
  m_count= 0;

  if (m_buffer)
  {
    my_free(m_buffer);
    m_buffer= nullptr;
    m_buffer_length= 0;
  }
}


bool Sql_path::init()
{
  init_array();
  free();

  return false;
}


bool Sql_path::add_schema(const char *schema_str, size_t schema_len)
{
  DBUG_ASSERT(schema_str);

  if (unlikely(m_count >= m_schemas.size()))
    return true;
  
  /*
    Disallow duplicate schema names. The intent here is to avoid
    ambiguity in the order of schema resolution when the path
    contains duplicate schema names.
  */
  for (size_t i= 0; i < m_count; i++)
  {
    if (unlikely(m_schemas[i].length == schema_len &&
        !memcmp(m_schemas[i].str, schema_str, schema_len)))
    {
      my_error(ER_INVALID_SCHEMA_NAME_LIST_SPEC, MYF(0));
      return true;
    }
  }

  m_schemas[m_count++]= {schema_str, schema_len};
  return false;
}


void Sql_path::set(const Sql_path &rhs, uint version_increment)
{
  init_array();
  free();

  if (rhs.m_buffer)
  {
    /*
      Create a copy of buffer. All schema names will be readjusted
      to point to the new buffer.
    */
    m_buffer= my_strndup(key_memory_Sys_var_charptr_value,
                        rhs.m_buffer, rhs.m_buffer_length,
                        MYF(MY_WME));
    if (!m_buffer)
      return;
    m_buffer_length= rhs.m_buffer_length;
  }

  for (size_t i= 0; i < rhs.m_count; i++)
  {
    if (add_schema(rhs.m_schemas[i].str - rhs.m_buffer + m_buffer,
                   rhs.m_schemas[i].length))
      break;
  }
}


void Sql_path::set(Sql_path &&rhs, uint version_increment)
{
  init_array();
  free();

  m_buffer= rhs.m_buffer;
  m_buffer_length= rhs.m_buffer_length;
  m_count= rhs.m_count;

  memcpy(m_schemas_array, rhs.m_schemas_array,
         sizeof(m_schemas_array[0]) * m_count);

  rhs.m_buffer= nullptr;
  rhs.m_buffer_length= 0;
  rhs.m_count= 0;
}


bool Sql_path::from_text(THD *thd, CHARSET_INFO *cs, const LEX_CSTRING &text)
{
  free();

  m_buffer= my_strndup(key_memory_Sys_var_charptr_value,
                       text.str, text.length,
                       MYF(MY_WME));
  if (unlikely(!m_buffer))
    return true;
  m_buffer_length= text.length;

  enum class tokenize_state
  {
    START,
    QUOTED_TOKEN_DOUBLE,
    QUOTED_TOKEN_BACKTICK,
    UNQUOTED_TOKEN,
    END
  } state= tokenize_state::START;
  
  char *curr= m_buffer;
  const char *end= curr + text.length;
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
      my_error(ER_INVALID_SCHEMA_NAME_LIST_SPEC, MYF(0));
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
      case tokenize_state::QUOTED_TOKEN_DOUBLE:
      case tokenize_state::QUOTED_TOKEN_BACKTICK:
        if ((state == tokenize_state::QUOTED_TOKEN_BACKTICK && *curr == '`') ||
            (state == tokenize_state::QUOTED_TOKEN_DOUBLE && *curr == '"'))
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
          my_error(ER_INVALID_SCHEMA_NAME_LIST_SPEC, MYF(0));
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
        if (unlikely(add_schema(token_start,
                                (size_t)(token_end - token_start))))
          return true;
        
        /*
          check_db_dir_existence requires a null-terminated string.
          since we own the buffer, we can safely null-terminate it.
        */
        *token_end= '\0';
      }

      state= tokenize_state::START;
    }
  }

  if (state == tokenize_state::UNQUOTED_TOKEN)
  {
    token_end= last_non_space + 1;
    auto len= (size_t)(token_end - token_start);

    if (len && add_schema(token_start, len))
      return true;
    *token_end= '\0';
  }
  else if (unlikely(state == tokenize_state::QUOTED_TOKEN_BACKTICK ||
                    state == tokenize_state::QUOTED_TOKEN_DOUBLE))
  {
    /*
      Unclosed quoted string.
    */
    my_error(ER_INVALID_SCHEMA_NAME_LIST_SPEC, MYF(0));
    return true;
  }

  return false;
}


size_t Sql_path::text_format_nbytes_needed() const
{
  size_t nbytes= 0;

  for (size_t i= 0; i < m_count; i++)
    nbytes+= m_schemas[i].length + 2 + 1;

  if (nbytes)
    nbytes--;

  return nbytes + 1;
}


size_t Sql_path::print(char *dst, size_t nbytes_available) const
{
  size_t nbytes= 0;

  for (size_t i= 0; i < m_count; i++)
  {
    const LEX_CSTRING &schema= m_schemas[i];
    size_t len= schema.length;
    if (nbytes + len + 3 > nbytes_available)
      break;

    *dst++= '`';
    memcpy(dst, schema.str, len);
    dst+= len;
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


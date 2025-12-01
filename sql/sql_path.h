#ifndef SQL_PATH_INCLUDED
#define SQL_PATH_INCLUDED

#include "sql_array.h"
#include "sql_list.h"


/* forward declarations */
class Database_qualified_name;
class sp_head;
class sp_name;
class Sp_handler;
struct MDL_key;
class Lex_ident_db_normalized;

struct Sql_path
{
private:
  Lex_ident_db m_schemas[16];
  uint m_count;

public:
  Sql_path();
  Sql_path& operator=(const Sql_path &rhs);
  Sql_path& operator=(Sql_path &&rhs)
  {
    set(std::move(rhs));
    return *this;
  }
  Sql_path(const Sql_path&) = delete;
  Sql_path(Sql_path &&rhs)
  {
    m_count= 0;
    set(std::move(rhs));
  }

  ~Sql_path() { free(); }

  bool resolve(THD *thd, sp_head *caller, sp_name *name,
               const Sp_handler **sph, Database_qualified_name *pkgname) const;
  /*
    Initialize the path variable with default values
  */
  bool init();
  /*
    Free the memory allocated by the path variable
  */
  void free();
  /*
    Set the variable to the value of rhs, making a copy of the buffer

    @param thd              The thread handle
    @param rhs              The path variable to copy from
  */
  void set(THD *thd, const Sql_path &rhs);
  /*
    Set the variable to the value of rhs, moving the buffer

    @param rhs              The path variable to move from
  */
  void set(Sql_path &&rhs);

  /*
    Parse a string and set the path variable to the parsed value

    The string is in my_charset_utf8mb3_general_ci.

    @param sv               system_variables, local or global
    @param str              The string to parse
  */
  bool from_text(const system_variables &sv, const LEX_CSTRING &str);
  /*
    Get the number of bytes needed to print the path variable

    @param thd              The thread handle

    @return The number of bytes needed
  */
  size_t text_format_nbytes_needed(THD *thd) const;
  /*
    Print the path variable to a string

    @param thd              The thread handle
    @param dst              The destination buffer
    @param nbytes_available The number of bytes available in the buffer

    @return The number of bytes written
  */
  size_t print(THD *thd, char *dst, size_t nbytes_available) const;

  LEX_CSTRING lex_cstring(THD *thd, MEM_ROOT *mem_root) const;

private:
  /*
    Helper function to resolve CURRENT_SCHEMA to actual database name.
    Returns the resolved schema, or {nullptr, 0} if resolution fails.
  */
  Lex_ident_db resolve_current_schema(THD *thd, sp_head *caller, size_t i) const;
  /*
    Helper function to try resolving a routine in a specific schema.
    Returns true if error occured.
  */
  bool try_resolve_in_schema(THD *thd, const Lex_ident_db_normalized &schema,
                             sp_name *name, const Sp_handler **sph,
                             Database_qualified_name *pkgname,
                             bool *resolved) const;

  LEX_CSTRING get_schema_for_print(size_t num, const LEX_CSTRING &db,
                                   bool *seen_current) const;

  bool add_schema(char **to, bool is_quoted);
  bool is_cur_schema(size_t i) const;
};


class Sql_path_instant_set
{
  THD *m_thd;
  Sql_path m_path;
public:
  Sql_path_instant_set(THD *thd, const LEX_CSTRING &str);
  Sql_path_instant_set(THD *thd, const Sql_path &new_path);
  ~Sql_path_instant_set();
  bool error() const { return m_thd; }
};

#endif /* SQL_PATH_INCLUDED */

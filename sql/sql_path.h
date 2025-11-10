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
  LEX_CSTRING m_schemas[16];
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

    @param thd              The thread handle
    @param cs               The character set of the string
    @param str              The string to parse
  */
  bool from_text(THD *thd, CHARSET_INFO *cs, const LEX_CSTRING &str);
  /*
    Get the number of bytes needed to print the path variable

    @param thd              The thread handle
    @param resolve          If true, resolve current schema to
                            the current database name

    @return The number of bytes needed
  */
  size_t text_format_nbytes_needed(THD *thd, bool resolve) const;
  /*
    Print the path variable to a string

    @param thd              The thread handle
    @param resolve          If true, resolve current schema to
                            the current database name
    @param dst              The destination buffer
    @param nbytes_available The number of bytes available in the buffer

    @return The number of bytes written
  */
  size_t print(THD *thd, bool resolve,
               char *dst, size_t nbytes_available) const;

  LEX_CSTRING lex_cstring(THD *thd, MEM_ROOT *mem_root) const;

protected:
  /*
    Helper function to resolve recursive routine calls
  */
  bool resolve_recursive_routine(sp_head *caller, sp_name *name) const;
  /*
    Helper function to resolve CURRENT_SCHEMA to actual database name.
    Returns the resolved schema, or {nullptr, 0} if resolution fails.
  */
  LEX_CSTRING resolve_current_schema(THD *thd, sp_head *caller,
                                     const LEX_CSTRING &schema) const;
  /*
    Helper function to try resolving a routine in a specific schema.
    Returns true if error occured.
  */
  bool try_resolve_in_schema(THD *thd, const Lex_ident_db_normalized &schema,
                             sp_name *name, const Sp_handler **sph,
                             Database_qualified_name *pkgname,
                             bool *resolved) const;

private:
  /*
    Add a schema to the path variable

    @param thd              The thread handle
    @param schema_str       The schema name
    @param schema_len       The length of the schema name

    @return                 true if the schema could not be added
  */
  bool add_schema(THD *thd, const char *schema_str, size_t schema_len);

  bool add_schema_direct(const char *schema_str, size_t schema_len);

  bool is_cur_schema(const LEX_CSTRING &schema) const;
};


class Sql_path_stack
{
  List<Sql_path> m_path_stack;
  THD *m_thd;
public:
  bool m_is_resolving;
public:
  Sql_path_stack(THD *thd) : m_thd(thd), m_is_resolving(false) {}
  bool push_path(CHARSET_INFO *cs, const LEX_CSTRING &path_str,
                 bool *was_pushed);
  bool pop_path();
};


class Sql_path_push
{
  bool m_pushed;
  bool m_error;
  Sql_path_stack *m_stack;
public:
  Sql_path_push(Sql_path_stack *stack, CHARSET_INFO *cs,
                const LEX_CSTRING &path_str)
  : m_pushed(false), m_error(false), m_stack(stack)
  {
    push(stack, cs, path_str);
  }
  Sql_path_push()
  : m_pushed(false), m_error(false), m_stack(nullptr)
  {
  }
  ~Sql_path_push()
  {
    pop();
  }

  bool error() const
  {
    return m_error;
  }

  void push(Sql_path_stack *stack, CHARSET_INFO *cs, const LEX_CSTRING &path_str)
  {
    m_stack= stack;
    if (m_stack && path_str.length)
      m_error= m_stack->push_path(cs, path_str, &m_pushed);
  }

  void pop()
  {
    if (m_pushed && m_stack)
    {
      m_stack->pop_path();
      m_pushed= false;
    }
  }
};


#endif /* SQL_PATH_INCLUDED */

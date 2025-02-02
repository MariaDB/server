#ifndef SQL_PATH_INCLUDED
#define SQL_PATH_INCLUDED

#include "mysqld.h"
#include "sql_array.h"
#include "sql_list.h"


/* forward declarations */
class Database_qualified_name;


struct Sql_path
{
private:
  LEX_CSTRING m_schemas_array[16];
  Bounds_checked_array<LEX_CSTRING> m_schemas;
  uint m_count;
  char *m_buffer;
  size_t m_buffer_length;

public:
  Sql_path();
  Sql_path& operator=(const Sql_path &rhs)
  {
    set(rhs, 1);
    return *this;
  }
  Sql_path& operator=(Sql_path &&rhs)
  {
    set(std::move(rhs), 1);
    return *this;
  }
  Sql_path(const Sql_path&) = delete;

  void init_array()
  {
    m_schemas.reset(m_schemas_array,
                    sizeof(m_schemas_array) / sizeof(m_schemas_array[0]));
  }

  bool find_db_unqualified(THD *thd, const LEX_CSTRING &name,
                           const Sp_handler *sph,
                           Lex_ident_db_normalized *dbn_out,
                           sp_name **spname_out) const;
  bool find_db_qualified(THD *thd,
                         sp_name *name,
                         const Sp_handler **sph,
                         Database_qualified_name *pkgname) const;

  /*
    Find the first schema in the path that is an internal schema.
    
    We are only interested in any one internal schema, since all internal
    schemas contain the same functions and procedures (but mapped to different
    Create_funcs).

    @return The first internal schema found, or nullptr if none was found
  */
  Schema *find_first_internal_schema() const;
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

    @param rhs              The path variable to copy from
    @param version_increment (not used)
  */
  void set(const Sql_path &rhs, uint version_increment);
  /*
    Set the variable to the value of rhs, moving the buffer

    @param rhs              The path variable to move from
    @param version_increment (not used)
  */
  void set(Sql_path &&rhs, uint version_increment);

  /*
    Parse a string and set the path variable to the parsed value

    @param thd              The thread handle
    @param cs               The character set of the string
    @param str              The string to parse
  */
  bool from_text(THD *thd, CHARSET_INFO *cs, const LEX_CSTRING &str);
  /*
    Get the number of bytes needed to print the path variable

    @return The number of bytes needed
  */
  size_t text_format_nbytes_needed() const;
  /*
    Print the path variable to a string

    @param dst              The destination buffer
    @param nbytes_available The number of bytes available in the buffer

    @return The number of bytes written
  */
  size_t print(char *dst, size_t nbytes_available) const;

private:
  /*
    Add a schema to the path variable

    @param schema_str       The schema name
    @param schema_len       The length of the schema name

    @return                 true if the schema could not be added
  */
  bool add_schema(const char *schema_str, size_t schema_len);
};

class Sql_path_save
{
protected:
  Sql_path m_old_path;
  Sql_path &m_path;
public:
  Sql_path_save(Sql_path &path) : m_path(path)
  {
    m_old_path= std::move(path);
  }
  ~Sql_path_save()
  {
    m_path= std::move(m_old_path);
  }
};

class Sql_path_save_and_clear: public Sql_path_save
{
public:
  Sql_path_save_and_clear(Sql_path &path) : Sql_path_save(path)
  {
    path.init();
  }
};

#endif /* SQL_PATH_INCLUDED */

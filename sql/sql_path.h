#ifndef SQL_PATH_INCLUDED
#define SQL_PATH_INCLUDED

#include "mysqld.h"
#include "sql_array.h"


/* forward declarations */
class Database_qualified_name;


class Sql_path
{
private:
  Dynamic_array<LEX_CSTRING> db_list;

public:
  Sql_path();
  void append_db(char *in);
  void strtok_db(char *in);
  bool find_db_unqualified(THD *thd, const LEX_CSTRING &name,
                           const Sp_handler *sph,
                           Lex_ident_db_normalized *dbn_out,
                           sp_name **spname_out);
  bool find_db_qualified(THD *thd,
                         sp_name *name,
                         const Sp_handler **sph,
                         Database_qualified_name *pkgname);
  void free_db_list();
};

#endif /* SQL_PATH_INCLUDED */

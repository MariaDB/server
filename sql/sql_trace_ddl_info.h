#ifndef SQL_TRACE_DDL_INFO
#define SQL_TRACE_DDL_INFO

#include <vector>
#include <unordered_map>
#include <cstring>

class SELECT_LEX;
class THD;

void save_table_definitions(THD *thd, SELECT_LEX *select_lex);
void dump_used_ddls(THD *thd);

struct table_name_hash_fn
{
  size_t operator()(const char *str) const
  {
    size_t hash_value= 0;
    for (int i= 0; str[i] != '\0'; i++)
    {
      hash_value= (hash_value * 31) + str[i];
    }
    return hash_value;
  }
};

struct table_name_comparator
{
  bool operator()(const char *str1, const char *str2) const
  {
    return strcmp(str1, str2) == 0;
  }
};

struct ddl_info_for_trace
{
  std::unordered_map<const char *, size_t, table_name_hash_fn,
                     table_name_comparator>
      tables_map;
  std::vector<const char *> stmts;
};

typedef ddl_info_for_trace DDL_Info;
#endif
#ifndef SQL_TRACE_DDL_INFO
#define SQL_TRACE_DDL_INFO

#include <vector>
#include <unordered_map>

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

typedef struct ddl_info_for_trace
{
  std::unordered_map<const char *, size_t, table_name_hash_fn,
                     table_name_comparator>
      tables_map;
  std::vector<const char *> stmts;
} DDL_Info;

#endif
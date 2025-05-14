#include "sql_trace_ddl_info.h"
#include "my_json_writer.h"
#include "sql_show.h"

static bool is_json_schema_internal_table(TABLE_LIST *tbl)
{
  if (tbl->table_function)
    return true;
  else if (tbl->schema_table)
    return true;
  else if (!tbl->is_view() && tbl->table && tbl->table->s &&
           tbl->table->s->tmp_table == INTERNAL_TMP_TABLE)
    return true;
  return false;
}

static bool compare_pairs(const std::pair<const char *, size_t> &p1,
                          const std::pair<const char *, size_t> &p2)
{
  return p1.second < p2.second;
}

void save_table_definitions(THD *thd, SELECT_LEX *select_lex)
{
  List_iterator<TABLE_LIST> li(select_lex->leaf_tables);
  while (TABLE_LIST *tbl= li++)
  {
    if (tbl->table && tbl->table->s && !is_json_schema_internal_table(tbl))
    {
      bool flag= false;
      String ddl(2048);
      size_t name_len=
          strlen(tbl->table_name.str) + strlen(tbl->get_db_name().str) + 1;
      char *full_name= thd->calloc(name_len);
      if (tbl->view)
      {
        show_create_view(thd, tbl, &ddl);
        flag= true;
      }
      else if (tbl->table->s->table_category == TABLE_CATEGORY_USER ||
               tbl->table->s->tmp_table == TRANSACTIONAL_TMP_TABLE)
      {
        show_create_table(thd, tbl, &ddl, NULL, WITH_DB_NAME);
        flag= true;
      }
      strcpy(full_name, tbl->get_db_name().str);
      strcat(full_name, ".");
      strcat(full_name, tbl->table_name.str);
      if (flag && thd->ddl_info->tables_map.find(full_name) ==
                      thd->ddl_info->tables_map.end())
      {
        thd->ddl_info->tables_map[full_name]= thd->ddl_info->stmts.size();
        char *buf = ddl.c_ptr();
        size_t buf_len= strlen(buf) + 1;
        char *buf_copy = thd->alloc(buf_len);
        strcpy(buf_copy, buf);
        thd->ddl_info->stmts.push_back(buf_copy);
      }
    }
  }
}

void dump_used_ddls(THD *thd)
{
  if (thd->ddl_info->stmts.size() > 0 &&
      thd->ddl_info->stmts.size() == thd->ddl_info->tables_map.size())
  {
    Json_writer_object ddls_wrapper(thd);
    Json_writer_array ddl_list(thd, "list_ddls");
    std::vector<std::pair<const char *, size_t>> mapVector(
        thd->ddl_info->tables_map.begin(), thd->ddl_info->tables_map.end());
    std::sort(mapVector.begin(), mapVector.end(), compare_pairs);
    for (const auto &pair : mapVector)
    {
      Json_writer_object ddl_wrapper(thd);
      ddl_wrapper.add("name", pair.first);
      ddl_wrapper.add_with_escapes("ddl", thd->ddl_info->stmts[pair.second]);
    }
  }
}
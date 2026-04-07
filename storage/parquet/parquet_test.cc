  #define MYSQL_SERVER 1
  
  #include "my_global.h"
  #include "my_config.h"

  #include "sql/table.h"
  #include "sql/field.h"
  #include "sql/sql_type.h"

  #include <string>
  #include <tap.h>


std::string build_query(std::string table_name, TABLE *table_arg);

static void test_build_query_basic_schema()
{
  LEX_CSTRING id_name= {STRING_WITH_LEN("id")};
  LEX_CSTRING name_name= {STRING_WITH_LEN("name")};

  TABLE table{};
  TABLE_SHARE share{};
  table.s= &share;

  Field_long id_field(11, false, &id_name, false);
  Field_varstring name_field(255, false, &name_name, &share,
                             DTCollation(&my_charset_bin));

  Field *fields[]= {&id_field, &name_field, nullptr};
  share.field= fields;

  std::string query= build_query("users", &table);
  ok(query == "CREATE TABLE users (id INTEGER, name VARCHAR)",
     "build_query maps INTEGER and VARCHAR columns");
}

static void test_build_query_blob_mapping()
{
  LEX_CSTRING payload_name= {STRING_WITH_LEN("payload")};

  TABLE table{};
  TABLE_SHARE share{};
  table.s= &share;

  Field_blob payload_field(1024, false, &payload_name,
                           DTCollation(&my_charset_bin));

  Field *fields[]= {&payload_field, nullptr};
  share.field= fields;

  std::string query= build_query("files", &table);
  ok(query == "CREATE TABLE files (payload BLOB)",
     "build_query maps binary blob columns to BLOB");
}

int main()
{
  plan(2);

  test_build_query_basic_schema();
  test_build_query_blob_mapping();

  return exit_status();
}

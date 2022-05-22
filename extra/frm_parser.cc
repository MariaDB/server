#include <stdio.h>

#include "my_global.h"
#include "m_ctype.h"
#include "item.h"
#include "table.h"

#include "sql_type.h"

CHARSET_INFO *system_charset_info = &my_charset_utf8mb3_general_ci;



uint lower_case_table_names= 1;

/* INFORMATION_SCHEMA name */
LEX_CSTRING INFORMATION_SCHEMA_NAME= {STRING_WITH_LEN("information_schema")};
extern const LEX_CSTRING primary_key_name= { STRING_WITH_LEN("PRIMARY") };

void Item::print_parenthesised(String *str, enum_query_type query_type,
                               enum precedence parent_prec)
{
  bool need_parens= precedence() < parent_prec;
  if (need_parens)
    str->append('(');
  print(str, query_type);
  if (need_parens)
    str->append(')');
}

typedef enum
{
  WITHOUT_DB_NAME,
  WITH_DB_NAME
} enum_with_db_name;

int show_create_table(TABLE_LIST *table_list, String *packet,
                      Table_specification_st *create_info_arg,
                      enum_with_db_name with_db_name, LEX_CSTRING db,
                      ulong option_bits, ulong sql_mode);

char reg_ext[FN_EXTLEN];
uint reg_ext_length;
ulong current_pid, specialflag;
ulong feature_check_constraint;

const LEX_CSTRING NULL_clex_str=  {STRING_WITH_LEN("NULL")};
SEQUENCE::SEQUENCE() {}

void push_warning_printf(THD *thd, Sql_condition::enum_warning_level level,
                         uint code, const char *format, ...)
{
  //
}


Compression_method compression_methods[MAX_COMPRESSION_METHODS]=
{
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { "zlib", 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 }
};


int main(int argc, char **argv)
{
  TABLE_SHARE share;
  TABLE_LIST list;
  String packet;

  show_create_table(&list, &packet, NULL, WITHOUT_DB_NAME, {}, 0, 0);

  open_table_def(NULL, &share, NULL, NULL, 0, NULL, NULL, NULL, NULL);

  printf("hello world");

  return 0;
}

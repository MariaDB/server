#include "my_global.h"
#include "sql_i_s.h"
#include "sql_class.h"
#include "table.h"
#include "field.h"
#include "sql_acl.h"
#include "removed_option_registry.h"

using namespace Show;

enum enum_removed_startup_options_fields
{
  RSO_OPTION_NAME = 0,
  RSO_OPTION_VALUE,
  RSO_SOURCE,
  RSO_CONFIG_FILE,
  RSO_HANDLING
};

ST_FIELD_INFO removed_startup_options_fields_info[] =
{
  Column("OPTION_NAME",  Varchar(128),  NOT_NULL, "Option_name"),
  Column("OPTION_VALUE", Varchar(1024), NULLABLE, "Option_value"),
  Column("SOURCE",       Varchar(32),   NOT_NULL, "Source"),
  Column("CONFIG_FILE",  Varchar(1024), NULLABLE, "Config_file"),
  Column("HANDLING",     Varchar(32),   NOT_NULL, "Handling"),
  CEnd()
};

int fill_schema_removed_startup_options(THD *thd, TABLE_LIST *tables, COND *cond)
{
  TABLE *table = tables->table;
  size_t i, cnt;
  (void) cond;

  if (check_global_access(thd, PROCESS_ACL, true))
    return 0;

  cnt = removed_startup_option_count();

  for (i = 0; i < cnt; ++i)
  {
    Removed_startup_option row;

    if (get_removed_startup_option_copy(i, &row))
      continue;

    restore_record(table, s->default_values);

    table->field[RSO_OPTION_NAME]->store(row.option_name.c_str(),
                                         (uint32) row.option_name.length(),
                                         system_charset_info);

    if (!row.option_value.empty())
    {
      table->field[RSO_OPTION_VALUE]->store(row.option_value.c_str(),
                                            (uint32) row.option_value.length(),
                                            system_charset_info);
      table->field[RSO_OPTION_VALUE]->set_notnull();
    }
    else
      table->field[RSO_OPTION_VALUE]->set_null();

    table->field[RSO_SOURCE]->store(row.source.c_str(),
                                    (uint32) row.source.length(),
                                    system_charset_info);

    if (!row.config_file.empty())
    {
      table->field[RSO_CONFIG_FILE]->store(row.config_file.c_str(),
                                           (uint32) row.config_file.length(),
                                           system_charset_info);
      table->field[RSO_CONFIG_FILE]->set_notnull();
    }
    else
      table->field[RSO_CONFIG_FILE]->set_null();

    table->field[RSO_HANDLING]->store(row.handling.c_str(),
                                      (uint32) row.handling.length(),
                                      system_charset_info);

    if (schema_table_store_record(thd, table))
     return 1;
  }

  return 0;
}

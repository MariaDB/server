/*
  Copyright (c) 2013, Spaempresarial - Brazil, Roberto Spadim
  http://www.spadim.com.br/
  roberto@spadim.com.br

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:
      * Redistributions of source code must retain the above copyright
        notice, this list of conditions and the following disclaimer.
      * Redistributions in binary form must reproduce the above copyright
        notice, this list of conditions and the following disclaimer in the
        documentation and/or other materials provided with the distribution.
      * Neither the name of the Roberto Spadim nor the
        names of the contributors may be used to endorse or promote products
        derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL ROBERTO SPADIM BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sql_class.h>          // THD
#include <table.h>              // ST_SCHEMA_TABLE
#include <mysql/plugin.h>
#include <m_ctype.h>
#include "sql_locale.h"

bool schema_table_store_record(THD *thd, TABLE *table);
static MY_LOCALE **locale_list;

/* LOCALES */
static ST_FIELD_INFO locale_info_locale_fields_info[]=
{
  {"ID", 4, MYSQL_TYPE_LONGLONG, 0, 0, "Id", 0},
  {"NAME", 255, MYSQL_TYPE_STRING, 0, 0, "Name", 0},
  {"DESCRIPTION", 255,  MYSQL_TYPE_STRING, 0, 0, "Description", 0},
  {"MAX_MONTH_NAME_LENGTH", 4, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"MAX_DAY_NAME_LENGTH", 4, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"DECIMAL_POINT", 2, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"THOUSAND_SEP", 2, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"ERROR_MESSAGE_LANGUAGE", 64, MYSQL_TYPE_STRING, 0, 0, "Error_Message_Language", 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}
};
static int locale_info_fill_table_locale(THD* thd, TABLE_LIST* tables, COND* cond)
{
  TABLE *table= tables->table;
  CHARSET_INFO *cs= system_charset_info;
  
  for (MY_LOCALE **loc= locale_list; *loc; loc++)
  {
      /* ID */
      table->field[0]->store((longlong) (*loc)->number, TRUE);
      /* NAME */
      table->field[1]->store((*loc)->name, strlen((*loc)->name), cs);
      /* DESCRIPTION */
      table->field[2]->store((*loc)->description, strlen((*loc)->description), cs);
      /* MAX_MONTH_NAME_LENGTH */
      table->field[3]->store((longlong) (*loc)->max_month_name_length, TRUE);
      /* MAX_DAY_NAME_LENGTH */
      table->field[4]->store((longlong) (*loc)->max_day_name_length, TRUE);
      /* DECIMAL_POINT */
      char decimal= (*loc)->decimal_point;
      table->field[5]->store(&decimal, decimal ? 1 : 0, cs);
      /* THOUSAND_SEP */
      char thousand= (*loc)->thousand_sep;
      table->field[6]->store(&thousand, thousand ?  1 : 0, cs);
      /* ERROR_MESSAGE_LANGUAGE */
      table->field[7]->store((*loc)->errmsgs->language,
                             strlen((*loc)->errmsgs->language), cs);
      if (schema_table_store_record(thd, table))
        return 1;
  }
  return 0;
}

static int locale_info_plugin_init_locales(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;
  schema->fields_info= locale_info_locale_fields_info;
  schema->fill_table= locale_info_fill_table_locale;

#if defined(_WIN64)
  locale_list = (MY_LOCALE **)GetProcAddress(GetModuleHandle(NULL), "?my_locales@@3PAPEAVMY_LOCALE@@A");
#elif defined(_WIN32)
  locale_list = (MY_LOCALE **)GetProcAddress(GetModuleHandle(NULL), "?my_locales@@3PAPAVMY_LOCALE@@A");
#else
  locale_list = my_locales;
#endif

  return 0;
}
static struct st_mysql_information_schema locale_info_plugin=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

/*
  Plugin library descriptor
*/

maria_declare_plugin(locales)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,      	/* the plugin type (see include/mysql/plugin.h) */
  &locale_info_plugin,                  	/* pointer to type-specific plugin descriptor   */
  "LOCALES",                            	/* plugin name */
  "Roberto Spadim, Spaempresarial - Brazil",    /* plugin author */
  "Lists all locales from server.", 		/* the plugin description */
  PLUGIN_LICENSE_BSD,      	             	/* the plugin license (see include/mysql/plugin.h) */
  locale_info_plugin_init_locales,          	/* Pointer to plugin initialization function */
  0,            	                        /* Pointer to plugin deinitialization function */
  0x0100, 	                             	/* Numeric version 0xAABB means AA.BB veriosn */
  NULL,                                 	/* Status variables */
  NULL,                                 	/* System variables */
  "1.0",                                	/* String version representation */
  MariaDB_PLUGIN_MATURITY_STABLE        	/* Maturity (see include/mysql/plugin.h)*/
}
maria_declare_plugin_end;

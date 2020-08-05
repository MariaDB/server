/*
   Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2009, 2019, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include <my_global.h>
#include <sql_class.h>          // THD
#include <sql_i_s.h>            // ST_SCHEMA_TABLE
#include <mysql/plugin.h>
#include "sql_show.h"           // get_all_tables()
#include "sql_error.h"          // convert_error_to_warning()
#include "sql_type_geom.h"


/*********** INFORMATION_SCHEMA.SPATIEL_REF_SYS *******************/

namespace Show {

static ST_FIELD_INFO spatial_ref_sys_fields_info[]=
{
  Column("SRID",      SShort(5),          NOT_NULL),
  Column("AUTH_NAME", Varchar(FN_REFLEN), NOT_NULL),
  Column("AUTH_SRID", SLong(5),           NOT_NULL),
  Column("SRTEXT",    Varchar(2048),      NOT_NULL),
  CEnd()
};


static int spatial_ref_sys_fill(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_spatial_ref_sys");
  TABLE *table= tables->table;
  CHARSET_INFO *cs= system_charset_info;
  int result= 1;

  restore_record(table, s->default_values);

  table->field[0]->store(-1, FALSE); /*SRID*/
  table->field[1]->store(STRING_WITH_LEN("Not defined"), cs); /*AUTH_NAME*/
  table->field[2]->store(-1, FALSE); /*AUTH_SRID*/
  table->field[3]->store(STRING_WITH_LEN(
        "LOCAL_CS[\"Spatial reference wasn't specified\","
        "LOCAL_DATUM[\"Unknown\",0]," "UNIT[\"m\",1.0]," "AXIS[\"x\",EAST],"
        "AXIS[\"y\",NORTH]]"), cs);/*SRTEXT*/
  if (schema_table_store_record(thd, table))
    goto exit;

  table->field[0]->store(0, TRUE); /*SRID*/
  table->field[1]->store(STRING_WITH_LEN("EPSG"), cs); /*AUTH_NAME*/
  table->field[2]->store(404000, TRUE); /*AUTH_SRID*/
  table->field[3]->store(STRING_WITH_LEN(
        "LOCAL_CS[\"Wildcard 2D cartesian plane in metric unit\","
        "LOCAL_DATUM[\"Unknown\",0]," "UNIT[\"m\",1.0],"
        "AXIS[\"x\",EAST]," "AXIS[\"y\",NORTH],"
        "AUTHORITY[\"EPSG\",\"404000\"]]"), cs);/*SRTEXT*/
  if (schema_table_store_record(thd, table))
    goto exit;

  result= 0;

exit:
  DBUG_RETURN(result);
}


static int plugin_init_spatial_ref_sys(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;
  schema->fields_info= spatial_ref_sys_fields_info;
  schema->fill_table= spatial_ref_sys_fill;
  return 0;
}


static struct st_mysql_information_schema spatial_ref_sys_plugin=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };


} // namespace Show

/*********** INFORMATION_SCHEMA.GEOMETRY_COLUMNS *******************/


namespace Show {

static ST_FIELD_INFO geometry_columns_fields_info[]=
{
  Column("F_TABLE_CATALOG",    Catalog(), NOT_NULL,  OPEN_FRM_ONLY),
  Column("F_TABLE_SCHEMA",     Name(),    NOT_NULL,  OPEN_FRM_ONLY),
  Column("F_TABLE_NAME",       Name(),    NOT_NULL,  OPEN_FRM_ONLY),
  Column("F_GEOMETRY_COLUMN",  Name(),    NOT_NULL,  OPEN_FRM_ONLY),
  Column("G_TABLE_CATALOG",    Catalog(), NOT_NULL,  OPEN_FRM_ONLY),
  Column("G_TABLE_SCHEMA",     Name(),    NOT_NULL,  OPEN_FRM_ONLY),
  Column("G_TABLE_NAME",       Name(),    NOT_NULL,  OPEN_FRM_ONLY),
  Column("G_GEOMETRY_COLUMN",  Name(),    NOT_NULL,  OPEN_FRM_ONLY),
  Column("STORAGE_TYPE",       STiny(2),  NOT_NULL,  OPEN_FRM_ONLY),
  Column("GEOMETRY_TYPE",      SLong(7),  NOT_NULL,  OPEN_FRM_ONLY),
  Column("COORD_DIMENSION",    STiny(2),  NOT_NULL,  OPEN_FRM_ONLY),
  Column("MAX_PPR",            STiny(2),  NOT_NULL,  OPEN_FRM_ONLY),
  Column("SRID",               SShort(5), NOT_NULL,  OPEN_FRM_ONLY),
  CEnd()
};


static void geometry_columns_fill_record(TABLE *table,
                                         const LEX_CSTRING *db_name,
                                         const LEX_CSTRING *table_name,
                                         const Field_geom *field)
{
  static const LEX_CSTRING catalog= {STRING_WITH_LEN("def")};
  const CHARSET_INFO *cs= system_charset_info;
  const Type_handler_geometry *gth= field->type_handler_geom();
  /*F_TABLE_CATALOG*/
  table->field[0]->store(catalog, cs);
  /*F_TABLE_SCHEMA*/
  table->field[1]->store(db_name, cs);
  /*F_TABLE_NAME*/
  table->field[2]->store(table_name, cs);
  /*G_TABLE_CATALOG*/
  table->field[4]->store(catalog, cs);
  /*G_TABLE_SCHEMA*/
  table->field[5]->store(db_name, cs);
  /*G_TABLE_NAME*/
  table->field[6]->store(table_name, cs);
  /*G_GEOMETRY_COLUMN*/
  table->field[7]->store(field->field_name, cs);
  /*STORAGE_TYPE*/
  table->field[8]->store(1LL, true); /*Always 1 (binary implementation)*/
  /*GEOMETRY_TYPE*/
  table->field[9]->store((longlong) (gth->geometry_type()), true);
  /*COORD_DIMENSION*/
  table->field[10]->store(2LL, true);
  /*MAX_PPR*/
  table->field[11]->set_null();
  /*SRID*/
  table->field[12]->store((longlong) (field->get_srid()), true);
}


static int get_geometry_column_record(THD *thd, TABLE_LIST *tables,
                                      TABLE *table, bool res,
                                      const LEX_CSTRING *db_name,
                                      const LEX_CSTRING *table_name)
{
  TABLE *show_table;
  Field **ptr, *field;
  DBUG_ENTER("get_geometry_column_record");

  if (res)
  {
    /*
      open_table() failed with an error.
      Convert the error to a warning and let the caller
      continue with the next table.
    */
    convert_error_to_warning(thd);
    DBUG_RETURN(0);
  }

  // Skip INFORMATION_SCHEMA tables. They don't have geometry columns.
  if (tables->schema_table)
    DBUG_RETURN(0);

  show_table= tables->table;
  ptr= show_table->field;
  show_table->use_all_columns();               // Required for default
  restore_record(show_table, s->default_values);

  for (; (field= *ptr) ; ptr++)
  {
    const Field_geom *fg;
    if (field->type() == MYSQL_TYPE_GEOMETRY &&
        (fg= dynamic_cast<const Field_geom*>(field)))
    {
      DEBUG_SYNC(thd, "get_schema_column");
      /* Get default row, with all NULL fields set to NULL */
      restore_record(table, s->default_values);
      geometry_columns_fill_record(table, db_name, table_name, fg);
      if (schema_table_store_record(thd, table))
        DBUG_RETURN(1);
    }
  }

  DBUG_RETURN(0);
}


static int plugin_init_geometry_columns(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;
  schema->fields_info= geometry_columns_fields_info;
  schema->fill_table= get_all_tables;
  schema->process_table= get_geometry_column_record;
  schema->idx_field1= 1;
  schema->idx_field2= 2;
  schema->i_s_requested_object= OPTIMIZE_I_S_TABLE | OPEN_VIEW_FULL;
  return 0;
}


static struct st_mysql_information_schema geometry_columns_plugin=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };


} // namespace Show


/********************* Plugin library descriptors ************************/


maria_declare_plugin(type_geom)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,   // the plugin type (see include/mysql/plugin.h)
  &Show::spatial_ref_sys_plugin,     // pointer to type-specific plugin descriptor
  "SPATIAL_REF_SYS",                 // plugin name
  "MariaDB",                         // plugin author
  "Lists all geometry columns",      // the plugin description
  PLUGIN_LICENSE_GPL,                // the plugin license (see include/mysql/plugin.h)
  Show::plugin_init_spatial_ref_sys, // Pointer to plugin initialization function
  0,                                 // Pointer to plugin deinitialization function
  0x0100,                            // Numeric version 0xAABB means AA.BB version
  NULL,                              // Status variables
  NULL,                              // System variables
  "1.0",                             // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE     // Maturity (see include/mysql/plugin.h)*/
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,   // the plugin type (see include/mysql/plugin.h)
  &Show::geometry_columns_plugin,    // pointer to type-specific plugin descriptor
  "GEOMETRY_COLUMNS",                // plugin name
  "MariaDB",                         // plugin author
  "Lists all geometry columns",      // the plugin description
  PLUGIN_LICENSE_GPL,                // the plugin license (see include/mysql/plugin.h)
  Show::plugin_init_geometry_columns,// Pointer to plugin initialization function
  0,                                 // Pointer to plugin deinitialization function
  0x0100,                            // Numeric version 0xAABB means AA.BB version
  NULL,                              // Status variables
  NULL,                              // System variables
  "1.0",                             // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE     // Maturity (see include/mysql/plugin.h)
}
maria_declare_plugin_end;

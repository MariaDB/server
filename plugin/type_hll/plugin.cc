/* Copyright (c) 2026, drrtuy@gmail.com
   Copyright (c) 2026, MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#define MYSQL_SERVER
#include "mariadb.h"
#include "sql_class.h"
#include "item_create.h"
#include "sql_type_hll.h"
#include "item_hllfunc.h"
#include <mysql/plugin_data_type.h>
#include <mysql/plugin_function.h>


static struct st_mariadb_data_type plugin_descriptor_hll=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  &type_handler_hll
};


/*
  Builder for a function with exactly ARG_COUNT arguments.
  Aggregates are constructed from their single argument.
*/
template<class T, uint ARG_COUNT, bool AGGREGATE= false>
class Create_func_hll: public Create_native_func
{
public:
  bool is_aggregate() const override { return AGGREGATE; }
  Item *create_native(THD *thd, const LEX_CSTRING *name,
                      List<Item> *item_list) override
  {
    if ((item_list ? item_list->elements : 0) != ARG_COUNT)
    {
      my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
      return NULL;
    }
    if constexpr (AGGREGATE)
      return new (thd->mem_root) T(thd, item_list->head());
    else if constexpr (ARG_COUNT == 0)
      return new (thd->mem_root) T(thd);
    else
      return new (thd->mem_root) T(thd, *item_list);
  }
  static Plugin_function *descriptor()
  {
    static Create_func_hll creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};


#define HLL_FUNCTION_PLUGIN(DESCRIPTOR, NAME, DESCRIPTION)                    \
{                                                                            \
  MariaDB_FUNCTION_PLUGIN, DESCRIPTOR, NAME, "drrtuy",          \
  DESCRIPTION, PLUGIN_LICENSE_GPL, 0, 0, 0x0100, NULL, NULL, "1.0",          \
  MariaDB_PLUGIN_MATURITY_STABLE                                             \
}


maria_declare_plugin(type_hll)
{
  MariaDB_DATA_TYPE_PLUGIN,     // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_hll,       // pointer to type-specific plugin descriptor
  "hll",                        // plugin name
  "drrtuy",        // plugin author
  "Data type HLL (HyperLogLog sketch)", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE  // Maturity(see include/mysql/plugin.h)*/
},
HLL_FUNCTION_PLUGIN(
  (Create_func_hll<Item_func_hll_empty, 0>::descriptor()),
  "hll_empty", "Function HLL_EMPTY()"),
HLL_FUNCTION_PLUGIN(
  (Create_func_hll<Item_func_hll_cardinality, 1>::descriptor()),
  "hll_cardinality", "Function HLL_CARDINALITY()"),
HLL_FUNCTION_PLUGIN(
  (Create_func_hll<Item_func_hll_cardinality_round, 1>::descriptor()),
  "hll_cardinality_round", "Function HLL_CARDINALITY_ROUND()"),
HLL_FUNCTION_PLUGIN(
  (Create_func_hll<Item_func_hll_union, 2>::descriptor()),
  "hll_union", "Function HLL_UNION()"),
HLL_FUNCTION_PLUGIN(
  (Create_func_hll<Item_func_hll_format_version, 1>::descriptor()),
  "hll_format_version", "Function HLL_FORMAT_VERSION()"),
HLL_FUNCTION_PLUGIN(
  (Create_func_hll<Item_func_hll_info, 1>::descriptor()),
  "hll_info", "Function HLL_INFO()"),
HLL_FUNCTION_PLUGIN(
  (Create_func_hll<Item_sum_hll_build, 1, true>::descriptor()),
  "hll_build_agg", "Aggregate function HLL_BUILD_AGG()"),
HLL_FUNCTION_PLUGIN(
  (Create_func_hll<Item_sum_hll_union, 1, true>::descriptor()),
  "hll_union_agg", "Aggregate function HLL_UNION_AGG()")
maria_declare_plugin_end;

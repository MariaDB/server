/* Copyright (c) 2016, MariaDB corporation. All rights
   reserved.

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

#define MYSQL_SERVER 1
#include <my_global.h>
#include <mysql_version.h>
#include <mysqld.h>
#include <mysql/plugin.h>
#include "sql_plugin.h"                         // st_plugin_int
#include "sql_class.h"
#include "item.h"
#include "vtq.h"
#include "vers_utils.h"

plugin_ref innodb_plugin= NULL;
static handlerton* innodb_hton= NULL;

/* System Versioning: VTQ_TRX_ID(), VTQ_COMMIT_ID(), VTQ_BEGIN_TS(), VTQ_COMMIT_TS(), VTQ_ISO_LEVEL() */
template <vtq_field_t VTQ_FIELD>
class Create_func_vtq : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_vtq<VTQ_FIELD> s_singleton;

protected:
  Create_func_vtq<VTQ_FIELD>() {}
  virtual ~Create_func_vtq<VTQ_FIELD>() {}
};

template<vtq_field_t VTQ_FIELD>
Create_func_vtq<VTQ_FIELD> Create_func_vtq<VTQ_FIELD>::s_singleton;

template <vtq_field_t VTQ_FIELD>
Item*
Create_func_vtq<VTQ_FIELD>::create_native(THD *thd, LEX_CSTRING *name,
  List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 1:
  {
    Item *param_1= item_list->pop();
    switch (VTQ_FIELD)
    {
    case VTQ_BEGIN_TS:
    case VTQ_COMMIT_TS:
      func= new (thd->mem_root) Item_func_vtq_ts(thd, innodb_hton, param_1, VTQ_FIELD);
      break;
    case VTQ_TRX_ID:
    case VTQ_COMMIT_ID:
    case VTQ_ISO_LEVEL:
      func= new (thd->mem_root) Item_func_vtq_id(thd, innodb_hton, param_1, VTQ_FIELD);
      break;
    default:
      DBUG_ASSERT(0);
    }
    break;
  }
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    switch (VTQ_FIELD)
    {
    case VTQ_TRX_ID:
    case VTQ_COMMIT_ID:
      func= new (thd->mem_root) Item_func_vtq_id(thd, innodb_hton, param_1, param_2, VTQ_FIELD);
      break;
    default:
      goto error;
    }
    break;
  }
  error:
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
};

template <class Item_func_vtq_trx_seesX>
class Create_func_vtq_trx_sees : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list)
  {
    Item *func= NULL;
    int arg_count= 0;

    if (item_list != NULL)
      arg_count= item_list->elements;

    switch (arg_count) {
    case 2:
    {
      Item *param_1= item_list->pop();
      Item *param_2= item_list->pop();
      func= new (thd->mem_root) Item_func_vtq_trx_seesX(thd, innodb_hton, param_1, param_2);
      break;
    }
    default:
      my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
      break;
    }

    return func;
  }

  static Create_func_vtq_trx_sees<Item_func_vtq_trx_seesX> s_singleton;

protected:
  Create_func_vtq_trx_sees<Item_func_vtq_trx_seesX>() {}
  virtual ~Create_func_vtq_trx_sees<Item_func_vtq_trx_seesX>() {}
};

template<class X>
Create_func_vtq_trx_sees<X> Create_func_vtq_trx_sees<X>::s_singleton;

#define BUILDER(F) & F::s_singleton

static Native_func_registry func_array[] =
{
  { { C_STRING_WITH_LEN("VTQ_BEGIN_TS") }, BUILDER(Create_func_vtq<VTQ_BEGIN_TS>)},
  { { C_STRING_WITH_LEN("VTQ_COMMIT_ID") }, BUILDER(Create_func_vtq<VTQ_COMMIT_ID>)},
  { { C_STRING_WITH_LEN("VTQ_COMMIT_TS") }, BUILDER(Create_func_vtq<VTQ_COMMIT_TS>)},
  { { C_STRING_WITH_LEN("VTQ_ISO_LEVEL") }, BUILDER(Create_func_vtq<VTQ_ISO_LEVEL>)},
  { { C_STRING_WITH_LEN("VTQ_TRX_ID") }, BUILDER(Create_func_vtq<VTQ_TRX_ID>)},
  { { C_STRING_WITH_LEN("VTQ_TRX_SEES") }, BUILDER(Create_func_vtq_trx_sees<Item_func_vtq_trx_sees>)},
  { { C_STRING_WITH_LEN("VTQ_TRX_SEES_EQ") }, BUILDER(Create_func_vtq_trx_sees<Item_func_vtq_trx_sees_eq>)},
  { {0, 0}, NULL}
};


/*
  Disable __attribute__() on non-gcc compilers.
*/
#if !defined(__attribute__) && !defined(__GNUC__)
#define __attribute__(A)
#endif

static int versioning_plugin_init(void *p __attribute__ ((unused)))
{
  static LString InnoDB= "InnoDB";

  DBUG_ENTER("versioning_plugin_init");
  // No need in locking since we so far single-threaded
  int res= item_create_append(func_array);
  if (res)
  {
    my_message(ER_PLUGIN_IS_NOT_LOADED, "Can't append function array" , MYF(0));
    DBUG_RETURN(res);
  }

  innodb_plugin= ha_resolve_by_name(NULL, &InnoDB.lex_cstring(), false);
  if (!innodb_plugin)
  {
    my_error(ER_PLUGIN_IS_NOT_LOADED, MYF(0), InnoDB.ptr());
    DBUG_RETURN(1);
  }

  innodb_hton= plugin_hton(innodb_plugin);
  if (!innodb_hton || (innodb_hton->flags & HTON_NOT_USER_SELECTABLE))
  {
    my_message(ER_PLUGIN_IS_NOT_LOADED, "Can't get handlerton" , MYF(0));
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}

static int versioning_plugin_deinit(void *p __attribute__ ((unused)))
{
  DBUG_ENTER("versioning_plugin_deinit");
  if (innodb_plugin)
    plugin_unlock(NULL, innodb_plugin);
  DBUG_RETURN(0);
}

struct st_mysql_daemon versioning_plugin=
{ MYSQL_REPLICATION_INTERFACE_VERSION  };

/*
  Plugin library descriptor
*/

maria_declare_plugin(versioning)
{
  MYSQL_REPLICATION_PLUGIN, // initialized after MYSQL_STORAGE_ENGINE_PLUGIN
  &versioning_plugin,
  "versioning",
  "MariaDB Corp",
  "System Vesioning testing features",
  PLUGIN_LICENSE_GPL,
  versioning_plugin_init, /* Plugin Init */
  versioning_plugin_deinit, /* Plugin Deinit */
  0x0100 /* 1.0 */,
  NULL,                       /* status variables                */
  NULL,                       /* system variables                */
  "1.0",                      /* string version */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL /* maturity */
}
maria_declare_plugin_end;

#ifndef VERS_UTILS_INCLUDED
#define VERS_UTILS_INCLUDED

#include "table.h"
#include "sql_class.h"
#include "vers_string.h"

class MDL_auto_lock
{
  THD *thd;
  TABLE_LIST &table;
  bool error;

public:
  MDL_auto_lock(THD *_thd, TABLE_LIST &_table) :
    thd(_thd), table(_table)
  {
    DBUG_ASSERT(thd);
    MDL_request protection_request;
    if (thd->global_read_lock.can_acquire_protection())
    {
      error= true;
      return;
    }
    protection_request.init(MDL_key::GLOBAL, "", "", MDL_INTENTION_EXCLUSIVE,
                            MDL_EXPLICIT);
    error= thd->mdl_context.acquire_lock(&protection_request, thd->variables.lock_wait_timeout);
    if (error)
      return;

    table.mdl_request.init(MDL_key::TABLE, table.db.str, table.table_name.str, MDL_EXCLUSIVE, MDL_EXPLICIT);
    error= thd->mdl_context.acquire_lock(&table.mdl_request, thd->variables.lock_wait_timeout);
    thd->mdl_context.release_lock(protection_request.ticket);
  }
  ~MDL_auto_lock()
  {
    if (!error)
    {
      DBUG_ASSERT(table.mdl_request.ticket);
      thd->mdl_context.release_lock(table.mdl_request.ticket);
      table.mdl_request.ticket= NULL;
    }
  }
  bool acquire_error() const { return error; }
};

#endif // VERS_UTILS_INCLUDED

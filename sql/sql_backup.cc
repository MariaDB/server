/* Copyright (c) 2026, MariaDB plc

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

#include "my_global.h"
#include "mdl.h"
#include "mysys_err.h"
#include "sql_class.h"
#include "sql_backup.h"
#include "sql_parse.h"

static my_bool backup_start(THD *thd, plugin_ref plugin, void *dst) noexcept
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->backup_start)
    return hton->backup_start(thd,
                              IF_WIN(static_cast<const char*>(dst),
                                     int(reinterpret_cast<uintptr_t>(dst))));
  return false;
}

static my_bool backup_end(THD *thd, plugin_ref plugin, void *arg) noexcept
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->backup_end)
    return hton->backup_end(thd, arg != nullptr);
  return false;
}

static my_bool backup_step(THD *thd, plugin_ref plugin, void *) noexcept
{
  handlerton *hton= plugin_hton(plugin);
  int res= 0;
  if (hton->backup_step)
    while ((res= hton->backup_step(thd)))
      if (res < 0)
        break;
  return res != 0;
}

static my_bool backup_finalize(THD *thd, plugin_ref plugin, void *) noexcept
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->backup_step)
    return hton->backup_finalize(thd);
  return 0;
}

bool Sql_cmd_backup::execute(THD *thd)
{
  if (check_global_access(thd, RELOAD_ACL) ||
      check_global_access(thd, SELECT_ACL) ||
      error_if_data_home_dir(target.str, "BACKUP SERVER TO"))
    return true;

  if (thd->current_backup_stage != BACKUP_FINISHED)
  {
    my_error(ER_BACKUP_LOCK_IS_ACTIVE, MYF(0));
    return true;
  }

  /* Block concurrent BACKUP SERVER and BACKUP STAGE */
  MDL_request mdl_request;
  MDL_REQUEST_INIT(&mdl_request, MDL_key::BACKUP, "", "", MDL_BACKUP_START,
                   MDL_EXPLICIT);

  if (thd->mdl_context.acquire_lock(&mdl_request,
                                    thd->variables.lock_wait_timeout))
    return true;

  if (my_mkdir(target.str, 0755, MYF(MY_WME)))
  {
#ifndef _WIN32
  err_exit:
#endif
    thd->mdl_context.release_lock(mdl_request.ticket);
    return true;
  }

#ifndef _WIN32
  int dir= open(target.str, O_DIRECTORY);
  if (dir < 0)
  {
    my_error(EE_CANT_MKDIR, MYF(ME_BELL), target.str, errno);
    goto err_exit;
  }
#endif

  bool fail= plugin_foreach_with_mask(thd, backup_start,
                                      MYSQL_STORAGE_ENGINE_PLUGIN,
                                      PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                                      IF_WIN(const_cast<char*>(target.str),
                                             reinterpret_cast<void*>(dir)));

  /* The backup_step may be invoked in multiple concurrent threads.
  At the time backup_end is invoked, all backup_step will have to complete. */
  if (!fail)
    fail= plugin_foreach_with_mask(thd, backup_step,
                                   MYSQL_STORAGE_ENGINE_PLUGIN,
                                   PLUGIN_IS_DELETED|PLUGIN_IS_READY, nullptr);

  /* FIXME: Escalate to MDL_BACKUP_BLOCK_DDL or similar */
  fail=
    plugin_foreach_with_mask(thd, backup_end, MYSQL_STORAGE_ENGINE_PLUGIN,
                             PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                             reinterpret_cast<void*>(fail)) || fail;

  /* The final part must not interfere with the use of the server datadir.
  Release the locks. */
  thd->mdl_context.release_lock(mdl_request.ticket);
  plugin_foreach_with_mask(thd, backup_finalize, MYSQL_STORAGE_ENGINE_PLUGIN,
                           PLUGIN_IS_DELETED|PLUGIN_IS_READY, nullptr);
#ifndef _WIN32
  close(dir);
#endif

  if (!fail)
    my_ok(thd);
  return fail;
}

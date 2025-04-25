/*
   Copyright (c) 2024, 2024, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/

/**
@file clone/src/clone_se.cc
Clone Plugin: Common SE data clone
*/

#include "handler.h"

/** Dummy SE handlerton for cloning common data and SEs that don't have clone
interfaces defined. */
struct handlerton clone_storage_engine;

static void clone_get_capability(Ha_clone_flagset &flags)
{
  flags.reset();
  flags.set(HA_CLONE_BLOCKING);
  flags.set(HA_CLONE_MULTI_TASK);
}

// static int clone_begin(THD *thd, const uchar *&loc, uint &loc_len,
//                        uint &task_id, Ha_clone_type type, Ha_clone_mode mode)
static int clone_begin(THD *, const uchar *&, uint &,
                       uint &, Ha_clone_type, Ha_clone_mode)
{
  return 0;
}

// static int clone_copy(THD *thd, const uchar *loc, uint loc_len, uint task_id,
//                       Ha_clone_stage stage, Ha_clone_cbk *cbk)
static int clone_copy(THD *, const uchar *, uint , uint ,
                      Ha_clone_stage , Ha_clone_cbk *)
{
  return 0;
}

// static int clone_ack(THD *thd, const uchar *loc, uint loc_len,
//                     uint task_id, int in_err, Ha_clone_cbk *cbk)
static int clone_ack(THD *, const uchar *, uint ,
                     uint , int , Ha_clone_cbk *)
{
  return 0;
}

// static int clone_end(THD *thd, const uchar *loc, uint loc_len,
//                      uint task_id, int in_err)
static int clone_end(THD *, const uchar *, uint ,
                     uint , int )
{
  return 0;
}

// static int clone_apply_begin(THD *thd, const uchar *&loc,
//                              uint &loc_len, uint &task_id, Ha_clone_mode mode,
//                              const char *data_dir)
static int clone_apply_begin(THD *, const uchar *&,
                             uint &, uint &, Ha_clone_mode ,
                             const char *)
{
  return 0;
}

// static int clone_apply(THD *thd, const uchar *loc,
//                        uint loc_len, uint task_id, int in_err,
//                        Ha_clone_cbk *cbk)
static int clone_apply(THD *, const uchar *,
                       uint , uint , int ,
                       Ha_clone_cbk *)
{
  return 0;
}

// static int clone_apply_end(THD *thd, const uchar *loc,
//                            uint loc_len, uint task_id, int in_err)
static int clone_apply_end(THD *, const uchar *,
                           uint , uint , int )
{
  return 0;
}

void init_clone_storage_engine()
{
  clone_storage_engine.db_type= DB_TYPE_UNKNOWN;

  auto &interface= clone_storage_engine.clone_interface;
  interface.clone_capability= clone_get_capability;

  interface.clone_begin= clone_begin;
  interface.clone_copy= clone_copy;
  interface.clone_ack= clone_ack;
  interface.clone_end= clone_end;

  interface.clone_apply_begin= clone_apply_begin;
  interface.clone_apply= clone_apply;
  interface.clone_apply_end= clone_apply_end;
}

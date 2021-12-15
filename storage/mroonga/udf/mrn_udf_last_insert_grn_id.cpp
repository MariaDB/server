/* -*- c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
  Copyright(C) 2010 Tetsuro IKEDA
  Copyright(C) 2010-2013 Kentoku SHIBA
  Copyright(C) 2011-2017 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include <mrn_mysql.h>
#include <mrn_windows.hpp>
#include <mrn_table.hpp>
#include <mrn_macro.hpp>
#include <mrn_current_thread.hpp>

MRN_BEGIN_DECLS

MRN_API my_bool last_insert_grn_id_init(UDF_INIT *init, UDF_ARGS *args, char *message)
{
  if (args->arg_count != 0) {
    strcpy(message, "last_insert_grn_id must not have arguments");
    return 1;
  }
  init->maybe_null = 0;
  return 0;
}

MRN_API longlong last_insert_grn_id(UDF_INIT *init, UDF_ARGS *args, char *is_null, char *error)
{
  THD *thd = current_thd;
  st_mrn_slot_data *slot_data = mrn_get_slot_data(thd, false);
  if (slot_data == NULL) {
    return 0;
  }
  longlong last_insert_record_id = slot_data->last_insert_record_id;
  return last_insert_record_id;
}

MRN_API void last_insert_grn_id_deinit(UDF_INIT *init)
{
}

MRN_END_DECLS

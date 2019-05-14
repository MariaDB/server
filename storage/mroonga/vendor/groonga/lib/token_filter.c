/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014-2015 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include <string.h>

#include "grn.h"
#include "grn_db.h"
#include <groonga/token_filter.h>

grn_rc
grn_token_filter_register(grn_ctx *ctx,
                          const char *plugin_name_ptr,
                          int plugin_name_length,
                          grn_token_filter_init_func *init,
                          grn_token_filter_filter_func *filter,
                          grn_token_filter_fin_func *fin)
{
  if (plugin_name_length == -1) {
    plugin_name_length = strlen(plugin_name_ptr);
  }

  {
    grn_obj *token_filter_object = grn_proc_create(ctx,
                                                   plugin_name_ptr,
                                                   plugin_name_length,
                                                   GRN_PROC_TOKEN_FILTER,
                                                   NULL, NULL, NULL, 0, NULL);
    if (token_filter_object == NULL) {
      GRN_PLUGIN_ERROR(ctx, GRN_TOKEN_FILTER_ERROR,
                       "[token-filter][%.*s] failed to grn_proc_create()",
                       plugin_name_length, plugin_name_ptr);
      return ctx->rc;
    }

    {
      grn_proc *token_filter = (grn_proc *)token_filter_object;
      token_filter->callbacks.token_filter.init = init;
      token_filter->callbacks.token_filter.filter = filter;
      token_filter->callbacks.token_filter.fin = fin;
    }
  }

  return GRN_SUCCESS;
}

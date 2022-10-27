/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010-2016 Brazil

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

#pragma once

#include "grn.h"
#include "grn_ctx.h"
#include "grn_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef WIN32
typedef HINSTANCE grn_dl;
typedef FARPROC grn_dl_symbol;

#else
typedef void * grn_dl;
typedef void * grn_dl_symbol;
#endif

typedef struct _grn_plugin grn_plugin;

struct _grn_plugin {
  char path[PATH_MAX];
  grn_dl dl;
  grn_plugin_func init_func;
  grn_plugin_func register_func;
  grn_plugin_func unregister_func;
  grn_plugin_func fin_func;
  int refcount;
};

void grn_plugin_init_from_env(void);
grn_rc grn_plugins_init(void);
grn_rc grn_plugins_fin(void);
grn_id grn_plugin_open(grn_ctx *ctx, const char *filename);
grn_rc grn_plugin_close(grn_ctx *ctx, grn_id id);
grn_id grn_plugin_reference(grn_ctx *ctx, const char *filename);
const char *grn_plugin_path(grn_ctx *ctx, grn_id id);
char *grn_plugin_find_path(grn_ctx *ctx, const char *name);
void grn_plugin_ensure_registered(grn_ctx *ctx, grn_obj *proc);

#ifdef __cplusplus
}
#endif

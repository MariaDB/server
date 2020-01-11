/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2012-2016 Brazil

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

#include <stddef.h>

#include <groonga/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

/*
  grn_normalizer_register() registers a normalizer to the database
  which is associated with `ctx'. `name_ptr' and `name_length' specify
  the normalizer name. `name_length' can be `-1'. `-1' means that
  `name_ptr` is NULL-terminated. Alphabetic letters ('A'-'Z' and
  'a'-'z'), digits ('0'-'9') and an underscore ('_') are capable
  characters. `init', `next' and `fin' specify the normalizer
  functions. `init' is called for initializing a tokenizer for a
  document or query. `next' is called for extracting tokens one by
  one. `fin' is called for finalizing a
  tokenizer. grn_tokenizer_register() returns GRN_SUCCESS on success,
  an error code on failure. See "groonga.h" for more details of
  grn_proc_func and grn_user_data, that is used as an argument of
  grn_proc_func.
 */
GRN_PLUGIN_EXPORT grn_rc grn_normalizer_register(grn_ctx *ctx,
                                                 const char *name_ptr,
                                                 int name_length,
                                                 grn_proc_func *init,
                                                 grn_proc_func *next,
                                                 grn_proc_func *fin);

#ifdef __cplusplus
}  /* extern "C" */
#endif  /* __cplusplus */

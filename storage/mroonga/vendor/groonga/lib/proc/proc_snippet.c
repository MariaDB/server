/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2016 Brazil

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

#include "../grn_proc.h"
#include "../grn_expr.h"

#include <groonga/plugin.h>
#include <string.h>

#define GRN_FUNC_SNIPPET_HTML_CACHE_NAME "$snippet_html"

static grn_obj *
snippet_exec(grn_ctx *ctx, grn_obj *snip, grn_obj *text,
             grn_user_data *user_data,
             const char *prefix, int prefix_length,
             const char *suffix, int suffix_length)
{
  grn_rc rc;
  unsigned int i, n_results, max_tagged_length;
  grn_obj snippet_buffer;
  grn_obj *snippets;

  if (GRN_TEXT_LEN(text) == 0) {
    return NULL;
  }

  rc = grn_snip_exec(ctx, snip,
                     GRN_TEXT_VALUE(text), GRN_TEXT_LEN(text),
                     &n_results, &max_tagged_length);
  if (rc != GRN_SUCCESS) {
    return NULL;
  }

  if (n_results == 0) {
    return grn_plugin_proc_alloc(ctx, user_data, GRN_DB_VOID, 0);
  }

  snippets = grn_plugin_proc_alloc(ctx, user_data, GRN_DB_SHORT_TEXT, GRN_OBJ_VECTOR);
  if (!snippets) {
    return NULL;
  }

  GRN_TEXT_INIT(&snippet_buffer, 0);
  grn_bulk_space(ctx, &snippet_buffer,
                 prefix_length + max_tagged_length + suffix_length);
  for (i = 0; i < n_results; i++) {
    unsigned int snippet_length;

    GRN_BULK_REWIND(&snippet_buffer);
    if (prefix_length) {
      GRN_TEXT_PUT(ctx, &snippet_buffer, prefix, prefix_length);
    }
    rc = grn_snip_get_result(ctx, snip, i,
                             GRN_TEXT_VALUE(&snippet_buffer) + prefix_length,
                             &snippet_length);
    if (rc == GRN_SUCCESS) {
      grn_strncat(GRN_TEXT_VALUE(&snippet_buffer),
                  GRN_BULK_WSIZE(&snippet_buffer),
                  suffix,
                  suffix_length);
      grn_vector_add_element(ctx, snippets,
                             GRN_TEXT_VALUE(&snippet_buffer),
                             prefix_length + snippet_length + suffix_length,
                             0, GRN_DB_SHORT_TEXT);
    }
  }
  GRN_OBJ_FIN(ctx, &snippet_buffer);

  return snippets;
}

/* TODO: support caching for the same parameter. */
static grn_obj *
func_snippet(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *snippets = NULL;

#define N_REQUIRED_ARGS 1
#define KEYWORD_SET_SIZE 3
  if (nargs > N_REQUIRED_ARGS) {
    grn_obj *text = args[0];
    grn_obj *end_arg = args[nargs - 1];
    grn_obj *snip = NULL;
    unsigned int width = 200;
    unsigned int max_n_results = 3;
    grn_snip_mapping *mapping = NULL;
    int flags = GRN_SNIP_SKIP_LEADING_SPACES;
    const char *prefix = NULL;
    int prefix_length = 0;
    const char *suffix = NULL;
    int suffix_length = 0;
    const char *normalizer_name = NULL;
    int normalizer_name_length = 0;
    const char *default_open_tag = NULL;
    int default_open_tag_length = 0;
    const char *default_close_tag = NULL;
    int default_close_tag_length = 0;
    int n_args_without_option = nargs;

    if (end_arg->header.type == GRN_TABLE_HASH_KEY) {
      grn_obj *options = end_arg;
      grn_hash_cursor *cursor;
      void *key;
      int key_size;
      grn_obj *value;

      n_args_without_option--;
      cursor = grn_hash_cursor_open(ctx, (grn_hash *)options,
                                    NULL, 0, NULL, 0,
                                    0, -1, 0);
      if (!cursor) {
        GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                         "snippet(): couldn't open cursor");
        goto exit;
      }
      while (grn_hash_cursor_next(ctx, cursor) != GRN_ID_NIL) {
        grn_hash_cursor_get_key_value(ctx, cursor,
                                      &key, &key_size,
                                      (void **)&value);
        if (key_size == 5 && !memcmp(key, "width", 5)) {
          width = GRN_UINT32_VALUE(value);
        } else if (key_size == 13 && !memcmp(key, "max_n_results", 13)) {
          max_n_results = GRN_UINT32_VALUE(value);
        } else if (key_size == 19 && !memcmp(key, "skip_leading_spaces", 19)) {
          if (GRN_BOOL_VALUE(value) == GRN_FALSE) {
            flags &= ~GRN_SNIP_SKIP_LEADING_SPACES;
          }
        } else if (key_size == 11 && !memcmp(key, "html_escape", 11)) {
          if (GRN_BOOL_VALUE(value)) {
            mapping = GRN_SNIP_MAPPING_HTML_ESCAPE;
          }
        } else if (key_size == 6 && !memcmp(key, "prefix", 6)) {
          prefix = GRN_TEXT_VALUE(value);
          prefix_length = GRN_TEXT_LEN(value);
        } else if (key_size == 6 && !memcmp(key, "suffix", 6)) {
          suffix = GRN_TEXT_VALUE(value);
          suffix_length = GRN_TEXT_LEN(value);
        } else if (key_size == 10 && !memcmp(key, "normalizer", 10)) {
          normalizer_name = GRN_TEXT_VALUE(value);
          normalizer_name_length = GRN_TEXT_LEN(value);
        } else if (key_size == 16 && !memcmp(key, "default_open_tag", 16)) {
          default_open_tag = GRN_TEXT_VALUE(value);
          default_open_tag_length = GRN_TEXT_LEN(value);
        } else if (key_size == 17 && !memcmp(key, "default_close_tag", 17)) {
          default_close_tag = GRN_TEXT_VALUE(value);
          default_close_tag_length = GRN_TEXT_LEN(value);
        } else {
          GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                           "invalid option name: <%.*s>",
                           key_size, (char *)key);
          grn_hash_cursor_close(ctx, cursor);
          goto exit;
        }
      }
      grn_hash_cursor_close(ctx, cursor);
    }

    snip = grn_snip_open(ctx, flags, width, max_n_results,
                         default_open_tag, default_open_tag_length,
                         default_close_tag, default_close_tag_length, mapping);
    if (snip) {
      grn_rc rc;
      unsigned int i;
      if (!normalizer_name) {
        grn_snip_set_normalizer(ctx, snip, GRN_NORMALIZER_AUTO);
      } else if (normalizer_name_length > 0) {
        grn_obj *normalizer;
        normalizer = grn_ctx_get(ctx, normalizer_name, normalizer_name_length);
        if (!grn_obj_is_normalizer_proc(ctx, normalizer)) {
          grn_obj inspected;
          GRN_TEXT_INIT(&inspected, 0);
          grn_inspect(ctx, &inspected, normalizer);
          GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                           "snippet(): not normalizer: <%.*s>",
                           (int)GRN_TEXT_LEN(&inspected),
                           GRN_TEXT_VALUE(&inspected));
          GRN_OBJ_FIN(ctx, &inspected);
          grn_obj_unlink(ctx, normalizer);
          goto exit;
        }
        grn_snip_set_normalizer(ctx, snip, normalizer);
        grn_obj_unlink(ctx, normalizer);
      }
      if (default_open_tag_length == 0 && default_close_tag_length == 0) {
        unsigned int n_keyword_sets =
          (n_args_without_option - N_REQUIRED_ARGS) / KEYWORD_SET_SIZE;
        grn_obj **keyword_set_args = args + N_REQUIRED_ARGS;
        for (i = 0; i < n_keyword_sets; i++) {
          rc = grn_snip_add_cond(ctx, snip,
                                 GRN_TEXT_VALUE(keyword_set_args[i * KEYWORD_SET_SIZE]),
                                 GRN_TEXT_LEN(keyword_set_args[i * KEYWORD_SET_SIZE]),
                                 GRN_TEXT_VALUE(keyword_set_args[i * KEYWORD_SET_SIZE + 1]),
                                 GRN_TEXT_LEN(keyword_set_args[i * KEYWORD_SET_SIZE + 1]),
                                 GRN_TEXT_VALUE(keyword_set_args[i * KEYWORD_SET_SIZE + 2]),
                                 GRN_TEXT_LEN(keyword_set_args[i * KEYWORD_SET_SIZE + 2]));
        }
      } else {
        unsigned int n_keywords = n_args_without_option - N_REQUIRED_ARGS;
        grn_obj **keyword_args = args + N_REQUIRED_ARGS;
        for (i = 0; i < n_keywords; i++) {
          rc = grn_snip_add_cond(ctx, snip,
                                 GRN_TEXT_VALUE(keyword_args[i]),
                                 GRN_TEXT_LEN(keyword_args[i]),
                                 NULL, 0,
                                 NULL, 0);
        }
      }
      snippets = snippet_exec(ctx, snip, text, user_data,
                              prefix, prefix_length,
                              suffix, suffix_length);
    }
  }
#undef KEYWORD_SET_SIZE
#undef N_REQUIRED_ARGS

exit :
  if (!snippets) {
    snippets = grn_plugin_proc_alloc(ctx, user_data, GRN_DB_VOID, 0);
  }

  return snippets;
}

void
grn_proc_init_snippet(grn_ctx *ctx)
{
  grn_proc_create(ctx, "snippet", -1, GRN_PROC_FUNCTION,
                  func_snippet, NULL, NULL, 0, NULL);
}

static grn_obj *
func_snippet_html(grn_ctx *ctx, int nargs, grn_obj **args,
                  grn_user_data *user_data)
{
  grn_obj *snippets = NULL;

  /* TODO: support parameters */
  if (nargs == 1) {
    grn_obj *text = args[0];
    grn_obj *expression = NULL;
    grn_obj *condition_ptr = NULL;
    grn_obj *condition = NULL;
    grn_obj *snip = NULL;
    int flags = GRN_SNIP_SKIP_LEADING_SPACES;
    unsigned int width = 200;
    unsigned int max_n_results = 3;
    const char *open_tag = "<span class=\"keyword\">";
    const char *close_tag = "</span>";
    grn_snip_mapping *mapping = GRN_SNIP_MAPPING_HTML_ESCAPE;

    grn_proc_get_info(ctx, user_data, NULL, NULL, &expression);
    condition_ptr = grn_expr_get_var(ctx, expression,
                                     GRN_SELECT_INTERNAL_VAR_CONDITION,
                                     strlen(GRN_SELECT_INTERNAL_VAR_CONDITION));
    if (condition_ptr) {
      condition = GRN_PTR_VALUE(condition_ptr);
    }

    if (condition) {
      grn_obj *snip_ptr;
      snip_ptr = grn_expr_get_var(ctx, expression,
                                  GRN_FUNC_SNIPPET_HTML_CACHE_NAME,
                                  strlen(GRN_FUNC_SNIPPET_HTML_CACHE_NAME));
      if (snip_ptr) {
        snip = GRN_PTR_VALUE(snip_ptr);
      } else {
        snip_ptr =
          grn_expr_get_or_add_var(ctx, expression,
                                  GRN_FUNC_SNIPPET_HTML_CACHE_NAME,
                                  strlen(GRN_FUNC_SNIPPET_HTML_CACHE_NAME));
        GRN_OBJ_FIN(ctx, snip_ptr);
        GRN_PTR_INIT(snip_ptr, GRN_OBJ_OWN, GRN_DB_OBJECT);

        snip = grn_snip_open(ctx, flags, width, max_n_results,
                             open_tag, strlen(open_tag),
                             close_tag, strlen(close_tag),
                             mapping);
        if (snip) {
          grn_snip_set_normalizer(ctx, snip, GRN_NORMALIZER_AUTO);
          grn_expr_snip_add_conditions(ctx, condition, snip,
                                       0, NULL, NULL, NULL, NULL);
          GRN_PTR_SET(ctx, snip_ptr, snip);
        }
      }
    }

    if (snip) {
      snippets = snippet_exec(ctx, snip, text, user_data, NULL, 0, NULL, 0);
    }
  }

  if (!snippets) {
    snippets = grn_plugin_proc_alloc(ctx, user_data, GRN_DB_VOID, 0);
  }

  return snippets;
}

void
grn_proc_init_snippet_html(grn_ctx *ctx)
{
  grn_proc_create(ctx, "snippet_html", -1, GRN_PROC_FUNCTION,
                  func_snippet_html, NULL, NULL, 0, NULL);
}

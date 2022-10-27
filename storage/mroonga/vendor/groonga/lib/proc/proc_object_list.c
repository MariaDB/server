/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2016 Brazil

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
#include "../grn_db.h"

#include <groonga/plugin.h>

static void
command_object_list_dump_flags(grn_ctx *ctx, grn_obj_spec *spec)
{
  grn_obj flags;

  GRN_TEXT_INIT(&flags, 0);

  switch (spec->header.type) {
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
  case GRN_TABLE_NO_KEY :
    grn_dump_table_create_flags(ctx, spec->header.flags, &flags);
    break;
  case GRN_COLUMN_VAR_SIZE :
  case GRN_COLUMN_FIX_SIZE :
  case GRN_COLUMN_INDEX :
    grn_dump_column_create_flags(ctx, spec->header.flags, &flags);
    break;
  case GRN_TYPE :
    if (spec->header.flags & GRN_OBJ_KEY_VAR_SIZE) {
      GRN_TEXT_PUTS(ctx, &flags, "KEY_VAR_SIZE");
    } else {
      switch (spec->header.flags & GRN_OBJ_KEY_MASK) {
      case GRN_OBJ_KEY_UINT :
        GRN_TEXT_PUTS(ctx, &flags, "KEY_UINT");
        break;
      case GRN_OBJ_KEY_INT :
        GRN_TEXT_PUTS(ctx, &flags, "KEY_INT");
        break;
      case GRN_OBJ_KEY_FLOAT :
        GRN_TEXT_PUTS(ctx, &flags, "KEY_FLOAT");
        break;
      case GRN_OBJ_KEY_GEO_POINT :
        GRN_TEXT_PUTS(ctx, &flags, "KEY_GEO_POINT");
        break;
      }
    }
    break;
  }
  if (spec->header.flags & GRN_OBJ_CUSTOM_NAME) {
    if (GRN_TEXT_LEN(&flags) > 0) {
      GRN_TEXT_PUTS(ctx, &flags, "|");
    }
    GRN_TEXT_PUTS(ctx, &flags, "CUSTOM_NAME");
  }

  grn_ctx_output_str(ctx, GRN_TEXT_VALUE(&flags), GRN_TEXT_LEN(&flags));

  GRN_OBJ_FIN(ctx, &flags);
}

static grn_obj *
command_object_list(grn_ctx *ctx,
                    int nargs,
                    grn_obj **args,
                    grn_user_data *user_data)
{
  grn_db *db;
  uint32_t n_objects = 0;
  grn_obj vector;

  db = (grn_db *)grn_ctx_db(ctx);
  if (!db->specs) {
    grn_ctx_output_map_open(ctx, "objects", n_objects);
    grn_ctx_output_map_close(ctx);
    return NULL;
  }

  GRN_TABLE_EACH_BEGIN_FLAGS(ctx, (grn_obj *)db, cursor, id,
                             GRN_CURSOR_BY_ID | GRN_CURSOR_ASCENDING) {
    grn_io_win jw;
    uint32_t value_len;
    char *value;

    value = grn_ja_ref(ctx, db->specs, id, &jw, &value_len);
    if (value) {
      n_objects++;
      grn_ja_unref(ctx, &jw);
    }
  } GRN_TABLE_EACH_END(ctx, cursor);

  GRN_OBJ_INIT(&vector, GRN_VECTOR, 0, GRN_DB_TEXT);

  grn_ctx_output_map_open(ctx, "objects", n_objects);
  GRN_TABLE_EACH_BEGIN_FLAGS(ctx, (grn_obj *)db, cursor, id,
                             GRN_CURSOR_BY_ID | GRN_CURSOR_ASCENDING) {
    void *name;
    int name_size;
    grn_io_win jw;
    uint32_t value_len;
    char *value;
    unsigned int n_elements;

    value = grn_ja_ref(ctx, db->specs, id, &jw, &value_len);
    if (!value) {
      continue;
    }

    name_size = grn_table_cursor_get_key(ctx, cursor, &name);

    grn_ctx_output_str(ctx, name, name_size);

    GRN_BULK_REWIND(&vector);
    if (grn_vector_decode(ctx, &vector, value, value_len) != GRN_SUCCESS) {
      grn_ctx_output_map_open(ctx, "object", 4);
      {
        grn_ctx_output_cstr(ctx, "id");
        grn_ctx_output_int64(ctx, id);
        grn_ctx_output_cstr(ctx, "name");
        grn_ctx_output_str(ctx, name, name_size);
        grn_ctx_output_cstr(ctx, "opened");
        grn_ctx_output_bool(ctx, grn_ctx_is_opened(ctx, id));
        grn_ctx_output_cstr(ctx, "value_size");
        grn_ctx_output_uint64(ctx, value_len);
      }
      grn_ctx_output_map_close(ctx);
      goto next;
    }

    n_elements = grn_vector_size(ctx, &vector);

    {
      uint32_t element_size;
      grn_obj_spec *spec;
      uint32_t n_properties = 8;
      grn_bool need_sources = GRN_FALSE;
      grn_bool need_token_filters = GRN_FALSE;

      element_size = grn_vector_get_element(ctx,
                                            &vector,
                                            GRN_SERIALIZED_SPEC_INDEX_SPEC,
                                            (const char **)&spec,
                                            NULL,
                                            NULL);
      if (element_size == 0) {
        grn_ctx_output_map_open(ctx, "object", 4);
        {
          grn_ctx_output_cstr(ctx, "id");
          grn_ctx_output_int64(ctx, id);
          grn_ctx_output_cstr(ctx, "name");
          grn_ctx_output_str(ctx, name, name_size);
          grn_ctx_output_cstr(ctx, "opened");
          grn_ctx_output_bool(ctx, grn_ctx_is_opened(ctx, id));
          grn_ctx_output_cstr(ctx, "n_elements");
          grn_ctx_output_uint64(ctx, n_elements);
        }
        grn_ctx_output_map_close(ctx);
        goto next;
      }

      switch (spec->header.type) {
      case GRN_COLUMN_INDEX :
        need_sources = GRN_TRUE;
        n_properties++;
        break;
      case GRN_TABLE_PAT_KEY :
      case GRN_TABLE_DAT_KEY :
      case GRN_TABLE_HASH_KEY :
      case GRN_TABLE_NO_KEY :
        need_token_filters = GRN_TRUE;
        n_properties++;
        break;
      }
      grn_ctx_output_map_open(ctx, "object", n_properties);
      {
        grn_ctx_output_cstr(ctx, "id");
        grn_ctx_output_uint64(ctx, id);

        grn_ctx_output_cstr(ctx, "name");
        grn_ctx_output_str(ctx, name, name_size);

        grn_ctx_output_cstr(ctx, "opened");
        grn_ctx_output_bool(ctx, grn_ctx_is_opened(ctx, id));

        grn_ctx_output_cstr(ctx, "n_elements");
        grn_ctx_output_uint64(ctx, n_elements);

        grn_ctx_output_cstr(ctx, "type");
        grn_ctx_output_map_open(ctx, "type", 2);
        {
          grn_ctx_output_cstr(ctx, "id");
          grn_ctx_output_uint64(ctx, spec->header.type);
          grn_ctx_output_cstr(ctx, "name");
          grn_ctx_output_cstr(ctx, grn_obj_type_to_string(spec->header.type));
        }
        grn_ctx_output_map_close(ctx);

        grn_ctx_output_cstr(ctx, "flags");
        grn_ctx_output_map_open(ctx, "flags", 2);
        {
          grn_ctx_output_cstr(ctx, "value");
          grn_ctx_output_uint64(ctx, spec->header.flags);
          grn_ctx_output_cstr(ctx, "names");
          command_object_list_dump_flags(ctx, spec);
        }
        grn_ctx_output_map_close(ctx);

        grn_ctx_output_cstr(ctx, "path");
        if (spec->header.flags & GRN_OBJ_CUSTOM_NAME) {
          const char *path;
          uint32_t path_size;
          path_size = grn_vector_get_element(ctx,
                                             &vector,
                                             GRN_SERIALIZED_SPEC_INDEX_PATH,
                                             &path,
                                             NULL,
                                             NULL);
          grn_ctx_output_str(ctx, path, path_size);
        } else {
          switch (spec->header.type) {
          case GRN_TABLE_HASH_KEY :
          case GRN_TABLE_PAT_KEY :
          case GRN_TABLE_DAT_KEY :
          case GRN_TABLE_NO_KEY :
          case GRN_COLUMN_VAR_SIZE :
          case GRN_COLUMN_FIX_SIZE :
          case GRN_COLUMN_INDEX :
            {
              char path[PATH_MAX];
              grn_db_generate_pathname(ctx, (grn_obj *)db, id, path);
              grn_ctx_output_cstr(ctx, path);
            }
            break;
          default :
            grn_ctx_output_null(ctx);
            break;
          }
        }

        switch (spec->header.type) {
        case GRN_TYPE :
          grn_ctx_output_cstr(ctx, "size");
          grn_ctx_output_uint64(ctx, spec->range);
          break;
        case GRN_PROC :
          grn_ctx_output_cstr(ctx, "plugin_id");
          grn_ctx_output_uint64(ctx, spec->range);
          break;
        default :
          grn_ctx_output_cstr(ctx, "range");
          grn_ctx_output_map_open(ctx, "range", 2);
          {
            char name[GRN_TABLE_MAX_KEY_SIZE];
            int name_size;

            name_size = grn_table_get_key(ctx,
                                          (grn_obj *)db,
                                          spec->range,
                                          name,
                                          GRN_TABLE_MAX_KEY_SIZE);

            grn_ctx_output_cstr(ctx, "id");
            grn_ctx_output_uint64(ctx, spec->range);

            grn_ctx_output_cstr(ctx, "name");
            if (name_size == 0) {
              grn_ctx_output_null(ctx);
            } else {
              grn_ctx_output_str(ctx, name, name_size);
            }
          }
          grn_ctx_output_map_close(ctx);
          break;
        }

        if (need_sources) {
          const grn_id *source_ids;
          uint32_t n_source_ids;
          uint32_t i;

          if (n_elements > GRN_SERIALIZED_SPEC_INDEX_SOURCE) {
            uint32_t element_size;

            element_size = grn_vector_get_element(ctx,
                                                  &vector,
                                                  GRN_SERIALIZED_SPEC_INDEX_SOURCE,
                                                  (const char **)&source_ids,
                                                  NULL,
                                                  NULL);
            n_source_ids = element_size / sizeof(grn_id);
          } else {
            source_ids = NULL;
            n_source_ids = 0;
          }

          grn_ctx_output_cstr(ctx, "sources");
          grn_ctx_output_array_open(ctx, "sources", n_source_ids);
          for (i = 0; i < n_source_ids; i++) {
            grn_id source_id;
            char name[GRN_TABLE_MAX_KEY_SIZE];
            int name_size;

            source_id = source_ids[i];
            name_size = grn_table_get_key(ctx,
                                          (grn_obj *)db,
                                          source_id,
                                          name,
                                          GRN_TABLE_MAX_KEY_SIZE);

            grn_ctx_output_map_open(ctx, "source", 2);
            {
              grn_ctx_output_cstr(ctx, "id");
              grn_ctx_output_uint64(ctx, source_id);

              grn_ctx_output_cstr(ctx, "name");
              if (name_size == 0) {
                grn_ctx_output_null(ctx);
              } else {
                grn_ctx_output_str(ctx, name, name_size);
              }
            }
            grn_ctx_output_map_close(ctx);
          }
          grn_ctx_output_array_close(ctx);
        }

        if (need_token_filters) {
          const grn_id *token_filter_ids;
          uint32_t n_token_filter_ids;
          uint32_t i;

          if (n_elements > GRN_SERIALIZED_SPEC_INDEX_TOKEN_FILTERS) {
            uint32_t element_size;

            element_size = grn_vector_get_element(ctx,
                                                  &vector,
                                                  GRN_SERIALIZED_SPEC_INDEX_TOKEN_FILTERS,
                                                  (const char **)&token_filter_ids,
                                                  NULL,
                                                  NULL);
            n_token_filter_ids = element_size / sizeof(grn_id);
          } else {
            token_filter_ids = NULL;
            n_token_filter_ids = 0;
          }

          grn_ctx_output_cstr(ctx, "token_filters");
          grn_ctx_output_array_open(ctx, "token_filters", n_token_filter_ids);
          for (i = 0; i < n_token_filter_ids; i++) {
            grn_id token_filter_id;
            char name[GRN_TABLE_MAX_KEY_SIZE];
            int name_size;

            token_filter_id = token_filter_ids[i];
            name_size = grn_table_get_key(ctx,
                                          (grn_obj *)db,
                                          token_filter_id,
                                          name,
                                          GRN_TABLE_MAX_KEY_SIZE);

            grn_ctx_output_map_open(ctx, "token_filter", 2);
            {
              grn_ctx_output_cstr(ctx, "id");
              grn_ctx_output_uint64(ctx, token_filter_id);

              grn_ctx_output_cstr(ctx, "name");
              if (name_size == 0) {
                grn_ctx_output_null(ctx);
              } else {
                grn_ctx_output_str(ctx, name, name_size);
              }
            }
            grn_ctx_output_map_close(ctx);
          }
          grn_ctx_output_array_close(ctx);
        }
      }
      grn_ctx_output_map_close(ctx);
    }

  next :
    grn_ja_unref(ctx, &jw);
  } GRN_TABLE_EACH_END(ctx, cursor);
  grn_ctx_output_map_close(ctx);

  GRN_OBJ_FIN(ctx, &vector);

  return NULL;
}

void
grn_proc_init_object_list(grn_ctx *ctx)
{
  grn_plugin_command_create(ctx,
                            "object_list", -1,
                            command_object_list,
                            0,
                            NULL);
}

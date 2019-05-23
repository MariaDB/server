/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2016 Brazil

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

#include "grn_report.h"

const grn_log_level GRN_REPORT_INDEX_LOG_LEVEL = GRN_LOG_INFO;

void
grn_report_index(grn_ctx *ctx,
                 const char *action,
                 const char *tag,
                 grn_obj *index)
{
  char index_name[GRN_TABLE_MAX_KEY_SIZE];
  int index_name_size;

  if (!grn_logger_pass(ctx, GRN_REPORT_INDEX_LOG_LEVEL)) {
    return;
  }

  index_name_size = grn_obj_name(ctx, index, index_name, GRN_TABLE_MAX_KEY_SIZE);
  GRN_LOG(ctx, GRN_REPORT_INDEX_LOG_LEVEL,
          "%s[index]%s <%.*s>",
          action, tag, index_name_size, index_name);
}

void
grn_report_index_not_used(grn_ctx *ctx,
                          const char *action,
                          const char *tag,
                          grn_obj *index,
                          const char *reason)
{
  char index_name[GRN_TABLE_MAX_KEY_SIZE];
  int index_name_size;

  if (!grn_logger_pass(ctx, GRN_REPORT_INDEX_LOG_LEVEL)) {
    return;
  }

  index_name_size = grn_obj_name(ctx, index, index_name, GRN_TABLE_MAX_KEY_SIZE);
  GRN_LOG(ctx, GRN_REPORT_INDEX_LOG_LEVEL,
          "%s[index-not-used]%s <%.*s>: %s",
          action, tag, index_name_size, index_name, reason);
}

void
grn_report_table(grn_ctx *ctx,
                 const char *action,
                 const char *tag,
                 grn_obj *table)
{
  grn_obj description;
  grn_obj *target;

  if (!grn_logger_pass(ctx, GRN_REPORT_INDEX_LOG_LEVEL)) {
    return;
  }

  GRN_TEXT_INIT(&description, 0);
  for (target = table; target; target = grn_ctx_at(ctx, target->header.domain)) {
    char name[GRN_TABLE_MAX_KEY_SIZE];
    int name_size;

    name_size = grn_obj_name(ctx, target, name, GRN_TABLE_MAX_KEY_SIZE);
    if (GRN_TEXT_LEN(&description) > 0) {
      GRN_TEXT_PUTS(ctx, &description, " -> ");
    }
    if (name_size == 0) {
      GRN_TEXT_PUTS(ctx, &description, "(temporary)");
    } else {
      GRN_TEXT_PUTS(ctx, &description, "<");
      GRN_TEXT_PUT(ctx, &description, name, name_size);
      GRN_TEXT_PUTS(ctx, &description, ">");
    }
  }
  GRN_LOG(ctx, GRN_REPORT_INDEX_LOG_LEVEL,
          "%s[table]%s %.*s",
          action, tag,
          (int)GRN_TEXT_LEN(&description),
          GRN_TEXT_VALUE(&description));
  GRN_OBJ_FIN(ctx, &description);
}

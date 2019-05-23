/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014 Brazil

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

#include <grn_mrb.h>

#include <stdio.h>
#include <stdlib.h>

static int
run(grn_ctx *ctx, const char *db_path, const char *ruby_script_path)
{
  grn_obj *db;

  db = grn_db_open(ctx, db_path);
  if (!db) {
    if (ctx->rc == GRN_NO_SUCH_FILE_OR_DIRECTORY) {
      db = grn_db_create(ctx, db_path, NULL);
      if (!db) {
        fprintf(stderr, "Failed to create database: <%s>: %s",
                db_path, ctx->errbuf);
        return EXIT_FAILURE;
      }
    } else {
      fprintf(stderr, "Failed to open database: <%s>: %s",
              db_path, ctx->errbuf);
      return EXIT_FAILURE;
    }
  }

  grn_mrb_load(ctx, ruby_script_path);
  if (ctx->rc != GRN_SUCCESS) {
      fprintf(stderr, "Failed to load Ruby script: <%s>: %s",
              ruby_script_path, ctx->errbuf);
  }

  grn_obj_close(ctx, db);

  if (ctx->rc == GRN_SUCCESS) {
    return EXIT_SUCCESS;
  } else {
    return EXIT_FAILURE;
  }
}

int
main(int argc, char **argv)
{
  int exit_code = EXIT_SUCCESS;

  if (argc != 3) {
    fprintf(stderr, "Usage: %s DB_PATH RUBY_SCRIPT_PATH\n", argv[0]);
    return EXIT_FAILURE;
  }

  grn_default_logger_set_path(GRN_LOG_PATH);

  if (grn_init() != GRN_SUCCESS) {
    return EXIT_FAILURE;
  }

  {
    grn_ctx ctx;
    grn_ctx_init(&ctx, 0);
    exit_code = run(&ctx, argv[1], argv[2]);
    grn_ctx_fin(&ctx);
  }

  grn_fin();

  return exit_code;
}

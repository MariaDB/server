/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Brazil

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

#include "grn_scanner.h"

grn_scanner *
grn_scanner_open(grn_ctx *ctx,
                 grn_obj *expr,
                 grn_operator op,
                 grn_bool record_exist)
{
  grn_scanner *scanner;

  scanner = GRN_MALLOC(sizeof(grn_scanner));
  if (!scanner) {
    return NULL;
  }

  scanner->source_expr = expr;
  scanner->expr = grn_expr_rewrite(ctx, expr);
  if (!scanner->expr) {
    scanner->expr = expr;
  }

  scanner->sis = grn_scan_info_build(ctx,
                                     scanner->expr,
                                     &(scanner->n_sis),
                                     op,
                                     record_exist);
  if (!scanner->sis) {
    grn_scanner_close(ctx, scanner);
    return NULL;
  }

  return scanner;
}

void
grn_scanner_close(grn_ctx *ctx, grn_scanner *scanner)
{
  if (!scanner) {
    return;
  }

  if (scanner->sis) {
    uint i;
    for (i = 0; i < scanner->n_sis; i++) {
      grn_scan_info_close(ctx, scanner->sis[i]);
    }
    GRN_FREE(scanner->sis);
  }

  if (scanner->expr != scanner->source_expr) {
    grn_obj_close(ctx, scanner->expr);
  }

  GRN_FREE(scanner);
}

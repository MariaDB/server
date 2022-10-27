/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2017 Kouhei Sutou <kou@clear-code.com>

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

#pragma once

#include <mrn_mysql.h>
#include <mrn_mysql_compat.h>

#include <groonga.h>

namespace mrn {
  class QueryParser {
  public:
    QueryParser(grn_ctx *ctx,
                THD *thd,
                grn_obj *expression,
                grn_obj *default_column,
                uint n_sections,
                grn_obj *match_columns=NULL);
    ~QueryParser();

    grn_rc parse(const char *query, size_t query_length);
    void parse_pragma(const char *query,
                      size_t query_length,
                      const char **raw_query,
                      size_t *raw_query_length,
                      grn_operator *default_operator,
                      grn_expr_flags *flags);

  private:
    grn_ctx *ctx_;
    THD *thd_;
    grn_obj *expression_;
    grn_obj *default_column_;
    uint n_sections_;
    grn_obj *match_columns_;

    bool parse_pragma_w(const char *query,
                        size_t query_length,
                        size_t *consumed_query_length);
    void append_section(uint section,
                        grn_obj *section_value_buffer,
                        int weight,
                        uint n_weights);
    bool parse_pragma_d(const char *query,
                        size_t query_length,
                        grn_operator *default_operator,
                        size_t *consumed_query_length);
    grn_expr_flags default_expression_flags();
  };
}

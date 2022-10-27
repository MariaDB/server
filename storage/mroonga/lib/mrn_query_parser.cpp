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

#include "mrn_query_parser.hpp"

#include <mrn_variables.hpp>

extern "C" {
  /* Groonga's internal functions */
  int grn_atoi(const char *nptr, const char *end, const char **rest);
  uint grn_atoui(const char *nptr, const char *end, const char **rest);
}

#define MRN_CLASS_NAME "mrn::QueryParser"

namespace mrn {
  QueryParser::QueryParser(grn_ctx *ctx,
                           THD *thd,
                           grn_obj *expression,
                           grn_obj *default_column,
                           uint n_sections,
                           grn_obj *match_columns)
    : ctx_(ctx),
      thd_(thd),
      expression_(expression),
      default_column_(default_column),
      n_sections_(n_sections),
      match_columns_(match_columns) {
  }

  QueryParser::~QueryParser() {
  }

  grn_rc QueryParser::parse(const char *query, size_t query_length) {
    MRN_DBUG_ENTER_METHOD();

    const char *raw_query = NULL;
    size_t raw_query_length = 0;
    grn_operator default_operator = GRN_OP_OR;
    grn_expr_flags expression_flags = 0;
    parse_pragma(query,
                 query_length,
                 &raw_query,
                 &raw_query_length,
                 &default_operator,
                 &expression_flags);

    grn_obj *default_column = default_column_;
    if (match_columns_) {
      default_column = match_columns_;
    }
    grn_rc rc = grn_expr_parse(ctx_,
                               expression_,
                               raw_query,
                               raw_query_length,
                               default_column,
                               GRN_OP_MATCH,
                               default_operator,
                               expression_flags);
    if (rc != GRN_SUCCESS) {
      char error_message[MRN_MESSAGE_BUFFER_SIZE];
      snprintf(error_message, MRN_MESSAGE_BUFFER_SIZE,
               "failed to parse fulltext search keyword: <%.*s>: <%s>",
               static_cast<int>(query_length),
               query,
               ctx_->errbuf);
      variables::ActionOnError action =
        variables::get_action_on_fulltext_query_error(thd_);
      switch (action) {
      case variables::ACTION_ON_ERROR_ERROR:
        my_message(ER_PARSE_ERROR, error_message, MYF(0));
        break;
      case variables::ACTION_ON_ERROR_ERROR_AND_LOG:
        my_message(ER_PARSE_ERROR, error_message, MYF(0));
        GRN_LOG(ctx_, GRN_LOG_ERROR, "%s", error_message);
        break;
      case variables::ACTION_ON_ERROR_IGNORE:
        break;
      case variables::ACTION_ON_ERROR_IGNORE_AND_LOG:
        GRN_LOG(ctx_, GRN_LOG_ERROR, "%s", error_message);
        break;
      }
    }

    DBUG_RETURN(rc);
  }

  void QueryParser::parse_pragma(const char *query,
                                 size_t query_length,
                                 const char **raw_query,
                                 size_t *raw_query_length,
                                 grn_operator *default_operator,
                                 grn_expr_flags *flags) {
    MRN_DBUG_ENTER_METHOD();

    const char *current_query = query;
    size_t current_query_length = query_length;

    *default_operator = GRN_OP_OR;

    if (current_query_length >= 4 && memcmp(current_query, "*SS ", 4) == 0) {
      *raw_query = current_query + 4;
      *raw_query_length = current_query_length - 4;
      *flags = GRN_EXPR_SYNTAX_SCRIPT;
      DBUG_VOID_RETURN;
    }

    bool weight_specified = false;
    *raw_query = query;
    *raw_query_length = query_length;
    *flags = default_expression_flags();
    if (current_query_length >= 2 && current_query[0] == '*') {
      bool parsed = false;
      bool done = false;
      current_query++;
      current_query_length--;
      while (!done) {
        size_t consumed_query_length = 0;
        switch (current_query[0]) {
        case 'D':
          if (parse_pragma_d(current_query + 1,
                             current_query_length - 1,
                             default_operator,
                             &consumed_query_length)) {
            parsed = true;
            consumed_query_length += 1;
            current_query += consumed_query_length;
            current_query_length -= consumed_query_length;
          } else {
            done = true;
          }
          break;
        case 'W':
          if (parse_pragma_w(current_query + 1,
                             current_query_length - 1,
                             &consumed_query_length)) {
            parsed = true;
            weight_specified = true;
            consumed_query_length += 1;
            current_query += consumed_query_length;
            current_query_length -= consumed_query_length;
          } else {
            done = true;
          }
          break;
        default:
          done = true;
          break;
        }
      }
      if (parsed) {
        *raw_query = current_query;
        *raw_query_length = current_query_length;
      }
    }

    // WORKAROUND: ignore the first '+' to support "+apple macintosh" pattern.
    while (*raw_query_length > 0 && (*raw_query)[0] == ' ') {
      (*raw_query)++;
      (*raw_query_length)--;
    }
    if (*raw_query_length > 0 && (*raw_query)[0] == '+') {
      (*raw_query)++;
      (*raw_query_length)--;
    }
    if (!weight_specified && match_columns_) {
      grn_expr_append_obj(ctx_, match_columns_, default_column_, GRN_OP_PUSH, 1);
    }

    DBUG_VOID_RETURN;
  }

  bool QueryParser::parse_pragma_w(const char *query,
                                   size_t query_length,
                                   size_t *consumed_query_length) {
    MRN_DBUG_ENTER_METHOD();

    *consumed_query_length = 0;

    grn_obj section_value_buffer;
    GRN_UINT32_INIT(&section_value_buffer, 0);

    MRN_ALLOCATE_VARIABLE_LENGTH_ARRAYS(bool, specified_sections, n_sections_);
    for (uint i = 0; i < n_sections_; ++i) {
      specified_sections[i] = false;
    }

    uint n_weights = 0;
    while (query_length >= 1) {
      if (n_weights >= 1) {
        if (query[0] != ',') {
          break;
        }
        size_t n_used_query_length = 1;
        *consumed_query_length += n_used_query_length;
        query_length -= n_used_query_length;
        query += n_used_query_length;
        if (query_length == 0) {
          break;
        }
      }

      uint section = 0;
      if ('1' <= query[0] && query[0] <= '9') {
        const char *section_start = query;
        const char *query_end = query + query_length;
        const char *query_rest;
        section = grn_atoui(section_start, query_end, &query_rest);
        if (section_start == query_rest) {
          break;
        }
        if (!(0 < section && section <= n_sections_)) {
          break;
        }
        section -= 1;
        specified_sections[section] = true;
        size_t n_used_query_length = query_rest - query;
        *consumed_query_length += n_used_query_length;
        query_length -= n_used_query_length;
        query += n_used_query_length;
      } else {
        break;
      }

      int weight = 1;
      if (query_length >= 2 && query[0] == ':') {
        const char *weight_start = query + 1;
        const char *query_end = query + query_length;
        const char *query_rest;
        weight = grn_atoi(weight_start, query_end, &query_rest);
        if (weight_start == query_rest) {
          break;
        }
        size_t n_used_query_length = query_rest - query;
        *consumed_query_length += n_used_query_length;
        query_length -= n_used_query_length;
        query += n_used_query_length;
      }

      n_weights++;

      append_section(section,
                     &section_value_buffer,
                     weight,
                     n_weights);
    }

    for (uint section = 0; section < n_sections_; ++section) {
      if (specified_sections[section]) {
        continue;
      }

      ++n_weights;

      int default_weight = 1;
     append_section(section,
                    &section_value_buffer,
                    default_weight,
                    n_weights);
    }
    MRN_FREE_VARIABLE_LENGTH_ARRAYS(specified_sections);

    GRN_OBJ_FIN(ctx_, &section_value_buffer);

    DBUG_RETURN(n_weights > 0);
  }

  void QueryParser::append_section(uint section,
                                   grn_obj *section_value_buffer,
                                   int weight,
                                   uint n_weights) {
    MRN_DBUG_ENTER_METHOD();

    if (!match_columns_) {
      DBUG_VOID_RETURN;
    }

    grn_expr_append_obj(ctx_, match_columns_, default_column_, GRN_OP_PUSH, 1);
    GRN_UINT32_SET(ctx_, section_value_buffer, section);
    grn_expr_append_const(ctx_, match_columns_, section_value_buffer,
                          GRN_OP_PUSH, 1);
    grn_expr_append_op(ctx_, match_columns_, GRN_OP_GET_MEMBER, 2);

    if (weight != 1) {
      grn_expr_append_const_int(ctx_, match_columns_, weight, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx_, match_columns_, GRN_OP_STAR, 2);
    }

    if (n_weights >= 2) {
      grn_expr_append_op(ctx_, match_columns_, GRN_OP_OR, 2);
    }

    DBUG_VOID_RETURN;
  }

  bool QueryParser::parse_pragma_d(const char *query,
                                   size_t query_length,
                                   grn_operator *default_operator,
                                   size_t *consumed_query_length) {
    MRN_DBUG_ENTER_METHOD();

    bool succeeded = true;
    if (query_length >= 1 && query[0] == '+') {
      *default_operator = GRN_OP_AND;
      *consumed_query_length = 1;
    } else if (query_length >= 1 && query[0] == '-') {
      *default_operator = GRN_OP_AND_NOT;
      *consumed_query_length = 1;
    } else if (query_length >= 2 && memcmp(query, "OR", 2) == 0) {
      *default_operator = GRN_OP_OR;
      *consumed_query_length = 2;
    } else {
      succeeded = false;
    }

    DBUG_RETURN(succeeded);
  }

  grn_expr_flags QueryParser::default_expression_flags() {
    MRN_DBUG_ENTER_METHOD();

    ulonglong syntax_flags = variables::get_boolean_mode_syntax_flags(thd_);
    grn_expr_flags expression_flags = 0;
    if (syntax_flags == variables::BOOLEAN_MODE_SYNTAX_FLAG_DEFAULT) {
      expression_flags = GRN_EXPR_SYNTAX_QUERY | GRN_EXPR_ALLOW_LEADING_NOT;
    } else {
      if (syntax_flags & variables::BOOLEAN_MODE_SYNTAX_FLAG_SYNTAX_SCRIPT) {
        expression_flags |= GRN_EXPR_SYNTAX_SCRIPT;
      } else {
        expression_flags |= GRN_EXPR_SYNTAX_QUERY;
      }
      if (syntax_flags & variables::BOOLEAN_MODE_SYNTAX_FLAG_ALLOW_COLUMN) {
        expression_flags |= GRN_EXPR_ALLOW_COLUMN;
      }
      if (syntax_flags & variables::BOOLEAN_MODE_SYNTAX_FLAG_ALLOW_UPDATE) {
        expression_flags |= GRN_EXPR_ALLOW_UPDATE;
      }
      if (syntax_flags & variables::BOOLEAN_MODE_SYNTAX_FLAG_ALLOW_LEADING_NOT) {
        expression_flags |= GRN_EXPR_ALLOW_LEADING_NOT;
      }
    }

    DBUG_RETURN(expression_flags);
  }
}

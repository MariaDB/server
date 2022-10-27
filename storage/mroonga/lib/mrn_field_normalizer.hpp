/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2015 Kouhei Sutou <kou@clear-code.com>

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

#ifndef MRN_FIELD_NORMALIZER_HPP_
#define MRN_FIELD_NORMALIZER_HPP_

#include <mrn_mysql.h>
#include <mrn_mysql_compat.h>

#include <groonga.h>

namespace mrn {
  class FieldNormalizer {
  public:
    FieldNormalizer(grn_ctx *ctx, THD *thread, Field *field);
    ~FieldNormalizer();

    bool should_normalize();
    grn_obj *normalize(const char *string, unsigned int string_length);
    grn_obj *find_grn_normalizer();

  private:
    grn_ctx *ctx_;
    THD *thread_;
    Field *field_;

    bool is_text_type();
  };
}

#endif // MRN_FIELD_NORMALIZER_HPP_

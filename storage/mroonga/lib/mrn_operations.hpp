/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Kouhei Sutou <kou@clear-code.com>

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

#ifndef MRN_OPERATIONS_HPP_
#define MRN_OPERATIONS_HPP_

#include <groonga.h>

namespace mrn {
  class Operations {
  public:
    Operations(grn_ctx *ctx);
    ~Operations();

    bool is_locked();

    grn_id start(const char *type,
                 const char *table_name, size_t table_name_size);
    void record_target(grn_id id, grn_id target_id);
    void finish(grn_id id);

    void enable_recording();
    void disable_recording();

    grn_hash *collect_processing_table_names();

    int repair(const char *table_name, size_t table_name_size);
    int clear(const char *table_name, size_t table_name_size);

  private:
    grn_ctx *ctx_;
    grn_obj text_buffer_;
    grn_obj id_buffer_;
    grn_obj *table_;
    struct {
      grn_obj *type_;
      grn_obj *table_;
      grn_obj *record_;
    } columns_;
    bool is_enabled_recording_;
  };
}

#endif /* MRN_OPERATIONS_HPP_ */

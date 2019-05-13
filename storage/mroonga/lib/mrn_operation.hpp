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

#ifndef MRN_OPERATION_HPP_
#define MRN_OPERATION_HPP_

#include <mrn_operations.hpp>

namespace mrn {
  class Operation {
  public:
    Operation(mrn::Operations *operations,
              const char *type,
              const char *table_name,
              size_t table_name_size);
    ~Operation();

    void record_target(grn_id record_id);

  private:
    mrn::Operations *operations_;
    grn_id id_;
  };
}

#endif /* MRN_OPERATION_HPP_ */

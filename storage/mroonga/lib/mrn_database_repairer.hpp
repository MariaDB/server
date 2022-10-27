/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2017 Kouhei Sutou <kou@clear-code.com>

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

#ifndef MRN_DATABASE_REPAIRER_HPP_
#define MRN_DATABASE_REPAIRER_HPP_

#include <groonga.h>

namespace mrn {
  class DatabaseRepairer {
  public:
    DatabaseRepairer(grn_ctx *ctx, THD *thd);
    ~DatabaseRepairer(void);
    bool is_crashed(void);
    bool is_corrupt(void);
    bool repair(void);

  private:
    grn_ctx *ctx_;
    THD *thd_;
    const char *base_directory_;
    char base_directory_buffer_[MRN_MAX_PATH_SIZE];
    const char *path_prefix_;
    char path_prefix_buffer_[MRN_MAX_PATH_SIZE];
    size_t path_prefix_length_;
    size_t mrn_db_file_suffix_length_;

    typedef void (DatabaseRepairer::*EachBodyFunc)(grn_ctx *ctx,
                                                   grn_obj *db,
                                                   const char *db_path,
                                                   void *user_data);

    void each_database(EachBodyFunc each_body_func, void *user_data);
    void each_database_body(const char *base_path,
                            grn_ctx *ctx,
                            EachBodyFunc each_body_func,
                            void *user_data);
    void detect_paths(void);

    void check_body(grn_ctx *ctx,
                    grn_obj *db,
                    const char *db_path,
                    void *user_data);
    void repair_body(grn_ctx *ctx,
                     grn_obj *db,
                     const char *db_path,
                     void *user_data);
  };
}

#endif /* MRN_DATABASE_REPAIRER_HPP_ */

/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010 Tetsuro IKEDA
  Copyright(C) 2010-2013 Kentoku SHIBA
  Copyright(C) 2011-2014 Kouhei Sutou <kou@clear-code.com>

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

#ifndef MRN_DATABASE_MANAGER_HPP_
#define MRN_DATABASE_MANAGER_HPP_

#include "mrn_database.hpp"

#include <groonga.h>

namespace mrn {
  class DatabaseManager {
  public:
    DatabaseManager(grn_ctx *ctx, mysql_mutex_t *mutex);
    ~DatabaseManager(void);
    bool init(void);
    int open(const char *path, Database **db);
    void close(const char *path);
    bool drop(const char *path);
    int clear(void);
    const char *error_message();

  private:
    grn_ctx *ctx_;
    grn_hash *cache_;
    mysql_mutex_t *mutex_;

    void mkdir_p(const char *directory);
    void ensure_database_directory(void);
    int ensure_normalizers_registered(grn_obj *db);
  };
}

#endif /* MRN_DATABASE_MANAGER_HPP_ */

/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010 Tetsuro IKEDA
  Copyright(C) 2010-2013 Kentoku SHIBA
  Copyright(C) 2011-2015 Kouhei Sutou <kou@clear-code.com>

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

#ifndef MRN_PATH_MAPPER_HPP_
#define MRN_PATH_MAPPER_HPP_

#include <mrn_constants.hpp>

namespace mrn {
  class PathMapper {
  public:
    static char *default_path_prefix;
    static char *default_mysql_data_home_path;

    PathMapper(const char *original_mysql_path,
               const char *path_prefix=default_path_prefix,
               const char *mysql_data_home_path=default_mysql_data_home_path);
    const char *db_path();
    const char *db_name();
    const char *table_name();
    const char *mysql_table_name();
    const char *mysql_path();
    bool is_internal_table_name();
    bool is_temporary_table_name();
  private:
    const char *original_mysql_path_;
    const char *path_prefix_;
    const char *mysql_data_home_path_;
    char db_path_[MRN_MAX_PATH_SIZE];
    char db_name_[MRN_MAX_PATH_SIZE];
    char table_name_[MRN_MAX_PATH_SIZE];
    char mysql_table_name_[MRN_MAX_PATH_SIZE];
    char mysql_path_[MRN_MAX_PATH_SIZE];
  };
}

#endif /* MRN_PATH_MAPPER_HPP_ */

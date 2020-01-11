/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010 Tetsuro IKEDA
  Copyright(C) 2011-2013 Kentoku SHIBA
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

#include <mrn_mysql.h>

#include "mrn_path_mapper.hpp"

#include <string.h>

namespace mrn {
  char *PathMapper::default_path_prefix = NULL;
  char *PathMapper::default_mysql_data_home_path = NULL;

  PathMapper::PathMapper(const char *original_mysql_path,
                         const char *path_prefix,
                         const char *mysql_data_home_path)
    : original_mysql_path_(original_mysql_path),
      path_prefix_(path_prefix),
      mysql_data_home_path_(mysql_data_home_path) {
    db_path_[0] = '\0';
    db_name_[0] = '\0';
    table_name_[0] = '\0';
    mysql_table_name_[0] = '\0';
    mysql_path_[0] = '\0';
  }

  /**
   * "./${db}/${table}"                              ==> "${db}.mrn"
   * "./${db}/"                                      ==> "${db}.mrn"
   * "/tmp/mysql-test/var/tmp/mysqld.1/#sql27c5_1_0" ==>
   *   "/tmp/mysql-test/var/tmp/mysqld.1/#sql27c5_1_0.mrn"
   */
  const char *PathMapper::db_path() {
    if (db_path_[0] != '\0') {
      return db_path_;
    }

    if (original_mysql_path_[0] == FN_CURLIB &&
        original_mysql_path_[1] == FN_LIBCHAR) {
      if (path_prefix_) {
        strcpy(db_path_, path_prefix_);
      }

      int i = 2, j = strlen(db_path_), len;
      len = strlen(original_mysql_path_);
      while (original_mysql_path_[i] != FN_LIBCHAR && i < len) {
        db_path_[j++] = original_mysql_path_[i++];
      }
      db_path_[j] = '\0';
    } else if (mysql_data_home_path_) {
      int len = strlen(original_mysql_path_);
      int mysql_data_home_len = strlen(mysql_data_home_path_);
      if (len > mysql_data_home_len &&
          !strncmp(original_mysql_path_,
                   mysql_data_home_path_,
                   mysql_data_home_len)) {
        int i = mysql_data_home_len, j;
        if (path_prefix_ && path_prefix_[0] == FN_LIBCHAR) {
          strcpy(db_path_, path_prefix_);
          j = strlen(db_path_);
        } else {
          memcpy(db_path_, mysql_data_home_path_, mysql_data_home_len);
          if (path_prefix_) {
            if (path_prefix_[0] == FN_CURLIB &&
                path_prefix_[1] == FN_LIBCHAR) {
              strcpy(&db_path_[mysql_data_home_len], &path_prefix_[2]);
            } else {
              strcpy(&db_path_[mysql_data_home_len], path_prefix_);
            }
            j = strlen(db_path_);
          } else {
            j = mysql_data_home_len;
          }
        }

        while (original_mysql_path_[i] != FN_LIBCHAR && i < len) {
          db_path_[j++] = original_mysql_path_[i++];
        }
        if (i == len) {
          memcpy(db_path_, original_mysql_path_, len);
        } else {
          db_path_[j] = '\0';
        }
      } else {
        strcpy(db_path_, original_mysql_path_);
      }
    } else {
      strcpy(db_path_, original_mysql_path_);
    }
    strcat(db_path_, MRN_DB_FILE_SUFFIX);
    return db_path_;
  }

  /**
   * "./${db}/${table}"                              ==> "${db}"
   * "./${db}/"                                      ==> "${db}"
   * "/tmp/mysql-test/var/tmp/mysqld.1/#sql27c5_1_0" ==>
   *   "/tmp/mysql-test/var/tmp/mysqld.1/#sql27c5_1_0"
   */
  const char *PathMapper::db_name() {
    if (db_name_[0] != '\0') {
      return db_name_;
    }

    if (original_mysql_path_[0] == FN_CURLIB &&
        original_mysql_path_[1] == FN_LIBCHAR) {
      int i = 2, j = 0, len;
      len = strlen(original_mysql_path_);
      while (original_mysql_path_[i] != FN_LIBCHAR && i < len) {
        db_name_[j++] = original_mysql_path_[i++];
      }
      db_name_[j] = '\0';
    } else if (mysql_data_home_path_) {
      int len = strlen(original_mysql_path_);
      int mysql_data_home_len = strlen(mysql_data_home_path_);
      if (len > mysql_data_home_len &&
          !strncmp(original_mysql_path_,
                   mysql_data_home_path_,
                   mysql_data_home_len)) {
        int i = mysql_data_home_len, j = 0;
        while (original_mysql_path_[i] != FN_LIBCHAR && i < len) {
          db_name_[j++] = original_mysql_path_[i++];
        }
        if (i == len) {
          memcpy(db_name_, original_mysql_path_, len);
        } else {
          db_name_[j] = '\0';
        }
      } else {
        strcpy(db_name_, original_mysql_path_);
      }
    } else {
      strcpy(db_name_, original_mysql_path_);
    }
    return db_name_;
  }

  /**
   * "./${db}/${table}" ==> "${table}" (with encoding first '_')
   */
  const char *PathMapper::table_name() {
    if (table_name_[0] != '\0') {
      return table_name_;
    }

    int len = strlen(original_mysql_path_);
    int i = len, j = 0;
    for (; original_mysql_path_[--i] != FN_LIBCHAR ;) {}
    if (original_mysql_path_[i + 1] == '_') {
      table_name_[j++] = '@';
      table_name_[j++] = '0';
      table_name_[j++] = '0';
      table_name_[j++] = '5';
      table_name_[j++] = 'f';
      i++;
    }
    for (; i < len ;) {
      table_name_[j++] = original_mysql_path_[++i];
    }
    table_name_[j] = '\0';
    return table_name_;
  }

  /**
   * "./${db}/${table}" ==> "${table}" (without encoding first '_')
   */
  const char *PathMapper::mysql_table_name() {
    if (mysql_table_name_[0] != '\0') {
      return mysql_table_name_;
    }

    int len = strlen(original_mysql_path_);
    int i = len, j = 0;
    for (; original_mysql_path_[--i] != FN_LIBCHAR ;) {}
    for (; i < len ;) {
      if (len - i - 1 >= 3 &&
          strncmp(original_mysql_path_ + i + 1, "#P#", 3) == 0) {
        break;
      }
      mysql_table_name_[j++] = original_mysql_path_[++i];
    }
    mysql_table_name_[j] = '\0';
    return mysql_table_name_;
  }

  /**
   * "./${db}/${table}"       ==> "./${db}/${table}"
   * "./${db}/${table}#P#xxx" ==> "./${db}/${table}"
   */
  const char *PathMapper::mysql_path() {
    if (mysql_path_[0] != '\0') {
      return mysql_path_;
    }

    int i;
    int len = strlen(original_mysql_path_);
    for (i = 0; i < len; i++) {
      if (len - i >= 3 &&
          strncmp(original_mysql_path_ + i, "#P#", 3) == 0) {
        break;
      }
      mysql_path_[i] = original_mysql_path_[i];
    }
    mysql_path_[i] = '\0';
    return mysql_path_;
  }

  bool PathMapper::is_internal_table_name() {
    return mysql_table_name()[0] == '#';
  }
}

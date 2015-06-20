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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <mrn_mysql.h>
#include <mrn_mysql_compat.h>
#include <mrn_constants.hpp>

#include "mrn_database_repairer.hpp"
#include "mrn_path_mapper.hpp"

// for debug
#define MRN_CLASS_NAME "mrn::DatabaseRepairer"

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#ifndef WIN32
#  include <dirent.h>
#endif

namespace mrn {
  DatabaseRepairer::DatabaseRepairer(grn_ctx *ctx, THD *thd)
    : ctx_(ctx),
      thd_(thd),
      base_directory_(NULL),
      base_directory_buffer_(),
      path_prefix_(NULL),
      path_prefix_buffer_(),
      path_prefix_length_(0),
      mrn_db_file_suffix_length_(strlen(MRN_DB_FILE_SUFFIX)) {
  }

  DatabaseRepairer::~DatabaseRepairer() {
  }

  bool DatabaseRepairer::is_crashed(void) {
    MRN_DBUG_ENTER_METHOD();

    bool is_crashed = false;
    each_database(&DatabaseRepairer::is_crashed_body, &is_crashed);

    DBUG_RETURN(is_crashed);
  }

  bool DatabaseRepairer::repair(void) {
    MRN_DBUG_ENTER_METHOD();

    bool succeeded = true;
    each_database(&DatabaseRepairer::repair_body, &succeeded);

    DBUG_RETURN(succeeded);
  }

  void DatabaseRepairer::each_database(EachBodyFunc each_body_func,
                                       void *user_data) {
    MRN_DBUG_ENTER_METHOD();

    detect_paths();

#ifdef WIN32
    WIN32_FIND_DATA data;
    HANDLE finder = FindFirstFile(base_directory_, &data);
    if (finder == INVALID_HANDLE_VALUE) {
      DBUG_VOID_RETURN;
    }

    do {
      each_database_body(data.cFileName, each_body_func, user_data);
    } while (FindNextFile(finder, &data) != 0);
    FindClose(finder);
#else
    DIR *dir = opendir(base_directory_);
    if (!dir) {
      DBUG_VOID_RETURN;
    }

    while (struct dirent *entry = readdir(dir)) {
      each_database_body(entry->d_name, each_body_func, user_data);
    }
    closedir(dir);
#endif

    DBUG_VOID_RETURN;
  }

  void DatabaseRepairer::each_database_body(const char *base_path,
                                            EachBodyFunc each_body_func,
                                            void *user_data) {
    MRN_DBUG_ENTER_METHOD();

    if (path_prefix_length_ > 0 &&
        strncmp(base_path, path_prefix_, path_prefix_length_) != 0) {
      DBUG_VOID_RETURN;
    }

    size_t path_length = strlen(base_path);
    if (path_length <= mrn_db_file_suffix_length_) {
      DBUG_VOID_RETURN;
    }

    if (strncmp(base_path + (path_length - mrn_db_file_suffix_length_),
                MRN_DB_FILE_SUFFIX, mrn_db_file_suffix_length_) != 0) {
      DBUG_VOID_RETURN;
    }

    char db_path[MRN_MAX_PATH_SIZE];
    snprintf(db_path, MRN_MAX_PATH_SIZE,
             "%s%c%s", base_directory_, FN_LIBCHAR, base_path);
    grn_obj *db = grn_db_open(ctx_, db_path);
    if (!db) {
      DBUG_VOID_RETURN;
    }

    (this->*each_body_func)(db, db_path, user_data);

    grn_obj_close(ctx_, db);

    DBUG_VOID_RETURN;
  }

  void DatabaseRepairer::detect_paths(void) {
    MRN_DBUG_ENTER_METHOD();

    const char *raw_path_prefix = mrn::PathMapper::default_path_prefix;

    if (!raw_path_prefix) {
      base_directory_ = ".";
      path_prefix_ = NULL;
      DBUG_VOID_RETURN;
    }

    strcpy(base_directory_buffer_, raw_path_prefix);
    size_t raw_path_prefix_length = strlen(raw_path_prefix);
    size_t separator_position = raw_path_prefix_length;
    for (; separator_position > 0; separator_position--) {
      if (base_directory_buffer_[separator_position] == FN_LIBCHAR ||
          base_directory_buffer_[separator_position] == FN_LIBCHAR2) {
        break;
      }
    }
    if (separator_position == 0 ||
        separator_position == raw_path_prefix_length) {
      base_directory_ = ".";
    } else {
      base_directory_buffer_[separator_position] = '\0';
      base_directory_ = base_directory_buffer_;
      strcpy(path_prefix_buffer_, raw_path_prefix + separator_position + 1);
      path_prefix_ = path_prefix_buffer_;
      path_prefix_length_ = strlen(path_prefix_);
    }

    DBUG_VOID_RETURN;
  }

  void DatabaseRepairer::is_crashed_body(grn_obj *db,
                                         const char *db_path,
                                         void *user_data) {
    MRN_DBUG_ENTER_METHOD();

    bool *is_crashed = static_cast<bool *>(user_data);

    if (grn_obj_is_locked(ctx_, db)) {
      *is_crashed = true;
      DBUG_VOID_RETURN;
    }

    grn_table_cursor *cursor;
    cursor = grn_table_cursor_open(ctx_, db,
                                   NULL, 0,
                                   NULL, 0,
                                   0, -1, GRN_CURSOR_BY_ID);
    if (!cursor) {
      *is_crashed = true;
      DBUG_VOID_RETURN;
    }

    grn_id id;
    while ((id = grn_table_cursor_next(ctx_, cursor)) != GRN_ID_NIL) {
      grn_obj *object = grn_ctx_at(ctx_, id);

      if (!object) {
        continue;
      }

      switch (object->header.type) {
      case GRN_TABLE_HASH_KEY :
      case GRN_TABLE_PAT_KEY:
      case GRN_TABLE_DAT_KEY:
      case GRN_TABLE_NO_KEY:
      case GRN_COLUMN_FIX_SIZE:
      case GRN_COLUMN_VAR_SIZE:
      case GRN_COLUMN_INDEX:
        grn_obj_is_locked(ctx_, object);
        *is_crashed = true;
        break;
      default:
        break;
      }

      grn_obj_unlink(ctx_, object);

      if (*is_crashed) {
        break;
      }
    }
    grn_table_cursor_close(ctx_, cursor);

    DBUG_VOID_RETURN;
  }

  void DatabaseRepairer::repair_body(grn_obj *db,
                                     const char *db_path,
                                     void *user_data) {
    MRN_DBUG_ENTER_METHOD();

    bool *succeeded = static_cast<bool *>(user_data);
    if (grn_db_recover(ctx_, db) != GRN_SUCCESS) {
      push_warning_printf(thd_,
                          Sql_condition::WARN_LEVEL_WARN,
                          ER_NOT_KEYFILE,
                          "mroonga: repair: "
                          "Failed to recover database: <%s>: <%s>",
                          db_path, ctx_->errbuf);
      *succeeded = false;
    }

    DBUG_VOID_RETURN;
  }
}

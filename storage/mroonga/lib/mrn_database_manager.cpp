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

#include <mrn_mysql.h>

#include "mrn_database_manager.hpp"
#include "mrn_encoding.hpp"
#include "mrn_lock.hpp"
#include "mrn_path_mapper.hpp"

#include <groonga/plugin.h>

// for debug
#define MRN_CLASS_NAME "mrn::DatabaseManager"

#ifdef WIN32
#  include <direct.h>
#  define MRN_MKDIR(pathname, mode) _mkdir((pathname))
#else
#  include <dirent.h>
#  include <unistd.h>
#  define MRN_MKDIR(pathname, mode) mkdir((pathname), (mode))
#endif

extern "C" {
  grn_rc GRN_PLUGIN_IMPL_NAME_TAGGED(init, normalizers_mysql)(grn_ctx *ctx);
  grn_rc GRN_PLUGIN_IMPL_NAME_TAGGED(register, normalizers_mysql)(grn_ctx *ctx);
}

namespace mrn {
  DatabaseManager::DatabaseManager(grn_ctx *ctx, mysql_mutex_t *mutex)
    : ctx_(ctx),
      cache_(NULL),
      mutex_(mutex) {
  }

  DatabaseManager::~DatabaseManager(void) {
    if (cache_) {
      void *db_address;
      GRN_HASH_EACH(ctx_, cache_, id, NULL, 0, &db_address, {
        Database *db;
        memcpy(&db, db_address, sizeof(grn_obj *));
        delete db;
      });
      grn_hash_close(ctx_, cache_);
    }
  }

  bool DatabaseManager::init(void) {
    MRN_DBUG_ENTER_METHOD();
    cache_ = grn_hash_create(ctx_,
                             NULL,
                             GRN_TABLE_MAX_KEY_SIZE,
                             sizeof(grn_obj *),
                             GRN_OBJ_KEY_VAR_SIZE);
    if (!cache_) {
      GRN_LOG(ctx_, GRN_LOG_ERROR,
              "failed to initialize hash table for caching opened databases");
      DBUG_RETURN(false);
    }

    DBUG_RETURN(true);
  }

  int DatabaseManager::open(const char *path, Database **db) {
    MRN_DBUG_ENTER_METHOD();

    int error = 0;
    *db = NULL;

    mrn::PathMapper mapper(path);
    mrn::Lock lock(mutex_);

    error = mrn::encoding::set(ctx_, system_charset_info);
    if (error) {
      DBUG_RETURN(error);
    }

    grn_id id;
    void *db_address;
    id = grn_hash_get(ctx_, cache_,
                      mapper.db_name(), strlen(mapper.db_name()),
                      &db_address);
    if (id == GRN_ID_NIL) {
      grn_obj *grn_db;
      struct stat db_stat;
      if (stat(mapper.db_path(), &db_stat)) {
        GRN_LOG(ctx_, GRN_LOG_INFO,
                "database not found. creating...: <%s>", mapper.db_path());
        if (path[0] == FN_CURLIB &&
            mrn_is_directory_separator(path[1])) {
          ensure_database_directory();
        }
        grn_db = grn_db_create(ctx_, mapper.db_path(), NULL);
        if (ctx_->rc) {
          error = ER_CANT_CREATE_TABLE;
          my_message(error, ctx_->errbuf, MYF(0));
          DBUG_RETURN(error);
        }
      } else {
        grn_db = grn_db_open(ctx_, mapper.db_path());
        if (ctx_->rc) {
          error = ER_CANT_OPEN_FILE;
          my_message(error, ctx_->errbuf, MYF(0));
          DBUG_RETURN(error);
        }
      }
      *db = new Database(ctx_, grn_db);
      grn_hash_add(ctx_, cache_,
                   mapper.db_name(), strlen(mapper.db_name()),
                   &db_address, NULL);
      memcpy(db_address, db, sizeof(Database *));
      error = ensure_normalizers_registered((*db)->get());
      if (!error) {
        if ((*db)->is_broken()) {
          error = ER_CANT_OPEN_FILE;
          char error_message[MRN_MESSAGE_BUFFER_SIZE];
          snprintf(error_message, MRN_MESSAGE_BUFFER_SIZE,
                   "mroonga: database: open: "
                   "The database maybe broken. "
                   "We recommend you to recreate the database. "
                   "If the database isn't broken, "
                   "you can remove this error by running "
                   "'groonga %s table_remove mroonga_operations' "
                   "on server. But the latter isn't recommended.",
                   mapper.db_path());
          my_message(error, error_message, MYF(0));
        }
      }
    } else {
      memcpy(db, db_address, sizeof(Database *));
      grn_ctx_use(ctx_, (*db)->get());
    }

    DBUG_RETURN(error);
  }

  void DatabaseManager::close(const char *path) {
    MRN_DBUG_ENTER_METHOD();

    mrn::PathMapper mapper(path);
    mrn::Lock lock(mutex_);

    grn_id id;
    void *db_address;
    id = grn_hash_get(ctx_, cache_,
                      mapper.db_name(), strlen(mapper.db_name()),
                      &db_address);
    if (id == GRN_ID_NIL) {
      DBUG_VOID_RETURN;
    }

    Database *db = NULL;
    memcpy(&db, db_address, sizeof(Database *));
    grn_ctx_use(ctx_, db->get());
    if (db) {
      delete db;
    }

    grn_hash_delete_by_id(ctx_, cache_, id, NULL);

    DBUG_VOID_RETURN;
  }

  bool DatabaseManager::drop(const char *path) {
    MRN_DBUG_ENTER_METHOD();

    mrn::PathMapper mapper(path);
    mrn::Lock lock(mutex_);

    grn_id id;
    void *db_address;
    id = grn_hash_get(ctx_, cache_,
                      mapper.db_name(), strlen(mapper.db_name()),
                      &db_address);

    Database *db = NULL;
    if (id == GRN_ID_NIL) {
      struct stat dummy;
      if (stat(mapper.db_path(), &dummy) == 0) {
        grn_obj *grn_db = grn_db_open(ctx_, mapper.db_path());
        db = new Database(ctx_, grn_db);
      }
    } else {
      memcpy(&db, db_address, sizeof(Database *));
      grn_ctx_use(ctx_, db->get());
    }

    if (!db) {
      DBUG_RETURN(false);
    }

    if (db->remove() == GRN_SUCCESS) {
      if (id != GRN_ID_NIL) {
        grn_hash_delete_by_id(ctx_, cache_, id, NULL);
      }
      delete db;
      DBUG_RETURN(true);
    } else {
      GRN_LOG(ctx_, GRN_LOG_ERROR,
              "failed to drop database: <%s>: <%s>",
              mapper.db_path(), ctx_->errbuf);
      if (id == GRN_ID_NIL) {
        delete db;
      }
      DBUG_RETURN(false);
    }
  }

  int DatabaseManager::clear(void) {
    MRN_DBUG_ENTER_METHOD();

    int error = 0;

    mrn::Lock lock(mutex_);

    grn_hash_cursor *cursor;
    cursor = grn_hash_cursor_open(ctx_, cache_,
                                  NULL, 0, NULL, 0,
                                  0, -1, 0);
    if (ctx_->rc) {
      my_message(ER_ERROR_ON_READ, ctx_->errbuf, MYF(0));
      DBUG_RETURN(ER_ERROR_ON_READ);
    }

    while (grn_hash_cursor_next(ctx_, cursor) != GRN_ID_NIL) {
      if (ctx_->rc) {
        error = ER_ERROR_ON_READ;
        my_message(error, ctx_->errbuf, MYF(0));
        break;
      }
      void *db_address;
      Database *db;
      grn_hash_cursor_get_value(ctx_, cursor, &db_address);
      memcpy(&db, db_address, sizeof(Database *));
      grn_ctx_use(ctx_, db->get());
      grn_rc rc = grn_hash_cursor_delete(ctx_, cursor, NULL);
      if (rc) {
        error = ER_ERROR_ON_READ;
        my_message(error, ctx_->errbuf, MYF(0));
        break;
      }
      delete db;
    }
    grn_hash_cursor_close(ctx_, cursor);

    DBUG_RETURN(error);
  }

  const char *DatabaseManager::error_message() {
    MRN_DBUG_ENTER_METHOD();
    DBUG_RETURN(ctx_->errbuf);
  }

  void DatabaseManager::mkdir_p(const char *directory) {
    MRN_DBUG_ENTER_METHOD();

    int i = 0;
    char sub_directory[MRN_MAX_PATH_SIZE];
    sub_directory[0] = '\0';
    while (true) {
      if (mrn_is_directory_separator(directory[i]) ||
          directory[i] == '\0') {
        sub_directory[i] = '\0';
        struct stat directory_status;
        if (stat(sub_directory, &directory_status) != 0) {
          DBUG_PRINT("info", ("mroonga: creating directory: <%s>", sub_directory));
          GRN_LOG(ctx_, GRN_LOG_INFO, "creating directory: <%s>", sub_directory);
          if (MRN_MKDIR(sub_directory, S_IRWXU) == 0) {
            DBUG_PRINT("info",
                       ("mroonga: created directory: <%s>", sub_directory));
            GRN_LOG(ctx_, GRN_LOG_INFO, "created directory: <%s>", sub_directory);
          } else {
            DBUG_PRINT("error",
                       ("mroonga: failed to create directory: <%s>: <%s>",
                        sub_directory, strerror(errno)));
            GRN_LOG(ctx_, GRN_LOG_ERROR,
                    "failed to create directory: <%s>: <%s>",
                    sub_directory, strerror(errno));
            DBUG_VOID_RETURN;
          }
        }
      }

      if (directory[i] == '\0') {
        break;
      }

      sub_directory[i] = directory[i];
      ++i;
    }

    DBUG_VOID_RETURN;
  }

  void DatabaseManager::ensure_database_directory(void) {
    MRN_DBUG_ENTER_METHOD();

    const char *path_prefix = mrn::PathMapper::default_path_prefix;
    if (!path_prefix)
      DBUG_VOID_RETURN;

    const char *last_path_separator;
    last_path_separator = strrchr(path_prefix, FN_LIBCHAR);
#ifdef FN_LIBCHAR2
    if (!last_path_separator)
      last_path_separator = strrchr(path_prefix, FN_LIBCHAR2);
#endif
    if (!last_path_separator)
      DBUG_VOID_RETURN;
    if (path_prefix == last_path_separator)
      DBUG_VOID_RETURN;

    char database_directory[MRN_MAX_PATH_SIZE];
    size_t database_directory_length = last_path_separator - path_prefix;
    strncpy(database_directory, path_prefix, database_directory_length);
    database_directory[database_directory_length] = '\0';
    mkdir_p(database_directory);

    DBUG_VOID_RETURN;
  }

  int DatabaseManager::ensure_normalizers_registered(grn_obj *db) {
    MRN_DBUG_ENTER_METHOD();

    int error = 0;
#ifdef WITH_GROONGA_NORMALIZER_MYSQL
    {
#  ifdef MRN_GROONGA_NORMALIZER_MYSQL_EMBEDDED
      GRN_PLUGIN_IMPL_NAME_TAGGED(init, normalizers_mysql)(ctx_);
      GRN_PLUGIN_IMPL_NAME_TAGGED(register, normalizers_mysql)(ctx_);
#  else
      grn_obj *mysql_normalizer;
      mysql_normalizer = grn_ctx_get(ctx_, "NormalizerMySQLGeneralCI", -1);
      if (mysql_normalizer) {
        grn_obj_unlink(ctx_, mysql_normalizer);
      } else {
        grn_plugin_register(ctx_, GROONGA_NORMALIZER_MYSQL_PLUGIN_NAME);
      }
#  endif
    }
#endif

    DBUG_RETURN(error);
  }
}

/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#ifndef _HATOKU_HTON_H
#define _HATOKU_HTON_H

#include "hatoku_defines.h"
#include "tokudb_debug.h"
#include "tokudb_information_schema.h"
#include "tokudb_memory.h"
#include "tokudb_thread.h"
#include "tokudb_time.h"
#include "tokudb_txn.h"
#include "tokudb_sysvars.h"

extern handlerton* tokudb_hton;

extern DB_ENV* db_env;

extern pfs_key_t ha_tokudb_mutex_key;
extern pfs_key_t num_DBs_lock_key;

inline tokudb::sysvars::row_format_t toku_compression_method_to_row_format(
    toku_compression_method method) {

    switch (method) {
    case TOKU_NO_COMPRESSION:
        return tokudb::sysvars::SRV_ROW_FORMAT_UNCOMPRESSED;
    case TOKU_ZLIB_WITHOUT_CHECKSUM_METHOD:
    case TOKU_ZLIB_METHOD:
        return tokudb::sysvars::SRV_ROW_FORMAT_ZLIB;
    case TOKU_SNAPPY_METHOD:
        return tokudb::sysvars::SRV_ROW_FORMAT_SNAPPY;
    case TOKU_QUICKLZ_METHOD:
        return tokudb::sysvars::SRV_ROW_FORMAT_QUICKLZ;
    case TOKU_LZMA_METHOD:
        return tokudb::sysvars::SRV_ROW_FORMAT_LZMA;
    case TOKU_DEFAULT_COMPRESSION_METHOD:
        return tokudb::sysvars::SRV_ROW_FORMAT_DEFAULT;
    case TOKU_FAST_COMPRESSION_METHOD:
        return tokudb::sysvars::SRV_ROW_FORMAT_FAST;
    case TOKU_SMALL_COMPRESSION_METHOD:
        return tokudb::sysvars::SRV_ROW_FORMAT_SMALL;
    default:
        assert_unreachable();
    }
}

inline toku_compression_method row_format_to_toku_compression_method(
    tokudb::sysvars::row_format_t row_format) {

    switch (row_format) {
    case tokudb::sysvars::SRV_ROW_FORMAT_UNCOMPRESSED:
        return TOKU_NO_COMPRESSION;
    case tokudb::sysvars::SRV_ROW_FORMAT_QUICKLZ:
    case tokudb::sysvars::SRV_ROW_FORMAT_FAST:
        return TOKU_QUICKLZ_METHOD;
    case tokudb::sysvars::SRV_ROW_FORMAT_SNAPPY:
        return TOKU_SNAPPY_METHOD;
    case tokudb::sysvars::SRV_ROW_FORMAT_ZLIB:
    case tokudb::sysvars::SRV_ROW_FORMAT_DEFAULT:
        return TOKU_ZLIB_WITHOUT_CHECKSUM_METHOD;
    case tokudb::sysvars::SRV_ROW_FORMAT_LZMA:
    case tokudb::sysvars::SRV_ROW_FORMAT_SMALL:
        return TOKU_LZMA_METHOD;
    default:
        assert_unreachable();
    }
}

inline enum row_type row_format_to_row_type(
    tokudb::sysvars::row_format_t row_format) {
#if defined(TOKU_INCLUDE_ROW_TYPE_COMPRESSION) && \
    TOKU_INCLUDE_ROW_TYPE_COMPRESSION
    switch (row_format) {
    case tokudb::sysvars::SRV_ROW_FORMAT_UNCOMPRESSED:
        return ROW_TYPE_TOKU_UNCOMPRESSED;
    case tokudb::sysvars::SRV_ROW_FORMAT_ZLIB:
        return ROW_TYPE_TOKU_ZLIB;
    case tokudb::sysvars::SRV_ROW_FORMAT_SNAPPY:
        return ROW_TYPE_TOKU_SNAPPY;
    case tokudb::sysvars::SRV_ROW_FORMAT_QUICKLZ:
        return ROW_TYPE_TOKU_QUICKLZ;
    case tokudb::sysvars::SRV_ROW_FORMAT_LZMA:
        return ROW_TYPE_TOKU_LZMA;
    case tokudb::sysvars::SRV_ROW_FORMAT_SMALL:
        return ROW_TYPE_TOKU_SMALL;
    case tokudb::sysvars::SRV_ROW_FORMAT_FAST:
        return ROW_TYPE_TOKU_FAST;
    case tokudb::sysvars::SRV_ROW_FORMAT_DEFAULT:
        return ROW_TYPE_DEFAULT;
    }
#endif  // defined(TOKU_INCLUDE_ROW_TYPE_COMPRESSION) &&
        // TOKU_INCLUDE_ROW_TYPE_COMPRESSION
    return ROW_TYPE_DEFAULT;
}

inline tokudb::sysvars::row_format_t row_type_to_row_format(
    enum row_type type) {
#if defined(TOKU_INCLUDE_ROW_TYPE_COMPRESSION) && \
    TOKU_INCLUDE_ROW_TYPE_COMPRESSION
    switch (type) {
    case ROW_TYPE_TOKU_UNCOMPRESSED:
        return tokudb::sysvars::SRV_ROW_FORMAT_UNCOMPRESSED;
    case ROW_TYPE_TOKU_ZLIB:
        return tokudb::sysvars::SRV_ROW_FORMAT_ZLIB;
    case ROW_TYPE_TOKU_SNAPPY:
        return tokudb::sysvars::SRV_ROW_FORMAT_SNAPPY;
    case ROW_TYPE_TOKU_QUICKLZ:
        return tokudb::sysvars::SRV_ROW_FORMAT_QUICKLZ;
    case ROW_TYPE_TOKU_LZMA:
        return tokudb::sysvars::SRV_ROW_FORMAT_LZMA;
    case ROW_TYPE_TOKU_SMALL:
        return tokudb::sysvars::SRV_ROW_FORMAT_SMALL;
    case ROW_TYPE_TOKU_FAST:
        return tokudb::sysvars::SRV_ROW_FORMAT_FAST;
    case ROW_TYPE_DEFAULT:
        return tokudb::sysvars::SRV_ROW_FORMAT_DEFAULT;
    default:
        return tokudb::sysvars::SRV_ROW_FORMAT_DEFAULT;
    }
#endif  // defined(TOKU_INCLUDE_ROW_TYPE_COMPRESSION) &&
        // TOKU_INCLUDE_ROW_TYPE_COMPRESSION
    return tokudb::sysvars::SRV_ROW_FORMAT_DEFAULT;
}

inline enum row_type toku_compression_method_to_row_type(
    toku_compression_method method) {

    return row_format_to_row_type(
        toku_compression_method_to_row_format(method));
}

inline toku_compression_method row_type_to_toku_compression_method(
    enum row_type type) {

    return row_format_to_toku_compression_method(row_type_to_row_format(type));
}

void tokudb_checkpoint_lock(THD * thd);
void tokudb_checkpoint_unlock(THD * thd);

inline uint64_t tokudb_get_lock_wait_time_callback(
    TOKUDB_UNUSED(uint64_t default_wait_time)) {
    THD *thd = current_thd;
    return tokudb::sysvars::lock_timeout(thd);
}

inline uint64_t tokudb_get_loader_memory_size_callback(void) {
    THD *thd = current_thd;
    return tokudb::sysvars::loader_memory_size(thd);
}

inline uint64_t tokudb_get_killed_time_callback(
    TOKUDB_UNUSED(uint64_t default_killed_time)) {
    THD *thd = current_thd;
    return tokudb::sysvars::killed_time(thd);
}

inline int tokudb_killed_callback(void) {
    THD *thd = current_thd;
    return thd_kill_level(thd);
}

inline bool tokudb_killed_thd_callback(void* extra,
                                       TOKUDB_UNUSED(uint64_t deleted_rows)) {
    THD *thd = static_cast<THD *>(extra);
    return thd_kill_level(thd) != 0;
}

extern const char* tokudb_hton_name;
extern int tokudb_hton_initialized;
extern tokudb::thread::rwlock_t tokudb_hton_initialized_lock;

void toku_hton_update_primary_key_bytes_inserted(uint64_t row_size);

void tokudb_split_dname(
    const char* dname,
    String& database_name,
    String& table_name,
    String& dictionary_name);

void tokudb_pretty_left_key(const DBT* key, String* out);
void tokudb_pretty_right_key(const DBT* key, String* out);
const char *tokudb_get_index_name(DB* db);

#endif //#ifdef _HATOKU_HTON

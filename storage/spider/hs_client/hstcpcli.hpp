
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#ifndef DENA_HSTCPCLI_HPP
#define DENA_HSTCPCLI_HPP

#define HANDLERSOCKET_MYSQL_UTIL 1

#include "mysql_version.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#endif

#include "config.hpp"
#include "socket.hpp"
#include "string_ref.hpp"
#include "string_buffer.hpp"

namespace dena {

struct hstcpcli_filter {
  string_ref filter_type;
  string_ref op;
  size_t ff_offset;
  string_ref val;
  hstcpcli_filter() : ff_offset(0) { }
};

struct hstcpcli_i;
typedef hstcpcli_i *hstcpcli_ptr;

struct hstresult {
  hstresult();
  virtual ~hstresult();
  string_buffer readbuf;
  size_t response_end_offset;
  size_t num_flds;
  size_t cur_row_offset;
  size_t cur_row_size;
  DYNAMIC_ARRAY flds;
};

struct hstcpcli_i {
  virtual ~hstcpcli_i() = default;
  virtual void close() = 0;
  virtual int reconnect() = 0;
  virtual bool stable_point() = 0;
  virtual void request_buf_auth(const char *secret, const char *typ) = 0;
  virtual void request_buf_open_index(size_t pst_id, const char *dbn,
    const char *tbl, const char *idx, const char *retflds,
    const char *filflds = 0) = 0;
  virtual void request_buf_exec_generic(size_t pst_id, const string_ref& op,
    const string_ref *kvs, size_t kvslen, uint32 limit, uint32 skip,
    const string_ref& mod_op, const string_ref *mvs, size_t mvslen,
    const hstcpcli_filter *fils = 0, size_t filslen = 0,
    int invalues_keypart = -1, const string_ref *invalues = 0,
    size_t invalueslen = 0) = 0; // FIXME: too long
  virtual size_t request_buf_append(const char *start, const char *finish) = 0;
  virtual void request_reset() = 0;
  virtual int request_send() = 0;
  virtual int response_recv(size_t& num_flds_r) = 0;
  virtual int get_result(hstresult& result) = 0;
  virtual const string_ref *get_next_row() = 0;
  virtual const string_ref *get_next_row_from_result(hstresult& result) = 0;
  virtual size_t get_row_size() = 0;
  virtual size_t get_row_size_from_result(hstresult& result) = 0;
  virtual void response_buf_remove() = 0;
  virtual int get_error_code() = 0;
  virtual String& get_error() = 0;
  virtual void clear_error() = 0;
  virtual int set_timeout(int send_timeout, int recv_timeout) = 0;
  virtual size_t get_num_req_bufd() = 0;
  virtual size_t get_num_req_sent() = 0;
  virtual size_t get_num_req_rcvd() = 0;
  virtual size_t get_response_end_offset() = 0;
  virtual const char *get_readbuf_begin() = 0;
  virtual const char *get_readbuf_end() = 0;
  virtual const char *get_writebuf_begin() = 0;
  virtual size_t get_writebuf_size() = 0;
  virtual void write_error_to_log(const char *func_name, const char *file_name,
    ulong line_no) = 0;
  static hstcpcli_ptr create(const socket_args& args);
};

};

#endif


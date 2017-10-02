
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#include <my_global.h>
#include "mysql_version.h"
#include "hs_compat.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#endif

#include "hstcpcli.hpp"
#include "auto_file.hpp"
#include "string_util.hpp"
#include "auto_addrinfo.hpp"
#include "escape.hpp"
#include "util.hpp"

/* TODO */
#if !defined(__linux__) && !defined(__FreeBSD__) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

#define DBG(x)

namespace dena {

hstresult::hstresult()
{
  SPD_INIT_DYNAMIC_ARRAY2(&flds, sizeof(string_ref), NULL, 16, 16,
    MYF(MY_WME));
}

hstresult::~hstresult()
{
  delete_dynamic(&flds);
}

struct hstcpcli : public hstcpcli_i, private noncopyable {
  hstcpcli(const socket_args& args);
  virtual ~hstcpcli();
  virtual void close();
  virtual int reconnect();
  virtual bool stable_point();
  virtual void request_buf_open_index(size_t pst_id, const char *dbn,
    const char *tbl, const char *idx, const char *retflds, const char *filflds);
  virtual void request_buf_auth(const char *secret, const char *typ);
  virtual void request_buf_exec_generic(size_t pst_id, const string_ref& op,
    const string_ref *kvs, size_t kvslen, uint32 limit, uint32 skip,
    const string_ref& mod_op, const string_ref *mvs, size_t mvslen,
    const hstcpcli_filter *fils, size_t filslen, int invalues_keypart,
    const string_ref *invalues, size_t invalueslen);
  virtual size_t request_buf_append(const char *start, const char *finish);
  virtual void request_reset();
  virtual int request_send();
  virtual int response_recv(size_t& num_flds_r);
  virtual int get_result(hstresult& result);
  virtual const string_ref *get_next_row();
  virtual const string_ref *get_next_row_from_result(hstresult& result);
  virtual void response_buf_remove();
  virtual int get_error_code();
  virtual String& get_error();
  virtual void clear_error();
  virtual int set_timeout(int send_timeout, int recv_timeout);
  virtual size_t get_num_req_bufd() { return num_req_bufd; }
  virtual size_t get_num_req_sent() { return num_req_sent; }
  virtual size_t get_num_req_rcvd() { return num_req_rcvd; }
  virtual size_t get_response_end_offset() { return response_end_offset; }
  virtual const char *get_readbuf_begin() { return readbuf.begin(); }
  virtual const char *get_readbuf_end() { return readbuf.end(); }
  virtual const char *get_writebuf_begin() { return writebuf.begin(); }
  virtual size_t get_writebuf_size() { return writebuf.size(); }
  virtual void write_error_to_log(const char *func_name, const char *file_name,
    ulong line_no);
 private:
  int read_more();
  int set_error(int code, const String& str);
  int set_error(int code, const char *str);
 private:
  auto_file fd;
  socket_args sargs;
  string_buffer readbuf;
  string_buffer writebuf;
  size_t response_end_offset; /* incl newline */
  size_t cur_row_offset;
  size_t num_flds;
  size_t num_req_bufd; /* buffered but not yet sent */
  size_t num_req_sent; /* sent but not yet received */
  size_t num_req_rcvd; /* received but not yet removed */
  int error_code;
  String error_str;
  DYNAMIC_ARRAY flds;
  int errno_buf;
};

hstcpcli::hstcpcli(const socket_args& args)
  : sargs(args), response_end_offset(0), cur_row_offset(0), num_flds(0),
    num_req_bufd(0), num_req_sent(0), num_req_rcvd(0), error_code(0), errno_buf(0)
{
  String err;
  SPD_INIT_DYNAMIC_ARRAY2(&flds, sizeof(string_ref), NULL, 16, 16, MYF(MY_WME));
  if (socket_connect(fd, sargs, err) != 0) {
    set_error(-1, err);
  }
}

hstcpcli::~hstcpcli()
{
  delete_dynamic(&flds);
}

void
hstcpcli::close()
{
  fd.close();
  readbuf.clear();
  writebuf.clear();
  response_end_offset = 0;
  cur_row_offset = 0;
  num_flds = 0;
  num_req_bufd = 0;
  num_req_sent = 0;
  num_req_rcvd = 0;
}

int
hstcpcli::reconnect()
{
  clear_error();
  close();
  String err;
  if (socket_connect(fd, sargs, err) != 0) {
    set_error(-1, err);
  }
  return error_code;
}

int
hstcpcli::set_timeout(int send_timeout, int recv_timeout)
{
  String err;
  sargs.send_timeout = send_timeout;
  sargs.recv_timeout = recv_timeout;
  if (socket_set_timeout(fd, sargs, err) != 0) {
    set_error(-1, err);
  }
  return error_code;
}

bool
hstcpcli::stable_point()
{
  /* returns true if cli can send a new request */
  return fd.get() >= 0 && num_req_bufd == 0 && num_req_sent == 0 &&
    num_req_rcvd == 0 && response_end_offset == 0;
}

int
hstcpcli::get_error_code()
{
  return error_code;
}

String&
hstcpcli::get_error()
{
  return error_str;
}

int
hstcpcli::read_more()
{
  const size_t block_size = 4096; // FIXME
  char *const wp = readbuf.make_space(block_size);
  int rlen;
  errno = 0;
  while ((rlen = read(fd.get(), wp, block_size)) <= 0) {
    errno_buf = errno;
    if (rlen < 0) {
      if (errno == EINTR || errno == EAGAIN)
      {
        errno = 0;
        continue;
      }
      error_str = String("read: failed", &my_charset_bin);
    } else {
      error_str = String("read: eof", &my_charset_bin);
    }
    return rlen;
  }
  readbuf.space_wrote(rlen);
  return rlen;
}

void
hstcpcli::clear_error()
{
  DBG(fprintf(stderr, "CLEAR_ERROR: %d\n", error_code));
  error_code = 0;
  error_str.length(0);
}

int
hstcpcli::set_error(int code, const String& str)
{
  DBG(fprintf(stderr, "SET_ERROR: %d\n", code));
  error_code = code;
  error_str = str;
  return error_code;
}

int
hstcpcli::set_error(int code, const char *str)
{
  uint32 str_len = strlen(str);
  DBG(fprintf(stderr, "SET_ERROR: %d\n", code));
  error_code = code;
  error_str.length(0);
  if (error_str.reserve(str_len + 1))
    return 0;
  error_str.q_append(str, str_len);
  error_str.c_ptr_safe();
  return error_code;
}

void
hstcpcli::request_buf_open_index(size_t pst_id, const char *dbn,
  const char *tbl, const char *idx, const char *retflds, const char *filflds)
{
/*
  if (num_req_sent > 0 || num_req_rcvd > 0) {
*/
  if (num_req_rcvd > 0) {
    close();
    set_error(-1, "request_buf_open_index: protocol out of sync");
    return;
  }
  const string_ref dbn_ref(dbn, strlen(dbn));
  const string_ref tbl_ref(tbl, strlen(tbl));
  const string_ref idx_ref(idx, strlen(idx));
  const string_ref rfs_ref(retflds, strlen(retflds));
  writebuf.append_literal("P\t");
  append_uint32(writebuf, pst_id); // FIXME size_t ?
  writebuf.append_literal("\t");
  writebuf.append(dbn_ref.begin(), dbn_ref.end());
  writebuf.append_literal("\t");
  writebuf.append(tbl_ref.begin(), tbl_ref.end());
  writebuf.append_literal("\t");
  writebuf.append(idx_ref.begin(), idx_ref.end());
  writebuf.append_literal("\t");
  writebuf.append(rfs_ref.begin(), rfs_ref.end());
  if (filflds != 0) {
    const string_ref fls_ref(filflds, strlen(filflds));
    writebuf.append_literal("\t");
    writebuf.append(fls_ref.begin(), fls_ref.end());
  }
  writebuf.append_literal("\n");
  ++num_req_bufd;
}

void
hstcpcli::request_buf_auth(const char *secret, const char *typ)
{
/*
  if (num_req_sent > 0 || num_req_rcvd > 0) {
*/
  if (num_req_rcvd > 0) {
    close();
    set_error(-1, "request_buf_auth: protocol out of sync");
    return;
  }
  if (typ == 0) {
    typ = "1";
  }
  const string_ref typ_ref(typ, strlen(typ));
  const string_ref secret_ref(secret, strlen(secret));
  writebuf.append_literal("A\t");
  writebuf.append(typ_ref.begin(), typ_ref.end());
  writebuf.append_literal("\t");
  writebuf.append(secret_ref.begin(), secret_ref.end());
  writebuf.append_literal("\n");
  ++num_req_bufd;
}

namespace {

void
append_delim_value(string_buffer& buf, const char *start, const char *finish)
{
  if (start == 0) {
    /* null */
    const char t[] = "\t\0";
    buf.append(t, t + 2);
  } else {
    /* non-null */
    buf.append_literal("\t");
    escape_string(buf, start, finish);
  }
}

};

void
hstcpcli::request_buf_exec_generic(size_t pst_id, const string_ref& op,
  const string_ref *kvs, size_t kvslen, uint32 limit, uint32 skip,
  const string_ref& mod_op, const string_ref *mvs, size_t mvslen,
  const hstcpcli_filter *fils, size_t filslen, int invalues_keypart,
  const string_ref *invalues, size_t invalueslen)
{
/*
  if (num_req_sent > 0 || num_req_rcvd > 0) {
*/
  if (num_req_rcvd > 0) {
    close();
    set_error(-1, "request_buf_exec_generic: protocol out of sync");
    return;
  }
  append_uint32(writebuf, pst_id); // FIXME size_t ?
  writebuf.append_literal("\t");
  writebuf.append(op.begin(), op.end());
  writebuf.append_literal("\t");
  append_uint32(writebuf, kvslen); // FIXME size_t ?
  for (size_t i = 0; i < kvslen; ++i) {
    const string_ref& kv = kvs[i];
    append_delim_value(writebuf, kv.begin(), kv.end());
  }
  if (limit != 0 || skip != 0 || invalues_keypart >= 0 ||
    mod_op.size() != 0 || filslen != 0) {
    /* has more option */
    writebuf.append_literal("\t");
    append_uint32(writebuf, limit); // FIXME size_t ?
    if (skip != 0 || invalues_keypart >= 0 ||
      mod_op.size() != 0 || filslen != 0) {
      writebuf.append_literal("\t");
      append_uint32(writebuf, skip); // FIXME size_t ?
    }
    if (invalues_keypart >= 0) {
      writebuf.append_literal("\t@\t");
      append_uint32(writebuf, invalues_keypart);
      writebuf.append_literal("\t");
      append_uint32(writebuf, invalueslen);
      for (size_t i = 0; i < invalueslen; ++i) {
        const string_ref& s = invalues[i];
        append_delim_value(writebuf, s.begin(), s.end());
      }
    }
    for (size_t i = 0; i < filslen; ++i) {
      const hstcpcli_filter& f = fils[i];
      writebuf.append_literal("\t");
      writebuf.append(f.filter_type.begin(), f.filter_type.end());
      writebuf.append_literal("\t");
      writebuf.append(f.op.begin(), f.op.end());
      writebuf.append_literal("\t");
      append_uint32(writebuf, f.ff_offset);
      append_delim_value(writebuf, f.val.begin(), f.val.end());
    }
    if (mod_op.size() != 0) {
      writebuf.append_literal("\t");
      writebuf.append(mod_op.begin(), mod_op.end());
      for (size_t i = 0; i < mvslen; ++i) {
        const string_ref& mv = mvs[i];
        append_delim_value(writebuf, mv.begin(), mv.end());
      }
    }
  }
  writebuf.append_literal("\n");
  ++num_req_bufd;
}

size_t
hstcpcli::request_buf_append(const char *start, const char *finish)
{
/*
  if (num_req_sent > 0 || num_req_rcvd > 0) {
*/
  if (num_req_rcvd > 0) {
    close();
    set_error(-1, "request_buf_append: protocol out of sync");
    return 0;
  }
  const char *nl = start;
  size_t num_req = 0;
  while ((nl = memchr_char(nl, '\n', finish - nl))) {
    if (nl == finish)
      break;
    num_req++;
    nl++;
  }
  num_req++;
  writebuf.append(start, finish);
  if (*(finish - 1) != '\n')
    writebuf.append_literal("\n");
  num_req_bufd += num_req;
  return num_req;
}

void
hstcpcli::request_reset()
{
  if (num_req_bufd) {
    writebuf.erase_front(writebuf.size());
    num_req_bufd = 0;
  }
}

int
hstcpcli::request_send()
{
  if (error_code < 0) {
    return error_code;
  }
  clear_error();
  if (fd.get() < 0) {
    close();
    return set_error(-1, "write: closed");
  }
/*
  if (num_req_bufd == 0 || num_req_sent > 0 || num_req_rcvd > 0) {
*/
  if (num_req_bufd == 0 || num_req_rcvd > 0) {
    close();
    return set_error(-1, "request_send: protocol out of sync");
  }
  const size_t wrlen = writebuf.size();
  const ssize_t r = send(fd.get(), writebuf.begin(), wrlen, MSG_NOSIGNAL);
  if (r <= 0) {
    close();
    return set_error(-1, r < 0 ? "write: failed" : "write: eof");
  }
  writebuf.erase_front(r);
  if (static_cast<size_t>(r) != wrlen) {
    close();
    return set_error(-1, "write: incomplete");
  }
  num_req_sent += num_req_bufd;
  num_req_bufd = 0;
  DBG(fprintf(stderr, "REQSEND 0\n"));
  return 0;
}

int
hstcpcli::response_recv(size_t& num_flds_r)
{
  if (error_code < 0) {
    return error_code;
  }
  clear_error();
  if (num_req_bufd > 0 || num_req_sent == 0 || num_req_rcvd > 0 ||
    response_end_offset != 0) {
    close();
    return set_error(-1, "response_recv: protocol out of sync");
  }
  cur_row_offset = 0;
  num_flds_r = num_flds = 0;
  if (fd.get() < 0) {
    return set_error(-1, "read: closed");
  }
  size_t offset = 0;
  while (true) {
    const char *const lbegin = readbuf.begin() + offset;
    const char *const lend = readbuf.end();
    if (lbegin < lend)
    {
      const char *const nl = memchr_char(lbegin, '\n', lend - lbegin);
      if (nl != 0) {
        offset += (nl + 1) - lbegin;
        break;
      }
      offset += lend - lbegin;
    }
    if (read_more() <= 0) {
      close();
      error_code = -1;
      return error_code;
    }
  }
  response_end_offset = offset;
  --num_req_sent;
  ++num_req_rcvd;
  char *start = readbuf.begin();
  char *const finish = start + response_end_offset - 1;
  const size_t resp_code = read_ui32(start, finish);
  skip_one(start, finish);
  num_flds_r = num_flds = read_ui32(start, finish);
  if (resp_code != 0) {
    skip_one(start, finish);
    char *const err_begin = start;
    read_token(start, finish);
    char *const err_end = start;
    String e = String(err_begin, (uint32)(err_end - err_begin), &my_charset_bin);
    if (!e.length()) {
      e = String("unknown_error", &my_charset_bin);
    }
    return set_error(resp_code, e);
  }
  cur_row_offset = start - readbuf.begin();
  DBG(fprintf(stderr, "[%s] ro=%zu eol=%zu\n",
    String(readbuf.begin(), readbuf.begin() + response_end_offset)
      .c_str(),
    cur_row_offset, response_end_offset));
  DBG(fprintf(stderr, "RES 0\n"));
  if (flds.max_element < num_flds)
  {
    if (allocate_dynamic(&flds, num_flds))
      return set_error(-1, "out of memory");
  }
  flds.elements = num_flds;
  return 0;
}

int
hstcpcli::get_result(hstresult& result)
{
/*
  readbuf.swap(result.readbuf);
*/
  char *const wp = result.readbuf.make_space(response_end_offset);
  memcpy(wp, readbuf.begin(), response_end_offset);
  result.readbuf.space_wrote(response_end_offset);
  result.response_end_offset = response_end_offset;
  result.num_flds = num_flds;
  result.cur_row_offset = cur_row_offset;
  if (result.flds.max_element < num_flds)
  {
    if (allocate_dynamic(&result.flds, num_flds))
      return set_error(-1, "out of memory");
  }
  result.flds.elements = num_flds;
  return 0;
}

const string_ref *
hstcpcli::get_next_row()
{
  if (num_flds == 0 || flds.elements < num_flds) {
    DBG(fprintf(stderr, "GNR NF 0\n"));
    return 0;
  }
  char *start = readbuf.begin() + cur_row_offset;
  char *const finish = readbuf.begin() + response_end_offset - 1;
  if (start >= finish) { /* start[0] == nl */
    DBG(fprintf(stderr, "GNR FIN 0 %p %p\n", start, finish));
    return 0;
  }
  for (size_t i = 0; i < num_flds; ++i) {
    skip_one(start, finish);
    char *const fld_begin = start;
    read_token(start, finish);
    char *const fld_end = start;
    char *wp = fld_begin;
    if (is_null_expression(fld_begin, fld_end)) {
      /* null */
      ((string_ref *) flds.buffer)[i] = string_ref();
    } else {
      unescape_string(wp, fld_begin, fld_end); /* in-place */
      ((string_ref *) flds.buffer)[i] = string_ref(fld_begin, wp);
    }
  }
  cur_row_offset = start - readbuf.begin();
  return (string_ref *) flds.buffer;
}

const string_ref *
hstcpcli::get_next_row_from_result(hstresult& result)
{
  if (result.num_flds == 0 || result.flds.elements < result.num_flds) {
    DBG(fprintf(stderr, "GNR NF 0\n"));
    return 0;
  }
  char *start = result.readbuf.begin() + result.cur_row_offset;
  char *const finish = result.readbuf.begin() + result.response_end_offset - 1;
  if (start >= finish) { /* start[0] == nl */
    DBG(fprintf(stderr, "GNR FIN 0 %p %p\n", start, finish));
    return 0;
  }
  for (size_t i = 0; i < result.num_flds; ++i) {
    skip_one(start, finish);
    char *const fld_begin = start;
    read_token(start, finish);
    char *const fld_end = start;
    char *wp = fld_begin;
    if (is_null_expression(fld_begin, fld_end)) {
      /* null */
      ((string_ref *) result.flds.buffer)[i] = string_ref();
    } else {
      unescape_string(wp, fld_begin, fld_end); /* in-place */
      ((string_ref *) result.flds.buffer)[i] = string_ref(fld_begin, wp);
    }
  }
  result.cur_row_offset = start - result.readbuf.begin();
  return (string_ref *) result.flds.buffer;
}

void
hstcpcli::response_buf_remove()
{
  if (response_end_offset == 0) {
    close();
    set_error(-1, "response_buf_remove: protocol out of sync");
    return;
  }
  readbuf.erase_front(response_end_offset);
  response_end_offset = 0;
  --num_req_rcvd;
  cur_row_offset = 0;
  num_flds = 0;
}

void
hstcpcli::write_error_to_log(
  const char *func_name,
  const char *file_name,
  ulong line_no
) {
  if (errno_buf) {
    time_t cur_time = (time_t) time((time_t*) 0);
    struct tm lt;
    struct tm *l_time = localtime_r(&cur_time, &lt);
    fprintf(stderr,
      "%04d%02d%02d %02d:%02d:%02d [ERROR] hstcpcli: [%d][%s]"
      " [%s][%s][%lu] errno=%d\n",
      l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
      l_time->tm_hour, l_time->tm_min, l_time->tm_sec,
      error_code, error_str.c_ptr_safe(),
      func_name, file_name, line_no, errno_buf);
  }
}

hstcpcli_ptr
hstcpcli_i::create(const socket_args& args)
{
  return hstcpcli_ptr(new hstcpcli(args));
}

};


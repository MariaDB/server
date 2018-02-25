
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011-2017 Kentoku SHIBA
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
#endif

#include "string_util.hpp"

namespace dena {

string_wref
get_token(char *& wp, char *wp_end, char delim)
{
  char *const wp_begin = wp;
  char *const p = memchr_char(wp_begin, delim, wp_end - wp_begin);
  if (p == 0) {
    wp = wp_end;
    return string_wref(wp_begin, wp_end - wp_begin);
  }
  wp = p + 1;
  return string_wref(wp_begin, p - wp_begin);
}

uint32
atoi_uint32_nocheck(const char *start, const char *finish)
{
  uint32 v = 0;
  for (; start != finish; ++start) {
    const char c = *start;
    if (c < '0' || c > '9') {
      break;
    }
    v *= 10;
    v += (uint32) (c - '0');
  }
  return v;
}

long long
atoll_nocheck(const char *start, const char *finish)
{
  long long v = 0;
  bool negative = false;
  if (start != finish) {
    if (start[0] == '-') {
      ++start;
      negative = true;
    } else if (start[0] == '+') {
      ++start;
    }
  }
  for (; start != finish; ++start) {
    const char c = *start;
    if (c < '0' || c > '9') {
      break;
    }
    v *= 10;
    if (negative) {
      v -= (long long) (c - '0');
    } else {
      v += (long long) (c - '0');
    }
  }
  return v;
}

void
append_uint32(string_buffer& buf, uint32 v)
{
  char *const wp = buf.make_space(64);
  const int len = snprintf(wp, 64, "%lu", static_cast<unsigned long>(v));
  if (len > 0) {
    buf.space_wrote(len);
  }
}

/*
String *
to_stdstring(uint32 v)
{
  char buf[64];
  int str_len;
  String *str;
  str_len = snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(v));
  if ((str = new String(str_len + 1)))
    str->q_append(buf, str_len);
  return str;
}
*/

int
errno_string(const char *s, int en, String& err_r)
{
  char buf[64];
  int str_len;
  str_len = snprintf(buf, sizeof(buf), "%s: %d", s, en);
  if (!err_r.reserve(str_len + 1))
    err_r.q_append(buf, str_len);
  return en;
}

size_t
split(char delim, const string_ref& buf, string_ref *parts,
  size_t parts_len)
{
  size_t i = 0;
  const char *start = buf.begin();
  const char *const finish = buf.end();
  for (i = 0; i < parts_len; ++i) {
    const char *const p = memchr_char(start, delim, finish - start);
    if (p == 0) {
      parts[i] = string_ref(start, finish - start);
      ++i;
      break;
    }
    parts[i] = string_ref(start, p - start);
    start = p + 1;
  }
  const size_t r = i;
  for (; i < parts_len; ++i) {
    parts[i] = string_ref();
  }
  return r;
}

size_t
split(char delim, const string_wref& buf, string_wref *parts,
  size_t parts_len)
{
  size_t i = 0;
  char *start = buf.begin();
  char *const finish = buf.end();
  for (i = 0; i < parts_len; ++i) {
    char *const p = memchr_char(start, delim, finish - start);
    if (p == 0) {
      parts[i] = string_wref(start, finish - start);
      ++i;
      break;
    }
    parts[i] = string_wref(start, p - start);
    start = p + 1;
  }
  const size_t r = i;
  for (; i < parts_len; ++i) {
    parts[i] = string_wref();
  }
  return r;
}

size_t
split(char delim, const string_ref& buf, DYNAMIC_ARRAY& parts_r)
{
  size_t i = 0;
  const char *start = buf.begin();
  const char *finish = buf.end();
  while (true) {
    const char *p = memchr_char(start, delim, finish - start);
    if (p == 0) {
      string_ref param(start, finish - start);
      insert_dynamic(&parts_r, (uchar *) &param);
      break;
    }
    string_ref param(start, p - start);
    insert_dynamic(&parts_r, (uchar *) &param);
    start = p + 1;
  }
  const size_t r = i;
  return r;
}

size_t
split(char delim, const string_wref& buf, DYNAMIC_ARRAY& parts_r)
{
  size_t i = 0;
  char *start = buf.begin();
  char *finish = buf.end();
  while (true) {
    char *p = memchr_char(start, delim, finish - start);
    if (p == 0) {
      string_wref param(start, finish - start);
      insert_dynamic(&parts_r, (uchar *) &param);
      break;
    }
    string_wref param(start, p - start);
    insert_dynamic(&parts_r, (uchar *) &param);
    start = p + 1;
  }
  const size_t r = i;
  return r;
}

};


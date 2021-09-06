
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#ifndef DENA_CONFIG_HPP
#define DENA_CONFIG_HPP

#include "mysql_version.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#endif

#define DENA_VERBOSE(lv, x) if (dena::verbose_level >= (lv)) { (x); }

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
#define INFO_KIND_HS_RET_FIELDS 1
#define INFO_KIND_HS_APPEND_STRING_REF 3
#define INFO_KIND_HS_CLEAR_STRING_REF 4
#define INFO_KIND_HS_INCREMENT_BEGIN 5
#define INFO_KIND_HS_INCREMENT_END 6
#define INFO_KIND_HS_DECREMENT_BEGIN 7
#define INFO_KIND_HS_DECREMENT_END 8
#endif

namespace dena {

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
struct uint32_info {
  size_t info_size;
  uint32 *info;
};
#endif

struct conf_param {
  String key;
  String val;
};

uchar *conf_get_key(
  conf_param *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
);

struct config {
  bool init;
  HASH conf_hash;
  config();
  ~config();
  conf_param *find(const String& key) const;
  conf_param *find(const char *key) const;
  String get_str(const String& key, const String& def =
    String("", 0, &my_charset_bin)) const;
  String get_str(const char *key, const char *def = "") const;
  long long get_int(const String& key, long long def = 0) const;
  long long get_int(const char *key, long long def = 0) const;
  bool replace(const char *key, const char *val);
  bool replace(const char *key, long long val);
  bool compare(const char *key, const char *val);
  void list_all_params() const;
  config& operator =(const config& x);
};

void parse_args(int argc, char **argv, config& conf);

extern unsigned int verbose_level;

};

#endif


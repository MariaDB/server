
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#include <my_global.h>
#include "mysql_version.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#endif

#include "config.hpp"

namespace dena {

unsigned int verbose_level = 0;

uchar *
conf_get_key(
  conf_param *param,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  *length = param->key.length();
  return (uchar*) param->key.ptr();
}

config::config()
{
  if (my_hash_init(&conf_hash, &my_charset_bin, 32, 0, 0,
    (my_hash_get_key) conf_get_key, 0, 0))
    init = FALSE;
  else
    init = TRUE;
  return;
}

config::~config()
{
  if (init)
  {
    conf_param *param;
    while ((param = (conf_param *) my_hash_element(&conf_hash, 0)))
    {
      my_hash_delete(&conf_hash, (uchar*) param);
      delete param;
    }
    my_hash_free(&conf_hash);
  }
}

conf_param *
config::find(const String& key) const
{
  if (init)
    return (conf_param *) my_hash_search(&conf_hash, (const uchar*) key.ptr(),
      key.length());
  else
    return NULL;
}

conf_param *
config::find(const char *key) const
{
  if (init)
    return (conf_param *) my_hash_search(&conf_hash, (const uchar*) key,
      strlen(key));
  else
    return NULL;
}

String
config::get_str(const String& key, const String& def) const
{
  DENA_VERBOSE(30, list_all_params());
  conf_param *param = find(key);
  if (!param) {
    DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%s(default)\n", key.ptr(),
      def.ptr()));
    return def;
  }
  DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%s\n", key.ptr(),
    param->val.ptr()));
  return param->val;
}

String
config::get_str(const char *key, const char *def) const
{
  DENA_VERBOSE(30, list_all_params());
  conf_param *param = find(key);
  if (!param) {
    DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%s(default)\n", key, def));
    return String(def, strlen(def), &my_charset_bin);
  }
  DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%s\n",
    key, param->val.ptr()));
  return param->val;
}

long long
config::get_int(const String& key, long long def) const
{
  int err;
  DENA_VERBOSE(30, list_all_params());
  conf_param *param = find(key);
  if (!param) {
    DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%lld(default)\n", key.ptr(),
      def));
    return def;
  }
  const long long r = my_strtoll10(param->val.ptr(), (char**) NULL, &err);
  if (err) {
    DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%lld(err)\n", key.ptr(),
      def));
    return def;
  }
  DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%lld\n", key.ptr(), r));
  return r;
}

long long
config::get_int(const char *key, long long def) const
{
  int err;
  DENA_VERBOSE(30, list_all_params());
  conf_param *param = find(key);
  if (!param) {
    DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%lld(default)\n", key, def));
    return def;
  }
  const long long r = my_strtoll10(param->val.ptr(), (char**) NULL, &err);
  if (err) {
    DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%lld(err)\n", key, def));
    return def;
  }
  DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%lld\n", key, r));
  return r;
}

bool
config::replace(const char *key, const char *val)
{
  uint32 val_len = strlen(val);
  conf_param *param = find(key);
  if (!param) {
    /* create */
    if (!(param = new conf_param()))
      return TRUE;
    uint32 key_len = strlen(key);
    if (
      param->key.reserve(key_len + 1) ||
      param->val.reserve(val_len + 1)
    ) {
      delete param;
      return TRUE;
    }
    param->key.q_append(key, key_len);
    param->val.q_append(val, val_len);
    param->key.c_ptr_safe();
    param->val.c_ptr_safe();
    if (my_hash_insert(&conf_hash, (uchar*) param))
    {
      delete param;
      return TRUE;
    }
    DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%s(create)\n",
      param->key.ptr(), param->val.ptr()));
    return FALSE;
  }
  /* replace */
  param->val.length(0);
  if (param->val.reserve(val_len + 1))
    return TRUE;
  param->val.q_append(val, val_len);
  param->val.c_ptr_safe();
  DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%s(replace)\n",
    param->key.ptr(), param->val.ptr()));
  return FALSE;
}

bool
config::replace(const char *key, long long val)
{
  char val_str[22];
  sprintf(val_str, "%lld", val);
  return replace(key, val_str);
}

bool
config::compare(const char *key, const char *val)
{
  conf_param *param = find(key);
  if (!param)
    return FALSE;
  return !strcmp(param->val.ptr(), val);
}

void
config::list_all_params() const
{
  conf_param *param;
  DENA_VERBOSE(10, fprintf(stderr, "list_all_params start\n"));
  for(ulong i = 0; i < conf_hash.records; i++)
  {
    if ((param = (conf_param *) my_hash_element((HASH *) &conf_hash, i)))
    {
      DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%s\n",
        param->key.ptr(), param->val.ptr()));
    }
  }
  DENA_VERBOSE(10, fprintf(stderr, "list_all_params end\n"));
}

config&
config::operator =(const config& x)
{
  DENA_VERBOSE(10, fprintf(stderr, "config operator = start"));
  if (this != &x && init && x.init) {
    conf_param *param, *new_param;
    for(ulong i = 0; i < x.conf_hash.records; i++)
    {
      if (
        (param = (conf_param *) my_hash_element((HASH *) &x.conf_hash, i)) &&
        (new_param = new conf_param())
      ) {
        if (
          !new_param->key.copy(param->key) &&
          !new_param->val.copy(param->val)
        ) {
          new_param->key.c_ptr_safe();
          new_param->val.c_ptr_safe();
          DENA_VERBOSE(10, fprintf(stderr, "CONFIG: %s=%s\n",
            new_param->key.ptr(), new_param->val.ptr()));
          if (my_hash_insert(&conf_hash, (uchar*) new_param))
            delete new_param;
        } else
          delete new_param;
      }
    }
  }
  DENA_VERBOSE(10, fprintf(stderr, "config operator = end %p", this));
  return *this;
}

void
parse_args(int argc, char **argv, config& conf)
{
  conf_param *param;
  for (int i = 1; i < argc; ++i) {
    const char *const arg = argv[i];
    const char *const eq = strchr(arg, '=');
    if (eq == 0) {
      continue;
    }
    if (!(param = new conf_param()))
      continue;
    uint32 key_len = (uint32)(eq - arg);
    uint32 val_len = (uint32)(strlen(eq + 1));
    if (
      param->key.reserve(key_len + 1) ||
      param->val.reserve(val_len + 1)
    ) {
      delete param;
      continue;
    }
    param->key.q_append(arg, key_len);
    param->val.q_append(eq + 1, val_len);
    param->key.c_ptr_safe();
    param->val.c_ptr_safe();
    if (my_hash_insert(&conf.conf_hash, (uchar*) param))
    {
      delete param;
      continue;
    }
  }
  param = conf.find("verbose");
  if (param) {
    verbose_level = atoi(param->val.c_ptr());
  }
}

};


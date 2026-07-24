/*
  Copyright (c) 2025 MariaDB

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA.
*/

#ifndef RPL_MASTER_INFO_FILE_H
#define RPL_MASTER_INFO_FILE_H

#include "rpl_info_file.h"
#include "sql_const.h"   // MAX_PASSWORD_LENGTH
#include "my_decimal.h"  // decimal_t for Heartbeat_period_value::from_decimal
#include "typelib.h"     // TYPELIB for value_map_typelib


/*
  A three-way comparison function for using sort_dynamic() and bsearch() on
  ID_array_value::array.
*/
inline static int change_master_id_cmp(const void *arg1, const void *arg2)
{
  const ulong *id1= (const ulong *) arg1;
  const ulong *id2= (const ulong *) arg2;
  return (*id1 > *id2) - (*id1 < *id2);
}


/*
  Three-state enum used by the parser to represent "true / false / use
  the default" for boolean CHANGE MASTER options.
*/
enum struct trilean { NO, YES, DEFAULT= -1 };

/* Enum for Master_info_file::master_use_gtid. */
enum struct enum_master_use_gtid { NO, CURRENT_POS, SLAVE_POS, DEFAULT };

/* CLI/typelib names for non-DEFAULT values. Defined in sys_vars.cc. */
extern const char *master_use_gtid_names[];

/*
  Defaults for the master_* options. Defined as Sys_var_* in sql/sys_vars.cc.
*/
extern uint slave_net_timeout;
extern uint master_connect_retry;
/*
  In seconds. When user did not set this on the command line / config file,
  Heartbeat_period_value reads slave_net_timeout / 2 dynamically instead
  (see master_heartbeat_period_is_auto).
*/
extern double master_heartbeat_period;
/* Snapshot of IS_SYSVAR_AUTOSIZE(&master_heartbeat_period) set at startup. */
extern my_bool master_heartbeat_period_is_auto;
extern my_bool master_ssl;
extern char *master_ssl_ca;
extern char *master_ssl_capath;
extern char *master_ssl_cert;
extern char *master_ssl_crl;
extern char *master_ssl_crlpath;
extern char *master_ssl_key;
extern char *master_ssl_cipher;
extern my_bool master_ssl_verify_server_cert;
extern ulong master_use_gtid;
extern ulonglong master_retry_count;


struct Master_info_file: Info_file
{
  /* Unsigned integer value with DEFAULT. */
  struct Uint_value_with_default: Value
  {
    uint value;
    uint *default_value;
    Uint_value_with_default(const char *name, uint *default_value)
      :Value(name),
      default_value(default_value) {}
    operator uint() { return value; }
    void operator=(uint new_value) { value= new_value; has_value= true; }
    bool set_default() override
    { value= *default_value; has_value= false; return false; }
    bool load_from(IO_CACHE *file) override;
    void save_to(IO_CACHE *file) override;
  };


  /* Unsigned 64-bit integer value with DEFAULT. */
  struct Ulonglong_value_with_default: Value
  {
    ulonglong value;
    ulonglong *default_value;
    Ulonglong_value_with_default(const char *name, ulonglong *default_value)
      : Value(name),
        default_value(default_value) {}
    operator ulonglong() { return value; }
    void operator=(ulonglong new_value) { value= new_value; has_value= true; }
    bool set_default() override
    { value= *default_value; has_value= false; return false; }
    bool load_from(IO_CACHE *file) override;
    void save_to(IO_CACHE *file) override;
  };


  /*
    Path value with DEFAULT (for SSL): FN_REFLEN-sized null-terminated
    string with a mariadbd option for the DEFAULT. set_default() copies
    the default into buf; has_value tracks whether buf is user-set or
    default.
  */
  struct Path_value: Value
  {
    char **default_value;
    char buf[FN_REFLEN];
    Path_value(const char *name, char **default_value):
      Value(name), default_value(default_value)
    { set_default(); }
    operator const char *() { return buf; }
    /* @param other '\0'-terminated string, or nullptr for set_default(). */
    void operator=(const char *other);
    bool set_default() override;
    bool load_from(IO_CACHE *file) override;
    void save_to(IO_CACHE *file) override;
  };


  /* Boolean value with DEFAULT. */
  struct Bool_value_with_default: Value
  {
    my_bool value;
    my_bool *default_value;
    Bool_value_with_default(const char *name, my_bool *default_value)
      : Value(name),
        default_value(default_value) {}
    operator bool() { return value; }
    void operator=(bool new_value)
    { value= (my_bool) new_value; has_value= true; }
    bool set_default() override
    { value= *default_value; has_value= false; return false; }
    bool load_from(IO_CACHE *file) override;
    void save_to(IO_CACHE *file) override;
  };


  /*
    Server-id / domain-id array value. The underlying DYNAMIC_ARRAY
    elements are ulong today (FIXME: should be uint32). Only one of
    DO_DOMAIN_IDS and IGNORE_DOMAIN_IDS can be active at a time, so
    separate arrays are wasteful. Until we refactor this pair, this
    struct only references existing arrays and does not manage their
    lifetimes.
  */
  struct ID_array_value: Value
  {
    DYNAMIC_ARRAY *array;
    /*
      The array is always considered "present" (possibly empty); it never
      defaults to absent. This keeps save_to_file() writing =value for
      this key.
    */
    ID_array_value(const char *name) :Value(name) {}
    ID_array_value(const char *name, DYNAMIC_ARRAY *array)
      :Value(name),
      array(array) { has_value= true; }
    /*
      No DEFAULT state distinct from "empty array". Returns true (matches
      base — invalid to write the key without =value), but does NOT touch
      has_value so the ctor's has_value=true survives Master_info_file's
      set_default loop.
    */
    bool set_default() override { return true; }
    bool load_from(IO_CACHE *file) override;
    void save_to(IO_CACHE *file) override;
  };


  /*
    Heartbeat period value: a non-negative decimal seconds value stored
    internally as unsigned integer milliseconds. The DEFAULT is dynamic,
    (see get_value()) : slave_net_timeout / 2 when the user did not
    set master_heartbeat_period, otherwise ::master_heartbeat_period
    (seconds, multiplied by 1000 here). Parsed from the file with
    my_strtod(); the maximum on-disk value is UINT_MAX32 milliseconds
    (~4294967.295 seconds).
  */
  struct Heartbeat_period_value: Value
  {
    Heartbeat_period_value() : Value("heartbeat_period") {}
    /* Max valid value: UINT_MAX32 milliseconds, as a string. */
    static constexpr char MAX[]= "4294967.295";

    uint32 value;       // milliseconds
    uint32 get_value()
    {
      if (has_value)
        return value;
      /*
        Default: dynamic. When user didn't set master_heartbeat_period,
        follow slave_net_timeout/2 (which may change at runtime via
        SET @@global.slave_net_timeout).
      */
      if (::master_heartbeat_period_is_auto)
        return (uint32) MY_MIN((ulonglong) slave_net_timeout * 500,
                                 UINT_MAX32);
      return (uint32) (::master_heartbeat_period * 1000);
    }
    void operator=(uint32 new_value)
    { value= new_value; has_value= true; }
    bool set_default() override
    {
      /* Default is computed dynamically in get_value() */
      has_value= false;
      return false;
    }
    /*
      Parse a DECIMAL into milliseconds (used by the SQL parser to handle
      CHANGE MASTER TO MASTER_HEARTBEAT_PERIOD=X). Sets *overprecise to
      true if the decimal has more than 3 decimal digits. Returns true
      if the decimal is out of range.
    */
    static uint from_decimal(uint32 *result, const decimal_t *decimal,
                             bool *overprecise);
    bool load_from(IO_CACHE *file) override;
    void save_to(IO_CACHE *file) override;
  };


  /*
    Singleton class for Master_info_file::master_use_gtid: an
    enum_master_use_gtid value whose DEFAULT is resolved from
    ::master_use_gtid, which in turn falls back to SLAVE_POS or NO
    depending on the per-connection.
    The intial value is set according the default.
    On connection to master, the state is set to NO if the master
    does not support GTID
    The current state is stored in Master_use_gtid_value::active_mode
  */
  struct Master_use_gtid_value: Value
  {
    Master_use_gtid_value() : Value("using_gtid") {}
    enum_master_use_gtid active_mode, user_mode;
    enum_master_use_gtid get_default_value()
    {
      return (enum_master_use_gtid) ::master_use_gtid;
    }
    void set_user_value(enum_master_use_gtid new_mode)
    {
      /*
        DEFAULT is a sentinel meaning "use the global / route through
        set_default() so has_value is set to false and get_value()
        resolves it dynamically.
      */
      if (new_mode == enum_master_use_gtid::DEFAULT)
        set_default();
      else
      {
        active_mode= user_mode= new_mode;
        has_value= true;
      }
    }
    bool set_default() override
    {
      active_mode= user_mode= get_default_value();
      has_value= false;
      return false;
    }
    void reset_to_user_value()
    {
      active_mode= user_mode;
    }
    void disable_gtid()
    {
      active_mode= enum_master_use_gtid::NO;
    }
    bool load_from(IO_CACHE *file) override;
    void save_to(IO_CACHE *file) override;
  };


  /*
    Buffers for String_value members. Declared before the String_value
    members so they are valid when passed to those members' constructors
    (C++ initializes data members in declaration order).
  */
  char master_log_name[FN_REFLEN+6]; /* Room for multi- */
  char host[HOSTNAME_LENGTH*SYSTEM_CHARSET_MBMAXLEN+1];
  char user[USERNAME_LENGTH+1];
  char password[MAX_PASSWORD_LENGTH*SYSTEM_CHARSET_MBMAXLEN+1];

  /*
    Values stored in master.info, in SHOW SLAVE STATUS order where
    applicable.
  */
  String_value master_host{"master_host", host, sizeof(host)};
  String_value master_user{"master_user", user, sizeof(user)};
  String_value master_password{"master_password", password, sizeof(password)};

  Uint_value master_port{"master_port"};
  Uint_value_with_default master_connect_retry{"connect_retry",
    &::master_connect_retry};
  String_value master_log_file{"master_log_file", master_log_name,
                               sizeof(master_log_name)};
  Ulonglong_value master_log_pos{"master_log_pos"};
  Bool_value_with_default master_ssl{"ssl", &::master_ssl};
  Path_value master_ssl_ca{"ssl_ca", &::master_ssl_ca};
  Path_value master_ssl_capath{"ssl_capath", &::master_ssl_capath};
  Path_value master_ssl_cert{"ssl_cert", &::master_ssl_cert};
  Path_value master_ssl_cipher{"ssl_cipher", &::master_ssl_cipher};
  Path_value master_ssl_key{"ssl_key", &::master_ssl_key};
  Bool_value_with_default master_ssl_verify_server_cert{"ssl_verify_server_cert",
    &::master_ssl_verify_server_cert};
  ID_array_value ignore_server_ids{"ignore_server_ids"};
  Path_value master_ssl_crl{"ssl_crl", &::master_ssl_crl};
  Path_value master_ssl_crlpath{"ssl_crlpath",
    &::master_ssl_crlpath};
  Master_use_gtid_value master_use_gtid;
  ID_array_value do_domain_ids{"do_domain_ids"};
  ID_array_value ignore_domain_ids{"ignore_domain_ids"};
  Ulonglong_value_with_default master_retry_count{"retry_count",
    &::master_retry_count};
  Heartbeat_period_value master_heartbeat_period;

  /*
    Per-instance list of value subobjects for the line-based section of
    master.info, in file order. nullptr marks a MySQL-only line that
    MariaDB ignores on read and reserves on write. Must be declared AFTER
    all referenced value members (member init runs in declaration order).
  */
  Value *const value_list[21]= {
    &master_log_file,
    &master_log_pos,
    &master_host,
    &master_user,
    &master_password,
    &master_port,
    &master_connect_retry,
    &master_ssl,
    &master_ssl_ca,
    &master_ssl_capath,
    &master_ssl_cert,
    &master_ssl_cipher,
    &master_ssl_key,
    &master_ssl_verify_server_cert,
    &master_heartbeat_period,
    nullptr, // &master_bind, MDEV-19248
    &ignore_server_ids,
    nullptr, // MySQL master_uuid, ignored by MariaDB
    &master_retry_count,
    &master_ssl_crl,
    &master_ssl_crlpath
  };


  /*
    Guard against trailing garbage left over when a later update causes
    the effective contents to shrink compared to the previous contents.
  */
  static constexpr const char END_MARKER[]= "END_MARKER";


  /*
    The MariaDB key=value section. Keys live in a TYPELIB-keyed lookup
    (value_map_keys + value_map_typelib); the parallel per-instance
    value_map[] array holds the corresponding value subobject pointers.
    For bidirectional compatibility with MySQL and earlier MariaDB,
    keys should match the corresponding old property name in Master_info.

    END_MARKER is included in the typelib so that find_type() detects it
    on load; its slot in value_map is nullptr ("no value, terminator").
    Defined out-of-class in rpl_master_info_file.cc.
  */
  static const char *value_map_keys[];
  static TYPELIB value_map_typelib;

  Value *const value_map[16]= {
    &master_connect_retry,
    &master_ssl,
    &master_ssl_ca,
    &master_ssl_capath,
    &master_ssl_cert,
    &master_ssl_cipher,
    &master_ssl_key,
    &master_ssl_crl,
    &master_ssl_crlpath,
    &master_ssl_verify_server_cert,
    &master_heartbeat_period,
    &master_retry_count,
    &master_use_gtid,
    &do_domain_ids,
    &ignore_domain_ids,
    nullptr // END_MARKER slot (no value)
  };


  Master_info_file(DYNAMIC_ARRAY *ignore_server_ids,
                   DYNAMIC_ARRAY *do_domain_ids,
                   DYNAMIC_ARRAY *ignore_domain_ids);

  bool load_from_file() override;
  void save_to_file() override;

  /*
    Accessors for the gtid mode in effect for the connection to the
    master (Master_use_gtid_value::active_mode), inline so that checks
    go directly to the value:
    * using_gtid() returns the mode itself, for code that must
      distinguish CURRENT_POS from SLAVE_POS.
    * is_gtid_supported() is TRUE when gtid is used for the connection,
      i.e. the mode is not NO.
  */
  enum_master_use_gtid using_gtid() const
  { return master_use_gtid.active_mode; }
  bool is_gtid_supported() const
  { return master_use_gtid.active_mode != enum_master_use_gtid::NO; }
};

#endif // include guard

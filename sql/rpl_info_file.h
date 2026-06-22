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

#ifndef RPL_INFO_FILE_H
#define RPL_INFO_FILE_H

#include <cstdint> // uintN_t & UINTn_MAX
#include <utility> // Explicit markers std::move & std::forward
// Interface type of Master_info_file::master_heartbeat_period
#include "my_decimal.h"
#include <my_sys.h> // IO_CACHE, FN_REFLEN, ...
#include <my_getopt.h> // autoset_my_option for master_heartbeat_period_str
#include "sql_const.h" // MAX_PASSWORD_LENGTH


/**
  A three-way comparison function for using
  sort_dynamic() and bsearch() on ID_array_value::array.
  @return -1 if first argument is less, 0 if it equal to, or 1 if it is greater
  than the second
  @deprecated Use a sorted set, such as `TREE` from `include/my_tree.h`,
  to save on explicitly calling those functions.
*/
int change_master_id_cmp(const void *arg1, const void *arg2);

//TODO: Remove stub from mariadb-corporation/mariadb-columnstore-engine#3951
#ifndef RPL_MASTER_INFO_FILE_H
/// enum for @ref Master_info_file::master_use_gtid
enum struct enum_master_use_gtid
{ NO, CURRENT_POS, SLAVE_POS,
  /**
    Currently, this value is only a sentinel representing that
    @ref master_use_gtid is unset or autoset.
    It is indistinguishable from its effective value to the user,
    as Master_info_file::Use_gtid_value::operator enum_master_use_gtid()
    automatically converts this value according to
    @ref Master_info_file::Use_gtid_value::gtid_supported.
  */
  AUTO
};
/// String names for non-@ref enum_master_use_gtid::DEFAULT values
inline const char *master_use_gtid_names[]=
  {"No", "Current_Pos", "Slave_Pos", nullptr};


/**
  Namespace of helpers for @ref Master_info_file::master_heartbeat_period,
  which is a non-negative `DECIMAL(10,3)` seconds value stored
  internally as a @ref uint32_t milliseconds value.
*/
class Uint32_3
{
  uint32_t value;
public:

  /// @return ((1<<32) - 1) / 1000.0 as a constant '\0'-terminated string
  static constexpr char MAX_STR[]= "4294967.295";
  Uint32_3() {}
  Uint32_3(uint32_t value): value(value) {}
  operator uint32_t &() { return value; }

  enum conversion_status
  { OK, ROUNDED,
    FAILED ///< @see ER_SLAVE_HEARTBEAT_VALUE_OUT_OF_RANGE
  };
  // Also used by the parser
  conversion_status from_decimal(const decimal_t &decimal);
  // Also used by the server option master_heartbeat_period
  /**
    @param delimiter
      This method also returns @ref conversion_status::FAILED if the exclusive
      end of the decimal (which may be `str.str[str.length]`!) is this char.
  */
  conversion_status from_chars(const LEX_CSTRING &str, char delimiter= '\0');
};


/**
  `mariadbd` Options for the `DEFAULT` values of @ref Master_info_file values
  @{
*/
/// Computes the `DEFAULT` value of @ref master_heartbeat_period
extern uint slave_net_timeout;
inline uint32_t master_connect_retry= 60;
// Options do not reset to default if the default is `nullptr`, so use `auto`.
inline char *master_heartbeat_period_str= autoset_my_option;
inline Uint32_3 master_heartbeat_period= {false};
inline bool master_ssl= true;
inline const char *master_ssl_ca     = "";
inline const char *master_ssl_capath = "";
inline const char *master_ssl_cert   = "";
inline const char *master_ssl_crl    = "";
inline const char *master_ssl_crlpath= "";
inline const char *master_ssl_key    = "";
inline const char *master_ssl_cipher = "";
inline bool master_ssl_verify_server_cert= true;
/// `ulong` is the data type `my_getopt` expects.
inline ulong master_use_gtid= static_cast<ulong>(enum_master_use_gtid::AUTO);
inline uint64_t master_retry_count= 100000;
/// @}
#endif // RPL_MASTER_INFO_FILE_H


/**
  This common superclass of @ref Master_info_file and
  @ref Relay_log_info_file provides them common code for saving
  and loading values in their MySQL line-based sections.
  As only the @ref Master_info_file has a MariaDB `key=value`
  section with a mix of explicit and `DEFAULT`-able values,
  code for those are in @ref Master_info_file instead.
*/
struct Info_file
{
  /**
    This is the base for @ref Value that provides a type-agnostic
    interface for file reading/writing and (if supported) `DEFAULT` handing.
    All methods automatically cover the is_default() case, so callers that
    don't diffentiate `DEFAULT` from set values don't need to check themselves.
    @note
      TODO: Split value members from meta-classes (similar
      to @ref Show::Type) to avoid instance-level duplication
  */
  struct Value_interface
  {
    virtual ~Value_interface()= default;
    virtual bool is_default() { return false; }
    /// @return `true` if the value is mandatory and cannot provide a default
    virtual bool set_default() { return true; }
    /** Set the value by reading a line from the IO and consume the `\n`
      @return `false` if the line has parsed successfully, or `true` on error
      @post is_default() is `false`
    */
    virtual bool load_from(IO_CACHE *file)= 0;
    /**
      Write the *effective* value to the IO **without** a `\n`
      (that is, agnostic of the is_default() state to facilitate downgrades)
      @note The caller should separately represent the is_default() state.
    */
    virtual void save_to(IO_CACHE *file)= 0;
  };

  /** This is the actual abstract base template of every value in the info file.
    @tparam T represented type
    @note
      Please explicitly specialize the methods of this type-oriented
      template rather than inheriting from an abstract template class.
      Specializations keep this as a template, reducing bytesize;
      whereas inheritance produces instances of this template
      that are unused on their own beyond as a superclass.
  */
  template<typename T> struct Value: Value_interface
  {
    /** TODO:
      Instead of accessing directly,
      finish migrating @ref Master_info & @ref Relay_log_info
    */
    T value;
  protected:
    bool _is_default= true;
  public:
    static constexpr size_t MAX_CHARS10=
      std::numeric_limits<T>::digits10 + // Fully-utilized decimal digits
      1 + // The partially-utilized digit (e.g., the 2's place in "2147483647")
      std::numeric_limits<T>::is_signed; // for specializing
    const T *const default_value= nullptr;

    Value(T value): value(std::move(value)), _is_default(false) {}
    Value(const T *default_value): default_value(default_value) {}

    /** @pre
      If is_default(), @ref default_value is not null;
      otherwise, @ref value is initialized.
    */
    virtual operator T() { return is_default() ? get_default() : value; }
    /// Fowards to @ref value perfectly
    template<typename O> auto &operator=(O&& other)
    {
      value= std::forward<O>(other);
      _is_default= false;
      return *this;
    }

    virtual T get_default() { return *default_value; }
    bool is_default() override { return _is_default; }
    bool set_default() override
    {
      if (default_value)
      {
        value.~T();
        _is_default= true;
        return false;
      }
      return Value_interface::set_default();
    }
    virtual bool load_from(IO_CACHE *file) override;
    virtual void save_to  (IO_CACHE *file) override;
  };


  /// @ref '\0'-terminated string subclass
  struct String_value: protected LEX_STRING, Value<const char *>
  {
    String_value(char *str, size_t length):
      LEX_STRING({str, length}), Value(str) {}
    String_value(char *str, size_t length, const char *const *default_value):
      LEX_STRING({str, length}), Value(default_value) { DBUG_ASSERT(str); }
    /// @param other `\0`-terminated string, or `nullptr` to call set_default()
    String_value &operator=(const char *other);
    virtual bool load_from(IO_CACHE *file) override;
  };

  /// Shorthand to create a self-managed @ref String_value
  template<size_t size= FN_REFLEN> struct Char_array_value: String_value
  {
    char buf[size]= {'\0'}; // G++ Bug 89053: `""` is too long for `char[1<<28]`
    Char_array_value(): String_value(buf, size) {}
    Char_array_value(const char *const *default_value):
      String_value(buf, size, default_value) {}
    using String_value::operator=;
  };


  IO_CACHE file;
  virtual ~Info_file()= default;

  bool load_from_file() { return get_values(&Info_file::load_from_file_impl); };
  void save_to_file() { get_values(&Info_file::save_to_file_impl); };


protected:
  struct KV
  {
    LEX_CSTRING key;
    Value_interface *value;
    static const uchar *my_hash_get_key(
      const void *ptr, size_t *r_size, [[maybe_unused]] my_bool);
  };
  using get_values_impl= bool (
    Value_interface **mysql_lines, size_t n_mysql_lines,
    KV *key_values, size_t n_key_values);

  /**
    Call `implementation` on `this` with
    * a consistently ordered list of MySQL line-oriented values, and
    * a not-necessarily-ordered set of `key=value` section values.

    For bidirectional compatibility with MySQL
    (codenames only at this writing) and earlier versions of MariaDB,
    keys should match the corresponding old property name in @ref Master_info.

    @note This should not need to allocate dynamic memory.
    @return whatever `implementation` returns
    @see load_from_file()
    @see save_to_file()
  */
  virtual bool get_values(get_values_impl Info_file::*implementation)= 0;

private:
  get_values_impl load_from_file_impl;
  get_values_impl save_to_file_impl;
};


//TODO: Remove stub from mariadb-corporation/mariadb-columnstore-engine#3951
#ifndef RPL_MASTER_INFO_FILE_H
/// @name Explicit template instantiations and specializations
///@{
/// @name Integers
///@{
  extern template struct Info_file::Value<uint32_t>;
  extern template struct Info_file::Value<uint64_t>;
  /**
    Using @ref Uint32_3 instead of @ref uint32_t diffentiates the
    specializations of Master_info_file::master_heartbeat_period.
  */
  ///@{
    template<> bool Info_file::Value<Uint32_3>::load_from(IO_CACHE *);
    template<> void Info_file::Value<Uint32_3>::save_to  (IO_CACHE *);
  ///@}
///@}

/** @name
  These are engineered (that is, hard-coded)
  on the types' single-digit ranges.
*/
///@{
  template<> bool Info_file::Value<bool>::load_from(IO_CACHE *);
  template<> void Info_file::Value<bool>::save_to  (IO_CACHE *);
  template<> bool Info_file::Value<enum_master_use_gtid>::load_from(IO_CACHE *);
  template<> void Info_file::Value<enum_master_use_gtid>::save_to  (IO_CACHE *);
///@}

template<> [[deprecated("use Info_file::String_value instead")]]
inline
bool Info_file::Value<const char *>::load_from(IO_CACHE *file) { return true; }
template<> void Info_file::Value<const char *>::save_to(IO_CACHE *);

  /** Array of `long`s (FIXME: Domain and Server IDs should be `uint32_t`s.)
  @deprecated
    Only one of `DO_DOMAIN_IDS` and `IGNORE_DOMAIN_IDS`
    can be active at a time, so separate arrays are wasteful.
    Until we refactor this pairs, this struct only reference existing arrays
    doesn't construct/destruct) to avoid code that will be obsolete by then.
*/
///@{
  /// @pre @ref array is initialized
  template<>
  inline Info_file::Value<DYNAMIC_ARRAY *>::Value(DYNAMIC_ARRAY *array):
    value(array), _is_default(false) { DBUG_ASSERT(array); }

  template<> bool Info_file::Value<DYNAMIC_ARRAY *>::load_from(IO_CACHE *);
  template<> void Info_file::Value<DYNAMIC_ARRAY *>::save_to  (IO_CACHE *);
///@}
///@}


struct Master_info_file: Info_file
{
  /** @name
    `@@master_info_file` values,
    in SHOW SLAVE STATUS order where applicable
  */
  ///@{

  Char_array_value<HOSTNAME_LENGTH*SYSTEM_CHARSET_MBMAXLEN + 1> master_host;
  Char_array_value<USERNAME_LENGTH + 1> master_user;
  // Not in SHOW SLAVE STATUS
  Char_array_value<MAX_PASSWORD_LENGTH*SYSTEM_CHARSET_MBMAXLEN + 1>
    master_password;
  Value<uint32_t> master_port= MYSQL_PORT;
  /// Connect_Retry
  Value<uint32_t> master_connect_retry= &::master_connect_retry;
  Char_array_value<> master_log_file;
  /// Read_Master_Log_Pos
  Value<my_off_t> master_log_pos= my_off_t(0);
  /// Master_SSL_Allowed
  Value<bool> master_ssl= &::master_ssl;
  /// Master_SSL_CA_File
  Char_array_value<> master_ssl_ca= &::master_ssl_ca;
  /// Master_SSL_CA_Path
  Char_array_value<> master_ssl_capath= &::master_ssl_capath;
  Char_array_value<> master_ssl_cert= &::master_ssl_cert;
  Char_array_value<> master_ssl_cipher= &::master_ssl_cipher;
  Char_array_value<> master_ssl_key= &::master_ssl_key;
  Value<bool> master_ssl_verify_server_cert=
    &::master_ssl_verify_server_cert;
  /// Replicate_Ignore_Server_Ids
  Value<DYNAMIC_ARRAY *> ignore_server_ids;
  Char_array_value<> master_ssl_crl= &::master_ssl_crl;
  Char_array_value<> master_ssl_crlpath= &::master_ssl_crlpath;

  /** Singleton class of @ref Master_info_file::master_use_gtid:
    It is a @ref enum_master_use_gtid value
    with a `DEFAULT` value of @ref ::master_use_gtid,
    which in turn has a `DEFAULT` value based on @ref gtid_supported.
  */
  struct Use_gtid_value: Value<enum_master_use_gtid>
  {
    /**
      @ref master_use_gtid::AUTO is normally `SLAVE_POS`; however, if the
      master does not support GTIDs, we fall back to `NO`. This value caches
      the check so future RESET SLAVE commands don't revert to `SLAVE_POS`.
    */
    bool gtid_supported= true;
    Use_gtid_value(): Value(
      // Substitute until `my_getopt.c` switches from @ref ulongs to enums
      reinterpret_cast<const enum_master_use_gtid *>(&::master_use_gtid)
    ) {}
    operator enum_master_use_gtid() override;
    operator bool()
    { return operator enum_master_use_gtid() != enum_master_use_gtid::NO; }
    using Value<enum_master_use_gtid>::operator=;
    enum_master_use_gtid get_default() override
    { return static_cast<enum_master_use_gtid>(::master_use_gtid); }
  }
  /// Using_Gtid
  master_use_gtid;

  /// Replicate_Do_Domain_Ids
  Value<DYNAMIC_ARRAY *> do_domain_ids;
  /// Replicate_Ignore_Domain_Ids
  Value<DYNAMIC_ARRAY *> ignore_domain_ids;
  Value<uint64_t> master_retry_count= &::master_retry_count;

  /** Singleton class of @ref Master_info_file::master_heartbeat_period:
    It is a @ref Uint32_3 value
    It has a `DEFAULT` value of @ref ::master_heartbeat_period
    which in turn has a `DEFAULT` value of `@@slave_net_timeout / 2` seconds.
    (TODO: A new "`master_heartbeat_frequency`" would be more explicit
     at configuring the"relative to @ref slave_net_timeout" aspect.)
  */
  struct Heartbeat_period_value: Value<Uint32_3>
  {
    Heartbeat_period_value(): Value<Uint32_3>(
      /*
        Substitute only; confirm @ref master_heartbeat_period_str !=
        @ref autoset_my_option before use
      */
      &::master_heartbeat_period
    ) {}
    using Value<Uint32_3>::operator Uint32_3;
    operator uint32_t() { return operator Uint32_3(); }
    using Value<Uint32_3>::operator=;
    Uint32_3 get_default() override;
  }
  /// `Slave_heartbeat_period` of SHOW ALL SLAVES STATUS
  master_heartbeat_period;

  ///@}


  Master_info_file(
    DYNAMIC_ARRAY &ignore_server_ids,
    DYNAMIC_ARRAY &do_domain_ids, DYNAMIC_ARRAY &ignore_domain_ids
  );

  bool get_values(get_values_impl Info_file::*implementation) override;
};
#endif // RPL_MASTER_INFO_FILE_H


struct Relay_log_info_file: Info_file
{
  ///@name `@@relay_log_info_file` values in SHOW SLAVE STATUS order
  ///@{
  Char_array_value<> relay_log_file;
  Value<my_off_t> relay_log_pos= my_off_t(0);
  /// Relay_Master_Log_File (of the event *group*)
  Char_array_value<> read_master_log_file;
  /// Exec_Master_Log_Pos (of the event *group*)
  Value<my_off_t> read_master_log_pos= my_off_t(0);
  /// SQL_Delay
  Value<uint32_t> sql_delay= uint32_t(0);
  ///@}

  bool get_values(get_values_impl Info_file::*implementation) override;
};


#endif // RPL_INFO_FILE_H

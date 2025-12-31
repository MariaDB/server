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

/*FIXME MDEV-38213:
  `rpl_master_info_file.h` requires C++17, but RocksDB,
  which transitively includes this file, is still on C++11.
*/
#if __cplusplus < 201703L && _MSVC_LANG < 201703L
struct Master_info_file;
enum struct enum_master_use_gtid {};
enum struct trilean {};
#else

#include "rpl_info_file.h"
#include <unordered_map> // Type of Master_info_file::VALUE_MAP
#include <string_view>   // Key type of Master_info_file::VALUE_MAP
#include <optional>      // Storage type of Master_info_file::Optional_int_value
#include <unordered_set> // Used by Master_info_file::load_from_file() to dedup
#include "sql_const.h"   // MAX_PASSWORD_LENGTH
// Interface type of Master_info_file::master_heartbeat_period
#include "my_decimal.h"


/**
  A three-way comparison function for using
  sort_dynamic() and bsearch() on ID_array_value::array.
  @return -1 if first argument is less, 0 if it equal to, or 1 if it is greater
  than the second
  @deprecated Use a sorted set, such as @ref std::set,
  to save on explicitly calling those functions.
*/
inline static int change_master_id_cmp(const void *arg1, const void *arg2)
{
  const ulong &id1= *(const ulong *)arg1, &id2= *(const ulong *)arg2;
  return (id1 > id2) - (id1 < id2);
}

/// enum for @ref Master_info_file::Optional_bool_value
/*TODO:
  `UNKNOWN` is the general term in ternary logic, but this name is `#define`d in
  `item_cmpfunc.h`, which is used by target RocksDB, whose *C++11* requirement
  doesn't recognize `inline` constants (whereas the server is on C++17).
*/
enum struct trilean { NO, YES, DEFAULT= -1 };
/// enum for @ref Master_info_file::master_use_gtid
enum struct enum_master_use_gtid { NO, CURRENT_POS, SLAVE_POS, DEFAULT };
/// String names for non-@ref enum_master_use_gtid::DEFAULT values
inline const char *master_use_gtid_names[]=
  {"No", "Current_Pos", "Slave_Pos", nullptr};

/**
  `mariadbd` Options for the `DEFAULT` values of @ref Master_info_file values
  @{
*/
/// Computes the `DEFAULT` value of @ref ::master_heartbeat_period
extern uint slave_net_timeout;
inline uint32_t master_connect_retry= 60;
inline std::optional<uint32_t> master_heartbeat_period= std::nullopt;
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
inline auto master_use_gtid= static_cast<ulong>(enum_master_use_gtid::DEFAULT);
inline uint64_t master_retry_count= 100000;
/// }@


struct Master_info_file: Info_file
{

  /** General Optional Value
    @tparam T wrapped type
 */
  template<typename T> struct Optional_value: Persistent
  {
    std::optional<T> optional;
    virtual operator T()= 0;
    /// Fowards to @ref optional perfectly
    template<typename O> auto &operator=(O&& other)
    {
      optional= std::forward<O>(other);
      return *this;
    }
    bool is_default() override { return !optional.has_value(); }
    bool set_default() override
    {
      optional.reset();
      return false;
    }
  };
  /** Integer Value with `DEFAULT`
    @tparam mariadbd_option
      server options variable that determines the value of `DEFAULT`
    @tparam I integer type (auto-deduced from `mariadbd_option`)
    @see Int_value version without `DEFAULT` (not a superclass)
  */
  template<auto &mariadbd_option,
           typename I= std::remove_reference_t<decltype(mariadbd_option)>>
  struct Optional_int_value: Optional_value<I>
  {
    using Optional_value<I>::operator=;
    operator I() override
    { return Optional_value<I>::optional.value_or(mariadbd_option); }
    virtual bool load_from(IO_CACHE *file) override
    { return Int_IO_CACHE::from_chars<I>(file, this); }
    virtual void save_to(IO_CACHE *file) override
    { return Int_IO_CACHE::to_chars(file, operator I()); }
  };

  /**
    Optional Path Value (for SSL): @ref FN_REFLEN-sized '\0'-
    terminated string with a `mariadbd` option for the `DEFAULT`.
    @note This reuses the @ref String_value::buf to track the `DEFAULT`ed state,
      which is a bit more efficient and convenient than
      `std::optional<std::array<char, FN_REFLEN>>`.
      Specifically, when the strlen() is 0, the value is an empty string if
      the index 1 char is also '\0', or is set_default() if it is '\1'.
  */
  template<const char *&mariadbd_option>
  struct Optional_path_value: String_value<>
  {
    operator const char *() override
    {
      if (is_default())
        return mariadbd_option;
      return String_value<>::operator const char *();
    }
    /// @param other `\0`-terminated string, or `nullptr` to call set_default()
    auto &operator=(const char *other)
    {
      if (other)
      {
        buf[1]= false; // not default
        String_value<>::operator=(other);
      }
      else
        set_default();
      return *this;
    }
    bool is_default() override { return /* strlen() == 0 */ !buf[0] && buf[1]; }
    bool set_default() override
    {
      buf[0]= false;
      buf[1]= true;
      return false;
    }
    bool load_from(IO_CACHE *file) override
    {
      buf[1]= false; // not default
      return String_value<>::load_from(file);
    }
  };

  /** Boolean Value with `DEFAULT`.
    @note
    * This uses the @ref trilean enum,
      which is more efficient than `std::optional<bool>`.
    * load_from() and save_to() are also engineered
      to make use of the range of only two cases.
  */
  template<bool &mariadbd_option> struct Optional_bool_value: Persistent
  {
    trilean value;
    operator bool()
    { return is_default() ? mariadbd_option : (value != trilean::NO); }
    bool is_default() override { return value <= trilean::DEFAULT; }
    bool set_default() override
    {
      value= trilean::DEFAULT;
      return false;
    }
    auto &operator=(trilean other)
    {
      this->value= other;
      return *this;
    }
    auto &operator=(bool value)
    { return operator=(value ? trilean::YES : trilean::NO); }
    /// @return `true` if the line is `0` or `1`, `false` otherwise or on error
    bool load_from(IO_CACHE *file) override
    {
      /** Only three chars are required:
        * One digit
          (When base prefixes are not recognized in integer parsing,
          anything with a leading `0` stops parsing
          after converting the `0` to zero anyway.)
        * the terminating `\n\0` as in IntegerLike::from_chars(IO_CACHE *, I &)
      */
      char buf[3];
      if (my_b_gets(file, buf, 3) && buf[1] == '\n')
        switch (buf[0]) {
        case '0':
          value= trilean::NO;
          return false;
        case '1':
          value= trilean::YES;
          return false;
        }
      return true;
    }
    void save_to(IO_CACHE *file) override
    { my_b_write_byte(file, operator bool() ? '1' : '0'); }
  };


  /** @ref uint32_t Array value
    @deprecated
      Only one of `DO_DOMAIN_IDS` and `IGNORE_DOMAIN_IDS` can be active
      at a time, so giving them separate arrays, let alone value instances,
      is wasteful. Until we refactor this pair, this will only reference
      to existing arrays to reduce changes that will be obsolete by then.
      As references, the struct does not manage (construct/destruct) the array.
  */
  struct ID_array_value: Persistent
  {
    /// Array of `long`s (FIXME: Domain and Server IDs should be `uint32_t`s.)
    DYNAMIC_ARRAY &array;
    ID_array_value(DYNAMIC_ARRAY &array): array(array) {}
    operator DYNAMIC_ARRAY &() { return array; }
    /// @pre @ref array is initialized

    bool load_from(IO_CACHE *file) override
    {
      uint32_t count;
      size_t i;
      /// +1 for the terminating delimiter
      char buf[Int_IO_CACHE::BUF_SIZE<uint32_t> + 1];
      for (i= 0; i < sizeof(buf); ++i)
      {
        int c= my_b_get(file);
        if (c == my_b_EOF)
          return true;
        buf[i]= static_cast<char>(c);
        if (c == /* End of Line */ '\n' || c == /* End of Count */ ' ')
          break;
      }
      /*
        * std::from_chars() fails if `count` will overflow in any way.
        * exclusive end index of the string = size
      */
      std::from_chars_result result= std::from_chars(buf, &buf[i], count);
      // Reserve enough elements ahead of time.
      if (result.ec != Int_IO_CACHE::ERRC_OK || allocate_dynamic(&array, count))
        return true;
      while (count--)
      {
        uint32_t value;
        /*
          Check that the previous number ended with a ` `,
          not `\n` or anything else.
        */
        if (*(result.ptr) != ' ')
          return true;
        for (i= 0; i < sizeof(buf); ++i)
        {
          /*
            Bottlenecks from repeated IO does not affect the
            performance of reading char by char thanks to the cache.
          */
          int c= my_b_get(file);
          if (c == my_b_EOF)
            return true;
          buf[i]= static_cast<char>(c);
          if (c == /* End of Count */ ' ' || c == /* End of Line */ '\n')
            break;
        }
        result= std::from_chars(buf, &buf[i], value);
        if (result.ec != Int_IO_CACHE::ERRC_OK)
          return true;
        ulong id= value;
        bool oom= insert_dynamic(&array, (uchar *)&id);
        /*
          This should not error because enough
          memory was already allocate_dynamic()-ed.
        */
        DBUG_ASSERT(!oom);
        if (oom)
          return true;
      }
      // Check that the last number ended with a `\n`, not ` ` or anything else.
      if (*(result.ptr) != '\n')
        return true;
      sort_dynamic(&array, change_master_id_cmp); // to be safe
      return false;
    }

    /// Store the total number of elements followed by the individual elements.
    void save_to(IO_CACHE *file) override
    {
      Int_IO_CACHE::to_chars(file, array.elements);
      for (size_t i= 0; i < array.elements; ++i)
      {
        ulong id;
        get_dynamic(&array, &id, i);
        my_b_write_byte(file, ' ');
        Int_IO_CACHE::to_chars(file, id);
      }
    }
  };


  /**
    `@@master_info_file` values, in SHOW SLAVE STATUS order where applicable
    @{
  */

  String_value<HOSTNAME_LENGTH*SYSTEM_CHARSET_MBMAXLEN + 1> master_host;
  String_value<USERNAME_LENGTH + 1> master_user;
  // Not in SHOW SLAVE STATUS
  String_value<MAX_PASSWORD_LENGTH*SYSTEM_CHARSET_MBMAXLEN + 1> master_password;
  Int_value<uint32_t> master_port;
  /// Connect_Retry
  Optional_int_value<::master_connect_retry> master_connect_retry;
  String_value<> master_log_file;
  /// Read_Master_Log_Pos
  Int_value<my_off_t> master_log_pos;
  /// Master_SSL_Allowed
  Optional_bool_value<::master_ssl> master_ssl;
  /// Master_SSL_CA_File
  Optional_path_value<::master_ssl_ca> master_ssl_ca;
  /// Master_SSL_CA_Path
  Optional_path_value<::master_ssl_capath> master_ssl_capath;
  Optional_path_value<::master_ssl_cert> master_ssl_cert;
  Optional_path_value<::master_ssl_cipher> master_ssl_cipher;
  Optional_path_value<::master_ssl_key> master_ssl_key;
  Optional_bool_value<::master_ssl_verify_server_cert>
    master_ssl_verify_server_cert;
  /// Replicate_Ignore_Server_Ids
  ID_array_value ignore_server_ids;
  Optional_path_value<::master_ssl_crl> master_ssl_crl;
  Optional_path_value<::master_ssl_crlpath> master_ssl_crlpath;

  /** Singleton class of @ref Master_info_file::master_use_gtid:
    It is a @ref enum_master_use_gtid value
    with a `DEFAULT` value of @ref ::master_use_gtid,
    which in turn has a `DEFAULT` value based on @ref gtid_supported.
  */
  struct: Persistent
  {
    enum_master_use_gtid mode;
    /**
      The default `master_use_gtid` is normally `SLAVE_POS`; however, if the
      master does not supports GTIDs, we fall back to `NO`. This value caches
      the check so future RESET SLAVE commands don't revert to `SLAVE_POS`.
      load_from() and save_to() are engineered (that is, hard-coded)
      on the single-digit range of @ref enum_master_use_gtid,
      similar to Optional_bool_value.
    */
    bool gtid_supported= true;
    operator enum_master_use_gtid()
    {
      if (is_default())
      {
        auto default_use_gtid=
          static_cast<enum_master_use_gtid>(::master_use_gtid);
        return default_use_gtid >= enum_master_use_gtid::DEFAULT ? (
          gtid_supported ?
            enum_master_use_gtid::SLAVE_POS : enum_master_use_gtid::NO
        ) : default_use_gtid;
      }
      return mode;
    }
    operator bool()
    { return operator enum_master_use_gtid() != enum_master_use_gtid::NO; }
    auto &operator=(enum_master_use_gtid mode)
    {
      this->mode= mode;
      return *this;
    }
    bool is_default() override
    { return mode >= enum_master_use_gtid::DEFAULT; }
    bool set_default() override
    {
      mode= enum_master_use_gtid::DEFAULT;
      return false;
    }
    /** @return
      `true` if the line is a @ref enum_master_use_gtid,
      `false` otherwise or on error
    */
    bool load_from(IO_CACHE *file) override
    {
      /**
        Only 3 chars are required for the enum,
        similar to @ref Optional_bool_value::load_from()
      */
      char buf[3];
      if (!my_b_gets(file, buf, 3) ||
          buf[1] != '\n' ||
          buf[0] > /* SLAVE_POS */ '2' || buf[0] < /* NO */ '0')
        return true;
      operator=(static_cast<enum_master_use_gtid>(buf[0] - '0'));
      return false;
    }
    void save_to(IO_CACHE *file) override
    {
      my_b_write_byte(file,
        '0' + static_cast<unsigned char>(operator enum_master_use_gtid()));
    }
  }
  /// Using_Gtid
  master_use_gtid;

  /// Replicate_Do_Domain_Ids
  ID_array_value do_domain_ids;
  /// Replicate_Ignore_Domain_Ids
  ID_array_value ignore_domain_ids;
  Optional_int_value<::master_retry_count> master_retry_count;

  /** Singleton class of Master_info_file::master_heartbeat_period:
    It is a non-negative `DECIMAL(10,3)` seconds value internally
    calculated as an unsigned integer milliseconds value.
    It has a `DEFAULT` value of @ref ::master_heartbeat_period,
    which in turn has a `DEFAULT` value of `@@slave_net_timeout / 2` seconds.
  */
  struct Heartbeat_period_value: Optional_value<uint32_t>
  {
    /**
      @return std::numeric_limits<uint32_t>::max() / 1000.0
        as a constant '\0'-terminated string
    */
    static constexpr char MAX[]= "4294967.295";
    using Optional_value::operator=;
    operator uint32_t() override
    {
      return is_default() ? ::master_heartbeat_period.value_or(
        MY_MIN(slave_net_timeout*500ULL, std::numeric_limits<uint32_t>::max())
      ) : *(Optional_value<uint32_t>::optional);
    }
    /** Load from a `DECIMAL(10,3)`
      @param overprecise
        set to `true` if the decimal has more than 3 decimal digits
      @return whether the decimal is out of range
      @post Output arguments are set on success and
       not changed if the decimal is out of range.
    */
    static uint from_decimal(
      uint32_t &result, const decimal_t &decimal, bool &overprecise
    )
    {
      /// Wrapper to enable only-once static const construction
      struct Decimal_from_str: my_decimal
      {
        Decimal_from_str(const char *str, size_t strlen): my_decimal()
        {
          const char *end= &(str[strlen]);
          [[maybe_unused]] int unexpected_error= str2my_decimal(
            E_DEC_ERROR, str, this, const_cast<char **>(&end));
          DBUG_ASSERT(!unexpected_error && !*end);
        }
      };
      /*
        The ideal use would work with `double`s, including the `*1000` step,
        but disappointingly, the double interfaces of @ref decimal_t are
        implemented by printing into a string and parsing that char array.
      */
      static const auto MAX_PERIOD= Decimal_from_str(STRING_WITH_LEN(MAX)),
                        THOUSAND  = Decimal_from_str(STRING_WITH_LEN("1000"));
      ulonglong decimal_out;
      if (decimal.sign || decimal_cmp(&MAX_PERIOD, &decimal) < 0)
        return true; // ER_SLAVE_HEARTBEAT_VALUE_OUT_OF_RANGE
      overprecise= decimal.frac > 3;
      // decomposed from my_decimal2int() to reduce a bit of computations
      auto rounded= my_decimal(), product= my_decimal();
      int unexpected_error=
        decimal_round(&decimal, &rounded, 3, HALF_UP) |
        decimal_mul(&rounded, &THOUSAND, &product) |
        decimal2ulonglong(&product, &decimal_out);
      DBUG_ASSERT(!unexpected_error);
      result= static_cast<uint32_t>(decimal_out);
      return unexpected_error;
    }
    /** Load from a '\0'-terminated string
      @param expected_end This function also checks that the exclusive end
        of the decimal *(which may be `str_end` itself)* is this delimiter.
      @return from_decimal(), or `true` on unexpected contents
      @post Output arguments are set on success and not changed on error.
    */
    static uint from_chars(
      std::optional<uint32_t> &self, const char *str,
      const char *str_end, bool &overprecise, char expected_end= '\n'
    )
    {
      uint32_t result;
      auto decimal= my_decimal();
      if (str2my_decimal(
          E_DEC_ERROR, str, &decimal, const_cast<char **>(&str_end)
        ) || *str_end != expected_end ||
        from_decimal(result, decimal, overprecise))
        return true;
      self.emplace(result);
      return false;
    }
    bool load_from(IO_CACHE *file) override
    {
      /**
        Number of chars Optional_int_value::load_from() uses plus
        1 for the decimal point; truncate the excess precision,
        which there should not be unless the file is edited externally.
      */
      char buf[Int_IO_CACHE::BUF_SIZE<uint32_t> + 3];
      bool overprecise;
      size_t length= my_b_gets(file, buf, sizeof(buf));
      return !length ||
        from_chars(optional, buf, &(buf[length]), overprecise) || overprecise;
    }
    /**
      This method is engineered (that is, hard-coded) to take
      full advantage of the non-negative `DECIMAL(10,3)` format.
    */
    void save_to(IO_CACHE *file) override {
      char buf[Int_IO_CACHE::BUF_SIZE<uint32_t> - /* decimal part */ 3];
      auto[integer_part, decimal_part]= div(operator uint32_t(), 1000);
      std::to_chars_result result=
        std::to_chars(buf, &buf[sizeof(buf)], integer_part);
      DBUG_ASSERT(result.ec == Int_IO_CACHE::ERRC_OK);
      my_b_write(file, reinterpret_cast<const uchar *>(buf), result.ptr - buf);
      my_b_write_byte(file, '.');
      result= std::to_chars(buf, &buf[sizeof(buf)], decimal_part);
      DBUG_ASSERT(result.ec == Int_IO_CACHE::ERRC_OK);
      for (ptrdiff_t digits= result.ptr - buf; digits < 3; ++digits)
        my_b_write_byte(file, '0');
      my_b_write(file, reinterpret_cast<const uchar *>(buf), result.ptr - buf);
    }
  }
  /// `Slave_heartbeat_period` of SHOW ALL SLAVES STATUS
  master_heartbeat_period;

  /// }@


  inline static const Mem_fn VALUE_LIST[] {
    &Master_info_file::master_log_file,
    &Master_info_file::master_log_pos,
    &Master_info_file::master_host,
    &Master_info_file::master_user,
    &Master_info_file::master_password,
    &Master_info_file::master_port,
    &Master_info_file::master_connect_retry,
    &Master_info_file::master_ssl,
    &Master_info_file::master_ssl_ca,
    &Master_info_file::master_ssl_capath,
    &Master_info_file::master_ssl_cert,
    &Master_info_file::master_ssl_cipher,
    &Master_info_file::master_ssl_key,
    &Master_info_file::master_ssl_verify_server_cert,
    &Master_info_file::master_heartbeat_period,
    nullptr, // &Master_info_file::master_bind, // MDEV-19248
    &Master_info_file::ignore_server_ids,
    nullptr, // MySQL `master_uuid`, which MariaDB ignores.
    &Master_info_file::master_retry_count,
    &Master_info_file::master_ssl_crl,
    &Master_info_file::master_ssl_crlpath
  };

  /**
    Guard agaist extra left-overs at the end of file in case a later update
    causes the effective content to shrink compared to earlier contents
  */
  static constexpr const char END_MARKER[]= "END_MARKER";
  /** A keyed iterable for the `key=value` section of `@@master_info_file`.
    For bidirectional compatibility with MySQL
    (codenames only at this writing) and earlier versions of MariaDB,
    keys should match the corresponding old property name in @ref Master_info.
  */
  // C++ default allocator to match that `mysql_execute_command()` uses `new`
  inline static
  const std::unordered_map<std::string_view, const Mem_fn> VALUE_MAP= {
    /*
      These are here to annotate whether they are `DEFAULT`.
      They are repeated from @ref VALUE_LIST to enable bidirectional
      compatibility with MySQL and earlier versions of MariaDB
      (where unrecognized keys, such as those from the future, are ignored).
    */
    {"connect_retry"    , &Master_info_file::master_connect_retry         },
    {"ssl"              , &Master_info_file::master_ssl                   },
    {"ssl_ca"           , &Master_info_file::master_ssl_ca                },
    {"ssl_capath"       , &Master_info_file::master_ssl_capath            },
    {"ssl_cert"         , &Master_info_file::master_ssl_cert              },
    {"ssl_cipher"       , &Master_info_file::master_ssl_cipher            },
    {"ssl_key"          , &Master_info_file::master_ssl_key               },
    {"ssl_crl"          , &Master_info_file::master_ssl_crl               },
    {"ssl_crlpath"      , &Master_info_file::master_ssl_crlpath           },
    {"ssl_verify_server_cert",
                          &Master_info_file::master_ssl_verify_server_cert},
    {"heartbeat_period" , &Master_info_file::master_heartbeat_period      },
    {"retry_count"      , &Master_info_file::master_retry_count           },
    // These are the ones new in MariaDB.
    {"using_gtid",        &Master_info_file::master_use_gtid  },
    {"do_domain_ids",     &Master_info_file::do_domain_ids    },
    {"ignore_domain_ids", &Master_info_file::ignore_domain_ids},
    {END_MARKER, nullptr}
  };


  Master_info_file(
    DYNAMIC_ARRAY &ignore_server_ids,
    DYNAMIC_ARRAY &do_domain_ids, DYNAMIC_ARRAY &ignore_domain_ids
  ):
    ignore_server_ids(ignore_server_ids),
    do_domain_ids(do_domain_ids), ignore_domain_ids(ignore_domain_ids)
  {
    for(auto &[_, mem_fn]: VALUE_MAP)
      if (mem_fn)
        mem_fn(this).set_default();
  }

  bool load_from_file() override
  {
    /// Repurpose the trailing `\0` spot to prepare for the `=` or `\n`
    static constexpr size_t LONGEST_KEY_SIZE= sizeof("ssl_verify_server_cert");
    if (Info_file::load_from_file(VALUE_LIST, /* MASTER_CONNECT_RETRY */ 7))
      return true;
    /*
      Info_file::load_from_file() is only for fixed-position entries.
      Proceed with `key=value` lines for MariaDB 10.0 and above:
      The "value" can then be read individually after consuming the`key=`.
    */
    /**
      MariaDB 10.0 does not have the `END_MARKER` before any left-overs at
      the end of the file, so ignore any non-first occurrences of a key.
      @note
        This set only "contains" the static strings of @ref VALUE_MAP's keys,
        which means it can simply compare pointers by face values rather than
        their pointed content, in contrast with how `HASH` of `include/hash.h`
        is designed for string contents in a specified charset.
    */
    auto seen= std::unordered_set<const char *>();
    while (true)
    {
      /**
        A `key=value` line might not actually have the `=value` part;
        in this case, it means this value was set_default().
      */
      bool found_equal= false;
      char key[LONGEST_KEY_SIZE];
      for (size_t i= 0; i < LONGEST_KEY_SIZE; ++i)
      {
        switch (int c= my_b_get(&file)) {
        case my_b_EOF:
          return i; // OK if no chars were read, or error if the line hits EOF.
        case '=':
          found_equal= true;
        [[fallthrough]];
        case '\n':
        {
          decltype(VALUE_MAP)::const_iterator kv=
            VALUE_MAP.find(std::string_view(
              key,
              i // size = exclusive end index of the string
            ));
          // The "unknown" lines would be ignored to facilitate downgrades.
          if (kv != VALUE_MAP.cend()) // found
          {
            const char *key= kv->first.data();
            if (key == END_MARKER)
              return false;
            /**
              The `second` member of std::unordered_set::insert()'s return
              is `true` for a new insertion or `false` for a duplicate.
            */
            else if (seen.insert(key).second)
            {
              Persistent &value= kv->second(this);
              if (found_equal ? value.load_from(&file) : value.set_default())
                return true;
            }
          }
          goto break_for;
        }
        default:
          key[i]= static_cast<char>(c);
        }
      }
break_for:;
    }
  }

  void save_to_file() override
  {
    // Write the line-based section with some reservations for MySQL additions
    Info_file::save_to_file(VALUE_LIST, 33);
    /* Write MariaDB `key=value` lines:
      The "value" can then be written individually after generating the`key=`.
    */
    for (auto &[key, pm]: VALUE_MAP)
      if (pm)
      {
        Persistent &value= pm(this);
        my_b_write(&file,
                   reinterpret_cast<const uchar *>(key.data()), key.size());
        if (!value.is_default())
        {
          my_b_write_byte(&file, '=');
          value.save_to(&file);
        }
        my_b_write_byte(&file, '\n');
      }
    my_b_write(&file, reinterpret_cast<const uchar *>(END_MARKER),
              sizeof(END_MARKER) - /* the '\0' */ 1);
    my_b_write_byte(&file, '\n');
  }

};

#endif // C++ standard guard
#endif // include guard

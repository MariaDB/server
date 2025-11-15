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

#include "rpl_info_file.h"
#include <unordered_map> // Type of @ref Master_info_file::FIELDS_MAP
#include <string_view>   // Key type of @ref Master_info_file::FIELDS_MAP
#include <optional>      // Storage type of @ref Optional_int_field
#include <unordered_set> // Used by Master_info_file::load_from_file() to dedup
#include "sql_const.h"   // MAX_PASSWORD_LENGTH


/**
  A three-way comparison function for using
  sort_dynamic() and bsearch() on ID_array_field::array.
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

/// enum for @ref Master_info_file::master_use_gtid
enum struct enum_master_use_gtid { NO, CURRENT_POS, SLAVE_POS, DEFAULT };
/// String names for non-@ref enum_master_use_gtid::DEFAULT values
inline const char *master_use_gtid_names[]=
  {"No", "Current_Pos", "Slave_Pos", nullptr};

/**
  `mariadbd` Options for the `DEFAULT` values of @ref Master_info_file fields
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
inline uint64_t master_retry_count= 100'000;
/// }@


struct Master_info_file: Info_file
{

  /** General Optional Field
    @tparam T wrapped type
 */
  template<typename T> struct Optional_field: virtual Persistent
  {
    std::optional<T> optional;
    virtual operator T()= 0;
    auto &operator=(T value)
    {
      optional.emplace(value);
      return *this;
    }
    bool is_default() override { return !optional.has_value(); }
    bool set_default() override
    {
      optional.reset();
      return false;
    }
  };
  /** Integer Field with `DEFAULT`
    @tparam mariadbd_option
      server options variable that determines the value of `DEFAULT`
    @tparam I integer type (auto-deduced from `mariadbd_option`)
    @see Int_field version without `DEFAULT` (not a superclass)
  */
  template<auto &mariadbd_option,
           typename I= std::remove_reference_t<decltype(mariadbd_option)>>
  struct Optional_int_field: Optional_field<I>
  {
    using Optional_field<I>::operator=;
    operator I() override
    { return Optional_field<I>::optional.value_or(mariadbd_option); }
    virtual bool load_from(IO_CACHE *file) override
    { return Int_IO_CACHE::from_chars<I>(file, this); }
    virtual void save_to(IO_CACHE *file) override
    { return Int_IO_CACHE::to_chars(file, operator I()); }
  };

  /** SSL Path Field:
    @ref FN_REFLEN-sized C-string with a `mariadbd` option for the `DEFAULT`.
    Empty string is "\0\0" and `DEFAULT`ed string is "\0\1".
  */
  template<const char *const &mariadbd_option>
  struct Optional_path_field: String_field<>
  {
    operator const char *() override
    {
      if (is_default())
        return mariadbd_option;
      return String_field<>::operator const char *();
    }
    auto &operator=(const char *other)
    {
      buf[1]= false; // not default
      String_field<>::operator=(other);
      return *this;
    }
    bool is_default() override { return !buf[0] && buf[1]; }
    bool set_default() override
    {
      buf[0]= false;
      buf[1]= true;
      return false;
    }
    bool load_from(IO_CACHE *file) override
    {
      buf[1]= false; // not default
      return String_field<>::load_from(file);
    }
  };

  /** Boolean Field with `DEFAULT`:
    This uses a trilean enum,
    which is more efficient than `std::optional<bool>`.
    load_from() and save_to() are also engineered
    to make use of the range of only two cases.
  */
  template<bool &mariadbd_option> struct Optional_bool_field: Persistent
  {
    enum { NO, YES, DEFAULT= -1 } value;
    operator bool() { return is_default() ? mariadbd_option : (value != NO); }
    bool is_default() override { return value <= DEFAULT; }
    bool set_default() override
    {
      value= DEFAULT;
      return false;
    }
    auto &operator=(bool value)
    {
      this->value= value ? YES : NO;
      return *this;
    }
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
          value= NO;
          return false;
        case '1':
          value= YES;
          return false;
        }
      return true;
    }
    void save_to(IO_CACHE *file) override
    { my_b_write_byte(file, operator bool() ? '1' : '0'); }
  };


  /** @ref uint32_t Array field
    @deprecated
      Only one of `DO_DOMAIN_IDS` and `IGNORE_DOMAIN_IDS` can be active
      at a time, so giving them separate arrays, let alone field instances,
      is wasteful. Until we refactor this pair, this will only reference
      to existing arrays to reduce changes that will be obsolete by then.
      As references, the struct does not manage (construct/destruct) the array.
  */
  struct ID_array_field: Persistent
  {
    /// Array of `long`s (FIXME: Domain and Server IDs should be `uint32_t`s.)
    DYNAMIC_ARRAY &array;
    ID_array_field(DYNAMIC_ARRAY &array): array(array) {}
    operator DYNAMIC_ARRAY &() { return array; }
    /// @pre @ref array is initialized

    bool load_from(IO_CACHE *file) override
    {
      uint32_t count;
      size_t i;
      /// +1 for the terminating delimiter
      char buf[Int_IO_CACHE::BUF_SIZE<uint32_t> + 1];
      for (i=0; i < sizeof(buf); ++i)
      {
        int c= my_b_get(file);
        if (c == my_b_EOF)
          return true;
        buf[i]= c;
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
        for (i=0; i < sizeof(buf); ++i)
        {
          /*
            Bottlenecks from repeated IO does not affect the
            performance of reading char by char thanks to the cache.
          */
          int c= my_b_get(file);
          if (c == my_b_EOF)
            return true;
          buf[i]= c;
          if (c == /* End of Count */ ' ' || c == /* End of Line */ '\n')
            break;
        }
        result= std::from_chars(buf, &buf[i], value);
        if (result.ec != Int_IO_CACHE::ERRC_OK)
          return true;
        ulong id= value;
        if (insert_dynamic(&array, (uchar *)&id))
        {
          DBUG_ASSERT(!"insert_dynamic(ID_array_field.array)");
          return true;
        }
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
    `@@master_info_file` fields, in SHOW SLAVE STATUS order where applicable
    @{
  */

  String_field<HOSTNAME_LENGTH*SYSTEM_CHARSET_MBMAXLEN + 1> master_host;
  String_field<USERNAME_LENGTH + 1> master_user;
  // Not in SHOW SLAVE STATUS
  String_field<MAX_PASSWORD_LENGTH*SYSTEM_CHARSET_MBMAXLEN + 1> master_password;
  Int_field<uint32_t> master_port;
  /// Connect_Retry
  Optional_int_field<::master_connect_retry> master_connect_retry;
  String_field<> master_log_file;
  /// Read_Master_Log_Pos
  Int_field<my_off_t> master_log_pos;
  /// Master_SSL_Allowed
  Optional_bool_field<::master_ssl> master_ssl;
  /// Master_SSL_CA_File
  Optional_path_field<::master_ssl_ca> master_ssl_ca;
  /// Master_SSL_CA_Path
  Optional_path_field<::master_ssl_capath> master_ssl_capath;
  Optional_path_field<::master_ssl_cert> master_ssl_cert;
  Optional_path_field<::master_ssl_cipher> master_ssl_cipher;
  Optional_path_field<::master_ssl_key> master_ssl_key;
  Optional_bool_field<::master_ssl_verify_server_cert>
    master_ssl_verify_server_cert;
  /// Replicate_Ignore_Server_Ids
  ID_array_field ignore_server_ids;
  Optional_path_field<::master_ssl_crl> master_ssl_crl;
  Optional_path_field<::master_ssl_crlpath> master_ssl_crlpath;

  /** Singleton class of @ref Master_info_file::master_use_gtid:
    It is a @ref enum_master_use_gtid field
    with a `DEFAULT` value of @ref ::master_use_gtid,
    which in turn has a `DEFAULT` value based on @ref gtid_supported.
  */
  struct: Persistent
  {
    enum_master_use_gtid mode;
    /**
      The default `master_use_gtid` is normally `SLAVE_POS`; however, if the
      master does not supports GTIDs, we fall back to `NO`. This field caches
      the check so future RESET SLAVE commands don't revert to `SLAVE_POS`.
      load_from() and save_to() are engineered (that is, hard-coded)
      on the single-digit range of @ref enum_master_use_gtid,
      similar to Optional_bool_field.
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
      DBUG_ASSERT(!is_default());
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
        similar to @ref Optional_bool_field::load_from()
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
  ID_array_field do_domain_ids;
  /// Replicate_Ignore_Domain_Ids
  ID_array_field ignore_domain_ids;
  Optional_int_field<::master_retry_count> master_retry_count;

  /** Singleton class of Master_info_file::master_heartbeat_period:
    It is a non-negative `DECIMAL(10,3)` seconds field internally
    calculated as an unsigned integer milliseconds field.
    It has a `DEFAULT` value of @ref ::master_heartbeat_period,
    which in turn has a `DEFAULT` value of `@@slave_net_timeout / 2` seconds.
  */
  struct: Optional_field<uint32_t>
  {
    using Optional_field::operator=;
    operator uint32_t() override
    {
      return is_default() ? ::master_heartbeat_period.value_or(
        MY_MIN(slave_net_timeout*500ULL, SLAVE_MAX_HEARTBEAT_PERIOD)
      ) : *(Optional_field<uint32_t>::optional);
    }
    bool load_from(IO_CACHE *file) override
    {
      /// Read in floating point first to validate the range
      double seconds;
      /**
        Number of chars Optional_int_field::load_from() uses plus
        1 for the decimal point; truncate the excess precision,
        which there should not be unless the file is edited externally.
      */
      char buf[Int_IO_CACHE::BUF_SIZE<uint32_t> + 3];
      size_t length= my_b_gets(file, buf, sizeof(buf));
      if (!length)
        return true;
#if defined(__clang__) ? _LIBCPP_VERSION < 200000 :\
    defined(__GNUC__) && __GNUC__ < 11
      // FIXME: floating-point variants of `std::from_chars()` not supported
      char end;
      if (sscanf(buf, "%lf%c", &seconds, &end) < 2 || end
#else
      std::from_chars_result result=
        std::from_chars(buf, &buf[length], seconds, std::chars_format::fixed);
      if (result.ec != Int_IO_CACHE::ERRC_OK || *(result.ptr)
#endif
            != '\n' || seconds < 0 || seconds > SLAVE_MAX_HEARTBEAT_PERIOD)
        return true;
      operator=(static_cast<uint32_t>(seconds * 1000));
      return false;
    }
    /**
      This method is engineered (that is, hard-coded) to take
      full advantage of the non-negative `DECIMAL(10,3)` format.
    */
    void save_to(IO_CACHE *file) override {
      char buf[Int_IO_CACHE::BUF_SIZE<uint32_t>];
      std::to_chars_result result=
        std::to_chars(buf, &buf[sizeof(buf)], operator uint32_t());
      DBUG_ASSERT(result.ec == Int_IO_CACHE::ERRC_OK);
      ptrdiff_t size= result.ptr - buf;
      if (size > 3) // decimal seconds has ones digit or more
      {
        my_b_write(file, (const uchar *)buf, size - 3);
        my_b_write_byte(file, '.');
        my_b_write(file, (const uchar *)(&result.ptr[-3]), 3);
      }
      else
      {
        my_b_write_byte(file, '0');
        my_b_write_byte(file, '.');
        for (ptrdiff_t zeroes= size; zeroes < 3; ++zeroes)
          my_b_write_byte(file, '0');
        my_b_write(file, (const uchar *)buf, size);
      }
    }
  }
  /// `Slave_heartbeat_period` of SHOW ALL SLAVES STATUS
  master_heartbeat_period;

  /// }@


  inline static Mem_fn::List FIELDS_LIST= {
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
    nullptr, // MySQL field `master_uuid`, which MariaDB ignores.
    &Master_info_file::master_retry_count,
    &Master_info_file::master_ssl_crl,
    &Master_info_file::master_ssl_crlpath
  };

  /**
    Guard agaist extra left-overs at the end of file in case a later update
    causes the effective content to shrink compared to earlier contents
  */
  static constexpr const char END_MARKER[]= "END_MARKER";
  /// An iterable for the `key=value` section of `@@master_info_file`
  // C++ default allocator to match that `mysql_execute_command()` uses `new`
  inline static const std::unordered_map<std::string_view, Mem_fn> FIELDS_MAP= {
    // These are here to annotate whether they are `DEFAULT`.
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
    /* These are the ones new in MariaDB.
      For backward compatibility,
      keys should match the corresponding old property name in @ref Master_info.
    */
    {"using_gtid",        &Master_info_file::master_use_gtid  },
    {"do_domain_ids",     &Master_info_file::do_domain_ids    },
    {"ignore_domain_ids", &Master_info_file::ignore_domain_ids},
    {END_MARKER, nullptr}
  };


  Master_info_file(DYNAMIC_ARRAY &ignore_server_ids,
                 DYNAMIC_ARRAY &do_domain_ids, DYNAMIC_ARRAY &ignore_domain_ids)
    : ignore_server_ids(ignore_server_ids),
      do_domain_ids(do_domain_ids), ignore_domain_ids(ignore_domain_ids)
  {
    for(auto &[_, Mem_fn]: FIELDS_MAP)
      if (static_cast<bool>(Mem_fn))
        Mem_fn(this).set_default();
  }

  bool load_from_file() override
  {
    /// Repurpose the trailing `\0` spot to prepare for the `=` or `\n`
    static constexpr size_t MAX_KEY_SIZE= sizeof("ssl_verify_server_cert");
    if (Info_file::load_from_file(FIELDS_LIST, /* MASTER_CONNECT_RETRY */ 7))
      return true;
    /*
      Info_file::load_from_file() is only for fixed-position entries.
      Proceed with `key=value` lines for MariaDB 10.0 and above:
      The "value" can then be read individually after consuming the`key=`.
    */
    /**
      MariaDB 10.0 does not have the `END_MARKER` before any left-overs at
      the end of the file, so ignore any non-first occurrences of a key.
    */
    auto seen= std::unordered_set<const char *>();
    while (true)
    {
      bool found_equal= false;
      char key[MAX_KEY_SIZE];
      for (size_t i= 0; i < MAX_KEY_SIZE; ++i)
      {
        switch (int c= my_b_get(&file)) {
        case my_b_EOF:
          return i; // OK if no chars were read, or error if the line hits EOF.
        case '=':
          found_equal= true;
        [[fallthrough]];
        case '\n':
        {
          decltype(FIELDS_MAP)::const_iterator kv=
            FIELDS_MAP.find(std::string_view(
              key,
              i // size = exclusive end index of the string
            ));
          // The "unknown" lines would be ignored to facilitate downgrades.
          if (kv != FIELDS_MAP.cend()) // found
          {
            const char *key= kv->first.data();
            if (key == END_MARKER)
              return false;
            else if (seen.insert(key).second) // if no previous insertion
            {
              Persistent &field= kv->second(this);
              /*
                If there is no `=value` part,
                it means the field was saved with `DEFAULT` as its value.
              */
              if (found_equal ? field.load_from(&file) : field.set_default())
                return true;
            }
          }
          goto break_for;
        }
        default:
          key[i]= c;
        }
      }
break_for:;
    }
  }

  void save_to_file() override
  {
    // Write the line-based section with some reservations for MySQL additions
    Info_file::save_to_file(FIELDS_LIST, 33);
    /* Write MariaDB `key=value` lines:
      The "value" can then be written individually after generating the`key=`.
    */
    for (auto &[key, pm]: FIELDS_MAP)
      if (static_cast<bool>(pm))
      {
        Persistent &field= pm(this);
        my_b_write(&file, (const uchar *)key.data(), key.size());
        if (!field.is_default())
        {
          my_b_write_byte(&file, '=');
          field.save_to(&file);
        }
        my_b_write_byte(&file, '\n');
      }
    my_b_write(&file, (const uchar *)END_MARKER,
              sizeof(END_MARKER) - /* the '\0' */ 1);
    my_b_write_byte(&file, '\n');
  }

};

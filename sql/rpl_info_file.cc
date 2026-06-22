/*
  Copyright (c) 2026 MariaDB

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
#include "sql_hset.h" // Index for Info_file::load_from_file_impl()


int change_master_id_cmp(const void *arg1, const void *arg2)
{
  const ulong &id1= *static_cast<const ulong *>(arg1);
  const ulong &id2= *static_cast<const ulong *>(arg2);
  return (id1 > id2) - (id1 < id2);
}


/**
  @return
    The `error` output from my_strtoll10(),
    or @ref MY_ERRNO_EDOM if the number is not terminated by a '\n'
  @deprecated my_strtoll10() doesn't cover every use case; what would that be?
*/
static int my_b_getsll(IO_CACHE *file, longlong &value)
{
  int error;
  /// +1 for the trailing '\n' that my_b_gets() includes
  char buf[LONGLONG_BUFFER_SIZE + 1];
  /// includes the `\n` but excludes the `\0`
  size_t length= my_b_gets(file, buf, sizeof(buf));
  if (!length) // EOF
    return MY_ERRNO_EDOM;
  char *end= &(buf[length]);
  value= my_strtoll10(buf, &end, &error);
  switch (error) {
  case -1:
  case 0:
    if (*end != '\n')
      return MY_ERRNO_EDOM;
  }
  return error;
}

template<typename T> bool Info_file::Value<T>::load_from(IO_CACHE *file)
{
  longlong val;
  switch (my_b_getsll(file, val)) {
  case -1:
    if (!std::numeric_limits<T>::is_signed)
      return true;
    [[fallthrough]];
  case 0:
  {
    T value= static_cast<T>(val);
    /*TODO:
      This upper range check is not needed when using type-
      specific variants of a safe string-to-integer converter
      (e.g., std::from_chars() when all platforms support it).
    */
    if (value <= std::numeric_limits<T>::max())
    {
      (*this)= value;
      return false;
    }
  }
  }
  return true;
}

template<typename T> void Info_file::Value<T>::save_to(IO_CACHE *file)
{
  char buf[MAX_CHARS10];
  /*TODO:
    * my_b_printf() needs updates and so doesn't
      support `long long`s at the moment.
    * We can avoid format parsing by expanding
      int10_to_str() if not supporting std::to_chars().
  */
  int len= std::numeric_limits<T>::is_signed ?
    snprintf(buf, sizeof(buf), "%lld",
     static_cast<long long>(operator T())) :
    snprintf(buf, sizeof(buf), "%llu",
     static_cast<unsigned long long>(operator T()));
  DBUG_ASSERT(len > 0);
  my_b_write(file, reinterpret_cast<const uchar *>(buf), len);
}

/// @name Explicit template instantiations
///@{
template struct Info_file::Value<uint32_t>;
template struct Info_file::Value<uint64_t>;
///@}


bool Info_file::String_value::load_from(IO_CACHE *file)
{
  size_t strlen= my_b_gets(file, str, length);
  if (!strlen) // EOF
    return true;
  Value<const char *>::operator=(str); // unflag @ref is_default
  /// If we stopped on a newline, kill it.
  char &last_char= str[strlen-1];
  if (last_char == '\n')
  {
    last_char= '\0';
    return false;
  }
  /*
    Consume the lost line break,
    or error if the line overflows the @ref str.
  */
  return my_b_get(file) != '\n';
}

template<> void Info_file::Value<const char *>::save_to(IO_CACHE *file)
{
  const char *str= *this;
  my_b_write(file, reinterpret_cast<const uchar *>(str), strlen(str));
}

Info_file::String_value &Info_file::String_value::operator=(const char *other)
{
  if (other)
  {
    strmake(str, other, length-1);
    Value<const char *>::operator=(str); // unflag @ref is_default
  }
  else
    set_default();
  return *this;
}


/// @return `true` if the line is `0` or `1`, `false` otherwise or on error
template<> bool Info_file::Value<bool>::load_from(IO_CACHE *file)
{
  /** Only three chars are required:
    * One digit
      (When base prefixes are not recognized in integer parsing,
      anything with a leading `0` stops parsing
      after converting the `0` to zero anyway.)
    * the terminating `\n\0` that my_b_gets() includes
  */
  char buf[3];
  if (my_b_gets(file, buf, 3) && buf[1] == '\n')
    switch (buf[0]) {
    case '0':
      (*this)= false;
      return false;
    case '1':
      (*this)= true;
      return false;
    }
  return true;
}

template<> void Info_file::Value<bool>::save_to(IO_CACHE *file)
{ my_b_write_byte(file, operator bool() ? '1' : '0'); }


template<> bool Info_file::Value<DYNAMIC_ARRAY *>::load_from(IO_CACHE *file)
{
  long count;
  size_t i;
  /// +1 for the terminating delimiter
  char buf[Value<uint32_t>::MAX_CHARS10 + 1];
  DBUG_ASSERT(!is_default());
  if (is_default())
    return true;

  /*TODO:
    merge this procedure into my_b_gets();
    same for the copy in the `while` loop below
  */
  for (i= 0; i < sizeof(buf); ++i)
  {
    int c= my_b_get(file);
    if (c == my_b_EOF)
      return true;
    buf[i]= static_cast<char>(c);
    if (c == /* End of Line */ '\n' || c == /* End of Count */ ' ')
      break;
  }

  char *end= str2int(buf, 10, 1, INT32_MAX, &count);
  // Reserve enough elements ahead of time.
  if (!end || allocate_dynamic(value, count))
    return true;
  while (count--)
  {
    long value;
    /*
      Check that the previous number ended with a ` `,
      not `\n` or anything else.
    */
    if (*end != ' ')
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

    end= str2int(buf, 10, 1, INT32_MAX, &value);
    if (!end)
      return true;
    ulong id= value;
    bool oom= insert_dynamic(this->value, (uchar *)&id);
    /*
      This should not err because enough
      memory was already allocate_dynamic()-ed.
    */
    DBUG_ASSERT(!oom);
    if (oom)
      return true;
  }
  // Check that the last number ended with a `\n`, not ` ` or anything else.
  if (*end != '\n')
    return true;
  sort_dynamic(value, change_master_id_cmp); // to be safe
  return false;
}

/// Store the total number of elements followed by the individual elements.
template<> void Info_file::Value<DYNAMIC_ARRAY *>::save_to(IO_CACHE *file)
{
  DYNAMIC_ARRAY *array= *this;
  Value<ulong> writer= static_cast<ulong>(array->elements);
  writer.save_to(file);
  for (size_t i= 0; i < array->elements; ++i)
  {
    ulong id;
    get_dynamic(array, &id, i);
    writer= std::move(id);
    my_b_write_byte(file, ' ');
    writer.save_to(file);
  }
}


Master_info_file::Use_gtid_value::operator enum_master_use_gtid()
{
  enum_master_use_gtid mode=
    Value<enum_master_use_gtid>::operator enum_master_use_gtid();
  return mode == enum_master_use_gtid::AUTO ? (gtid_supported ?
    enum_master_use_gtid::SLAVE_POS : enum_master_use_gtid::NO
  ) : mode;
}

/** @return
  `true` if the line is a @ref enum_master_use_gtid,
  `false` otherwise or on error
*/
template<>
bool Info_file::Value<enum_master_use_gtid>::load_from(IO_CACHE *file)
{
  /**
    Only 3 chars are required for the enum,
    similar to Value<bool>::load_from().
  */
  char buf[3];
  if (!my_b_gets(file, buf, 3) ||
      buf[1] != '\n' ||
      buf[0] > /* SLAVE_POS */ '2' || buf[0] < /* NO */ '0')
    return true;
  (*this)= static_cast<enum_master_use_gtid>(buf[0] - '0');
  return false;
}

template<> void Info_file::Value<enum_master_use_gtid>::save_to(IO_CACHE *file)
{
  my_b_write_byte(file, static_cast<uchar>(
    '0' + static_cast<unsigned char>(operator enum_master_use_gtid())));
}


Uint32_3 Master_info_file::Heartbeat_period_value::get_default()
{
  if (::master_heartbeat_period_str != autoset_my_option)
    return ::master_heartbeat_period;
  uint64_t frequency_2= slave_net_timeout;
  frequency_2 *= 1000 / 2;
  return static_cast<uint32_t>(MY_MIN(frequency_2, UINT32_MAX));
}

Uint32_3::conversion_status Uint32_3::from_decimal(const decimal_t &decimal)
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
    The ideal implementation would work with native `double`s as they are
    sufficiently precise in this case; but decimal2double() is currently
    implemented by printing into a string and parsing that char array,
    which is an even larger overhead than `my_decimal` multiplication.
  */
  static const auto
    MAX_PERIOD= Decimal_from_str(STRING_WITH_LEN(Uint32_3::MAX_STR)),
    THOUSAND  = Decimal_from_str(STRING_WITH_LEN("1000"));

  ulonglong decimal_out;
  if (decimal.sign || decimal_cmp(&MAX_PERIOD, &decimal) < 0)
    return FAILED;
  bool overprecise= decimal.frac > 3;
  // decomposed from my_decimal2int() to reduce a bit of computations
  my_decimal product;
  bool unexpected_error=
    decimal_mul(&decimal, &THOUSAND, &product) ||
    (decimal2ulonglong(&product, &decimal_out, HALF_UP) > E_DEC_TRUNCATED);
  DBUG_ASSERT(!unexpected_error);
  if (unexpected_error)
    return FAILED;
  (*this)= static_cast<uint32_t>(decimal_out);
  return overprecise ? ROUNDED : OK;
}

Uint32_3::conversion_status
Uint32_3::from_chars(const LEX_CSTRING &str, char delimiter)
{
  my_decimal decimal;
  const char *str_end= &(str.str[str.length]);
  return (str2my_decimal(
    E_DEC_ERROR, str.str, &decimal, const_cast<char **>(&str_end)
  ) || *str_end != delimiter) ? FAILED : from_decimal(decimal);
}

template<> bool Info_file::Value<Uint32_3>::load_from(IO_CACHE *file)
{
  Uint32_3 decimal;
  /**
    +3 for the decimal point and the
    terminating `\n\0` that my_b_gets() includes.
    Having more precision than `DECIMAL(10,3)` is considered a mistake here,
    so we only need to verify the '\n'; no need to leave room for extra digits,
    which there should not be unless the file is edited externally.
  */
  char buf[Value<uint32_t>::MAX_CHARS10 + 3];
  size_t length= my_b_gets(file, buf, sizeof(buf));
  if (!length || decimal.from_chars({buf, length}, '\n'))
    return true;
  (*this)= std::move(decimal);
  return false;
}

/**
  This method is engineered (that is, hard-coded) to take
  full advantage of the non-negative `DECIMAL(10,3)` format.
*/
template<> void Info_file::Value<Uint32_3>::save_to(IO_CACHE *file)
{
  auto[integer_part, decimal_part]= div(operator Uint32_3(), 1000);
  my_b_printf(file, "%u.%03u", integer_part, decimal_part);
}


/**
  Guard agaist extra left-overs at the end of file in case a later update
  causes the effective content to shrink compared to earlier contents
*/
static constexpr LEX_CSTRING END_MARKER= "END_MARKER"_LEX_CSTRING;

bool Master_info_file::get_values(get_values_impl Info_file::*implementation)
{
  Value_interface *mysql_lines[
    /*
      Reserve extra (ignored) MySQL lines for interoperability with MySQL;
      MariaDB before 12.3 also only recognizes the
      key-value section if the line count is at least 33.
    */
    33
  ]= {
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
    nullptr, // &master_bind, // MDEV-19248
    &ignore_server_ids,
    nullptr, // MySQL `master_uuid`, which MariaDB ignores.
    &master_retry_count,
    &master_ssl_crl,
    &master_ssl_crlpath
    // master_auto_position
    // FOR CHANNEL
    // master_tls_version
    // master_public_key_path
    // get_master_public_key
    // network_namespace
    // master_compression_algorithms
    // master_zstd_compression_level
    // master_tls_ciphersuites
    // source_connection_auto_failover
    // gtid_only
  };

  KV key_values[]= {
    /*
      These are here to annotate whether they are `DEFAULT`.
      They are repeated from @ref VALUE_LIST to enable bidirectional
      compatibility with MySQL and earlier versions of MariaDB
      (where unrecognized keys, such as those from the future, are ignored).
    */
    {"connect_retry"_LEX_CSTRING, &master_connect_retry},
    {"ssl"_LEX_CSTRING, &master_ssl},
    {"ssl_ca"_LEX_CSTRING, &master_ssl_ca},
    {"ssl_capath"_LEX_CSTRING, &master_ssl_capath},
    {"ssl_cert"_LEX_CSTRING, &master_ssl_cert},
    {"ssl_cipher"_LEX_CSTRING, &master_ssl_cipher},
    {"ssl_key"_LEX_CSTRING, &master_ssl_key},
    {"ssl_crl"_LEX_CSTRING, &master_ssl_crl},
    {"ssl_crlpath"_LEX_CSTRING, &master_ssl_crlpath},
    {"ssl_verify_server_cert"_LEX_CSTRING, &master_ssl_verify_server_cert},
    {"heartbeat_period"_LEX_CSTRING, &master_heartbeat_period},
    {"retry_count"_LEX_CSTRING, &master_retry_count},
    // These are the ones new in MariaDB.
    {"using_gtid"_LEX_CSTRING, &master_use_gtid},
    {"do_domain_ids"_LEX_CSTRING, &do_domain_ids},
    {"ignore_domain_ids"_LEX_CSTRING, &ignore_domain_ids},
  };
  return (this->*implementation)(mysql_lines, array_elements(mysql_lines),
    key_values, array_elements(key_values));
}

bool Relay_log_info_file::get_values(get_values_impl Info_file::*implementation)
{
  Value_interface *mysql_lines[]= {
    &relay_log_file,
    &relay_log_pos,
    &read_master_log_file,
    &read_master_log_pos,
    &sql_delay
  };
  return (this->*implementation)(mysql_lines, array_elements(mysql_lines),
    /* none */ nullptr, 0);
}


Master_info_file::Master_info_file(
  DYNAMIC_ARRAY &ignore_server_ids,
  DYNAMIC_ARRAY &do_domain_ids, DYNAMIC_ARRAY &ignore_domain_ids
):
  ignore_server_ids(&ignore_server_ids),
  do_domain_ids(&do_domain_ids), ignore_domain_ids(&ignore_domain_ids)
{}


const uchar *Info_file::KV::my_hash_get_key(
  const void *ptr, size_t *r_size, [[maybe_unused]] my_bool)
{
  const LEX_CSTRING &key= static_cast<const KV *>(ptr)->key;
  *r_size= key.length;
  return reinterpret_cast<const uchar *>(key.str);
}

bool Info_file::load_from_file_impl(
  Value_interface **mysql_lines, size_t n_mysql_lines,
  KV *key_values, size_t n_key_values)
{
  Value<uint32_t> line_count_value= 1;
  if (line_count_value.load_from(&file))
    return true;
  uint32_t line_count= std::move(line_count_value);
  for (uint32_t i= 0; i < line_count; ++i)
  {

    int c;
    if (i < n_mysql_lines) // line known in the `value_list`
    {
      if (Value_interface *value= mysql_lines[i])
      {
        if (value->load_from(&file))
          return true;
        continue;
      }
    }
    /*
      Count and discard unrecognized lines.
      This is especially to prepare for @ref Master_info_file for MariaDB 10.0+,
      which reserves a bunch of lines before its unique `key=value` section
      to accomodate any future line-based (old-style) additions in MySQL.
      (This will make moving from MariaDB to MySQL easier by not
      requiring MySQL to recognize MariaDB `key=value` lines.)
    */
    while ((c= my_b_get(&file)) != '\n')
      if (c == my_b_EOF)
        return true; // EOF already?

  }
  /// Repurpose the trailing `\0` spot to prepare for the `=` or `\n`
  static constexpr size_t LONGEST_KEY_SIZE= sizeof("ssl_verify_server_cert");
  /*
    Proceed with `key=value` lines for MariaDB 10.0 and above:
    The "value" can then be read individually after consuming the`key=`.
  */
  /**
    MariaDB 10.0 does not have the `END_MARKER` before any left-overs at
    the end of the file, so ignore any non-first occurrences of a key.
    @note
      This set only "contains" the static strings of @ref MASTER_VALUE_MAP's keys,
      which means it can simply compare pointers by face values rather than
      their pointed content, in contrast with how `HASH` of `include/hash.h`
      is designed for string contents in a specified charset.
  */
  // C++ default allocator to match that `mysql_execute_command()` uses `new`
  auto map= Hash_set<KV>(PSI_INSTRUMENT_ME, KV::my_hash_get_key);
  constexpr KV END_MARK= {END_MARKER, nullptr};
  if (map.insert(&END_MARK))
    return true;
  while (n_key_values--)
    if (map.insert(&(key_values[n_key_values])))
      return true;
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
        KV *kv= map.find(
          key,
          i // size = exclusive end index of the string
        );
        // The "unknown" lines would be ignored to facilitate downgrades.
        if (kv) // found
        {
          // Compare the underlying string directly, no need to do char-by-char.
          if (kv->key.str == END_MARKER.str)
            return false;
          /*
            MariaDB 10.0 does not have the `END_MARKER` before any left-overs at
            the end of the file, so ignore any non-first occurrences of a key.
          */
          [[maybe_unused]] bool not_found= map.remove(kv);
          DBUG_ASSERT(!not_found);
          if (Value_interface *value= kv->value)
          {
            if (found_equal ? value->load_from(&file) : value->set_default())
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


bool Info_file::save_to_file_impl(
  Value_interface **mysql_lines, size_t n_mysql_lines,
  KV *key_values, size_t n_key_values)
{
  my_b_seek(&file, 0);

  /*
    If the new contents take less space than the previous file contents,
    then this code would write the file with unerased trailing garbage lines.
    But these garbage don't matter thanks to the number
    of effective lines in the first line of the file.
  */
  Value<size_t>(n_mysql_lines).save_to(&file);
  my_b_write_byte(&file, '\n');
  for (size_t i= 0; i < n_mysql_lines; ++i)
  {
    if (Value_interface *value= mysql_lines[i])
      value->save_to(&file);
    my_b_write_byte(&file, '\n');
  }

  /* Write MariaDB `key=value` lines:
    The "value" can then be written individually after generating the`key=`.
    (Note that there is no strict requirement to follow a certain order.)
  */
  for (size_t i= 0; i < n_key_values; ++i)
  {
    KV &kv= key_values[i];
    if (Value_interface *value= kv.value)
    {
      my_b_write(&file,
                  reinterpret_cast<const uchar *>(kv.key.str), kv.key.length);
      if (!value->is_default())
      {
        my_b_write_byte(&file, '=');
        value->save_to(&file);
      }
      my_b_write_byte(&file, '\n');
    }
  }
  my_b_write(&file, reinterpret_cast<const uchar *>(END_MARKER.str),
              END_MARKER.length);
  my_b_write_byte(&file, '\n');

  return false;
}

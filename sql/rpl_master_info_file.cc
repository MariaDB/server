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

#include "mariadb.h"
#include "rpl_master_info_file.h"
#include <m_string.h>   // strmake, my_strtod
#include <mysqld_error.h> // Error numbers


const char *master_use_gtid_names[]=
  {"No", "Current_Pos", "Slave_Pos", nullptr};


/* ===== Uint_value_with_default ===== */

bool Master_info_file::Uint_value_with_default::load_from(IO_CACHE *file)
{
  ulonglong tmp;
  if (uint_from_chars(file, UINT_MAX32, &tmp))
  {
    set_default();
    return true;
  }
  value= (uint) tmp;
  has_value= true;
  return false;
}


void Master_info_file::Uint_value_with_default::save_to(IO_CACHE *file)
{
  uint_to_chars(file, value);
}


/* ===== Ulonglong_value_with_default ===== */

bool Master_info_file::Ulonglong_value_with_default::load_from(IO_CACHE *file)
{
  if (uint_from_chars(file, ULONGLONG_MAX, &value))
  {
    set_default();
    return true;
  }
  has_value= true;
  return false;
}


void Master_info_file::Ulonglong_value_with_default::save_to(IO_CACHE *file)
{
  uint_to_chars(file, value);
}


/* ===== Path_value ===== */

void Master_info_file::Path_value::operator=(const char *other)
{
  DBUG_ASSERT(other);
  strmake(buf, other, sizeof(buf) - 1);
  has_value= true;
}


bool Master_info_file::Path_value::set_default()
{
  const char *default_str= *default_value ? *default_value : "";
  strmake(buf, default_str, sizeof(buf) - 1);
  has_value= false;
  return false;
}


bool Master_info_file::Path_value::load_from(IO_CACHE *file)
{
  size_t length= my_b_gets(file, buf, sizeof(buf));
  if (!length)
  {
    set_default();
    return true;
  }
  char *last_char= buf + length - 1;
  if (*last_char == '\n')
  {
    *last_char= '\0';
    has_value= true;
    return false;
  }
  if (my_b_get(file) != '\n')
  {
    set_default();
    return true;
  }
  has_value= true;
  return false;
}


void Master_info_file::Path_value::save_to(IO_CACHE *file)
{
  my_b_write(file, (const uchar *) buf, strlen(buf));
}


/* ===== Bool_value_with_default ===== */

bool Master_info_file::Bool_value_with_default::load_from(IO_CACHE *file)
{
  /*
    Only three chars are required: one digit ('0' or '1'), the terminating
    '\n', and the '\0' that my_b_gets() appends.
  */
  char buf[3];
  if (my_b_gets(file, buf, sizeof(buf)) && buf[1] == '\n')
  {
    switch (buf[0])
    {
    case '0': value= 0; has_value= true; return false;
    case '1': value= 1; has_value= true; return false;
    }
  }
  set_default();
  return true;
}


void Master_info_file::Bool_value_with_default::save_to(IO_CACHE *file)
{
  my_b_write_byte(file, value ? '1' : '0');
}


/* ===== ID_array_value ===== */

bool Master_info_file::ID_array_value::load_from(IO_CACHE *file)
{
  long count;
  size_t i;
  /* +1 is to store the newline */
  char buf[INT_BUFFER_SIZE+1];
  buf[INT_BUFFER_SIZE]= 0;

  for (i= 0; i < sizeof(buf)-1; ++i)
  {
    int chr= my_b_get(file);
    if (chr == my_b_EOF)
      return true;
    buf[i]= (char) chr;
    if (chr == '\n' || chr == ' ')
      break;
  }
  char *end= str2int(buf, i, 10, &count);
  if (!end || allocate_dynamic(array, count))
    return true;
  while (count--)
  {
    long parsed;
    ulong id;
    bool oom;
    /*
      Check that the previous number ended with a ' ', not '\n' or
      anything else.
    */
    if (*end != ' ')
      return true;
    for (i= 0; i < sizeof(buf)-1; ++i)
    {
      int chr= my_b_get(file);
      if (chr == my_b_EOF)
        return true;
      buf[i]= (char) chr;
      if (chr == ' ' || chr == '\n')
        break;
    }
    end= str2int(buf, i, 10, &parsed);
    if (!end || parsed == 0)
      return true;
    id= parsed;
    oom= insert_dynamic(array, (uchar *) &id);
    DBUG_ASSERT(!oom);
    if (oom)
      return true;
  }
  /* Check that the last number ended with '\n', not ' '. */
  if (*end != '\n')
    return true;
  sort_dynamic(array, change_master_id_cmp);
  return false;
}


void Master_info_file::ID_array_value::save_to(IO_CACHE *file)
{
  uint_to_chars(file, array->elements);
  for (size_t i= 0; i < array->elements; ++i)
  {
    ulong id;
    get_dynamic(array, (uchar *) &id, i);
    my_b_write_byte(file, ' ');
    uint_to_chars(file, id);
  }
}


/* ===== Heartbeat_period_value ===== */

uint Master_info_file::Heartbeat_period_value::from_decimal(
  uint32 *result, const decimal_t *decimal, bool *overprecise)
{
  /* Wrapper to enable only-once static const construction. */
  struct Decimal_from_str: my_decimal
  {
    Decimal_from_str(const char *str, size_t length): my_decimal()
    {
      const char *end= str + length;
      int err __attribute__((unused))= str2my_decimal(
        E_DEC_ERROR, str, this, const_cast<char **>(&end));
      DBUG_ASSERT(!err && !*end);
    }
  };
  static const Decimal_from_str MAX_PERIOD(STRING_WITH_LEN(MAX));
  static const Decimal_from_str THOUSAND(STRING_WITH_LEN("1000"));

  if (decimal->sign || decimal_cmp(&MAX_PERIOD, decimal) < 0)
    return true; /* ER_SLAVE_HEARTBEAT_VALUE_OUT_OF_RANGE */
  *overprecise= decimal->frac > 3;
  /* Decomposed from my_decimal2int() to reduce computations. */
  my_decimal product;
  ulonglong decimal_out;
  bool err __attribute__((unused))=
    decimal_mul(decimal, &THOUSAND, &product) ||
    (decimal2ulonglong(&product, &decimal_out, HALF_UP) > E_DEC_TRUNCATED);
  DBUG_ASSERT(!err);
  if (err)
    return true;
  *result= (uint32) decimal_out;
  return false;
}


bool Master_info_file::Heartbeat_period_value::load_from(IO_CACHE *file)
{
  /*
    Max textual width is ~12 chars ("4294967.295" + '\n' + '\0'). Round
    up a bit; the parser ignores trailing precision.
  */
  char buf[24];
  if (!my_b_gets(file, buf, sizeof(buf)))
  {
    set_default();
    return true;
  }
  char *endptr= buf + strlen(buf);
  int error= 0;
  double seconds= my_strtod(buf, &endptr, &error);
  /* Max valid: UINT_MAX32 milliseconds = UINT_MAX32 / 1000 seconds. */
  if (error || seconds < 0 || seconds > (double) UINT_MAX32 / 1000 ||
      *endptr != '\n')
  {
    set_default();
    return true;
  }
  value= (uint32) (seconds * 1000);
  has_value= true;
  return false;
}


void Master_info_file::Heartbeat_period_value::save_to(IO_CACHE *file)
{
  /*
    Get the use set value or the default value which is calculated
    based on other values. The value is in milliseconds.
    Output format as seconds.milliseconds.
  */
  uint32 ms= get_value();
  uint32 whole= ms / 1000;
  uint32 frac=  ms % 1000;
  my_b_printf(file, "%u.%03u", whole, frac);
}


/* ===== Master_use_gtid_value ===== */

bool Master_info_file::Master_use_gtid_value::load_from(IO_CACHE *file)
{
  /* Only 3 chars required for the digit + '\n' + '\0'. */
  char buf[3];
  if (!my_b_gets(file, buf, sizeof(buf)) ||
      buf[1] != '\n' ||
      buf[0] > /* SLAVE_POS */ '2' || buf[0] < /* NO */ '0')
  {
    set_default();
    return true;
  }
  active_mode= user_mode= (enum_master_use_gtid) (buf[0] - '0');
  has_value= true;
  return false;
}


void Master_info_file::Master_use_gtid_value::save_to(IO_CACHE *file)
{
  enum_master_use_gtid effective= user_mode;
  my_b_write_byte(file, (uchar) ('0' + (unsigned char) effective));
}


/* ===== Master_info_file ===== */

/*
  Populated at first instance construction from value_map[]->variable_name
  (plus END_MARKER and the trailing NullS). The +1 is for END_MARKER
  (NULL pointer) at the end of the array.
*/
const char *Master_info_file::value_map_keys[
  array_elements(((Master_info_file *) nullptr)->value_map) + 1];

TYPELIB Master_info_file::value_map_typelib;


Master_info_file::Master_info_file(DYNAMIC_ARRAY *ignore_server_ids,
                                   DYNAMIC_ARRAY *do_domain_ids,
                                   DYNAMIC_ARRAY *ignore_domain_ids):
  ignore_server_ids("ignore_server_ids", ignore_server_ids),
  do_domain_ids("do_domain_ids", do_domain_ids),
  ignore_domain_ids("ignore_domain_ids", ignore_domain_ids)
{
  /*
    Populate the static value_map_keys[] and value_map_typelib from
    value_map[]->variable_name on first construction. The writes are
    idempotent (every instance produces the same pointer values), so
    no once-guard is needed.
  */
  Value *const *p;
  const char **key= value_map_keys;
  for (p= value_map; *p; ++p, ++key)
    *key= (*p)->variable_name;
  *key++= END_MARKER;
  *key= NullS;
  value_map_typelib= CREATE_TYPELIB_FOR(value_map_keys);
  /*
    value_map ends with a nullptr (the END_MARKER slot), which terminates
    the loop without having to know the array size.
  */
  for (p= value_map; *p; ++p)
    (*p)->set_default();
}


bool Master_info_file::load_from_file()
{
  /*
    Keys come from value_map_keys, the longest of which is currently
    23 chars ("ssl_verify_server_cert"). Use 80 so the file format can
    accept longer key names from future versions without recompilation.
  */
  static constexpr size_t LONGEST_KEY_SIZE= 80;
  if (Info_file::load_from_file(value_list,
                                array_elements(value_list),
                                /* MASTER_CONNECT_RETRY */ 7, 1))
    return true;
  /*
    Info_file::load_from_file() handles only fixed-position entries.
    Proceed with key=value lines for MariaDB 10.0 and above. The "value"
    can be read individually after consuming the "key=".

    MariaDB 10.0 does not have the END_MARKER before any left-overs at
    the end of the file, so ignore any non-first occurrences of a key.
    A bit in seen tracks which value_map positions have been processed,
    indexed by typelib position. uint32 is enough for value_map's
    current 16 entries.
  */
  static_assert(array_elements(((Master_info_file *) nullptr)->value_map)
                <= 32,
                "value_map outgrew the uint32 seen bitmask");
  uint32 seen= 0;
  while (true)
  {
    /*
      A key=value line might not have the =value part; in that case the
      value is set_default().
    */
    bool found_equal= false;
    char key[LONGEST_KEY_SIZE];
    for (size_t i= 0; i < LONGEST_KEY_SIZE; ++i)
    {
      int chr= my_b_get(&file);
      switch (chr)
      {
      case my_b_EOF:
        /* OK if no chars were read, or error if the line hits EOF. */
        return i;
      case '=':
        found_equal= true;
        /* fallthrough */
      case '\n':
      {
        /*
          find_type() expects a '\0'-terminated string; the buffer has
          space because LONGEST_KEY_SIZE > any real key length.
        */
        key[i]= '\0';
        int idx_1based= find_type(key, &value_map_typelib,
                                  FIND_TYPE_NO_PREFIX);
        /* Unknown lines are ignored to facilitate downgrades. */
        if (idx_1based > 0)
        {
          size_t idx= idx_1based - 1;
          const char *kv_key= value_map_keys[idx];
          if (kv_key == END_MARKER)
            return false;
          uint32 bit= 1U << idx;
          if (!(seen & bit))
          {
            seen|= bit;
            my_off_t filepos= my_b_tell(&file);
            Value *entry= value_map[idx];
            DBUG_ASSERT(entry);
            if (found_equal ? entry->load_from(&file) : entry->set_default())
            {
              if (!(reinit_io_cache(&file, READ_CACHE, filepos, 0, 0)))
              {
                char buf[160];
                buf[0]= 0;
                size_t length= my_b_gets(&file, buf, sizeof(buf));
                /* Strip the trailing '\n' that my_b_gets() includes. */
                if (length && buf[length - 1] == '\n')
                  buf[length - 1]= '\0';
                my_error(ER_WRONG_VALUE, MYF(ME_FATAL | ME_ERROR_LOG),
                         entry->variable_name,
                         found_equal ? buf : "default");
              }
              return true;
            }
          }
        }
        goto break_for;
      }
      default:
        key[i]= (char) chr;
      }
    }
break_for:;
  }
}


void Master_info_file::save_to_file()
{
  /* Write the line-based section with reservations for MySQL additions. */
  Info_file::save_to_file(value_list, array_elements(value_list), 33);
  /*
    Write MariaDB key=value lines. value_map ends with a nullptr
    (END_MARKER slot); iterate in parallel with value_map_keys.
  */
  const char *const *k= value_map_keys;
  for (Value *const *p= value_map; *p; ++p, ++k)
  {
    Value *value= *p;
    const char *key= *k;
    my_b_write(&file, (const uchar *) key, strlen(key));
    if (!value->is_default())
    {
      my_b_write_byte(&file, '=');
      value->save_to(&file);
    }
    my_b_write_byte(&file, '\n');
  }
  my_b_write(&file, (const uchar *) END_MARKER, sizeof(END_MARKER) - 1);
  my_b_write_byte(&file, '\n');
}

/* Copyright (c) 2026, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#define MYSQL_SERVER
#include "mariadb.h"
#include "sql_class.h"
#include "item_hllfunc.h"
#include "sql_type_hll.h"
#include "hll_codec.h"
#include <cmath>

#define XXH_INLINE_ALL
#include "../../mysys/xxhash.h"

static constexpr uint64 HLL_HASH_SEED_V1= 0;

/* Type tags of hash contract v1 (plan section 6.1) */
enum Hll_tag : uchar
{
  HLL_TAG_NONNEG_INT= 0x01, HLL_TAG_NEG_INT= 0x02, HLL_TAG_BINARY= 0x03,
  HLL_TAG_FLOAT= 0x05, HLL_TAG_DOUBLE= 0x06,
  HLL_TAG_DATE= 0x08, HLL_TAG_TIME= 0x09, HLL_TAG_DATETIME= 0x0a,
  HLL_TAG_TIMESTAMP= 0x0b, HLL_TAG_YEAR= 0x0c,
  HLL_TAG_UUID= 0x10, HLL_TAG_INET4= 0x11, HLL_TAG_INET6= 0x12
};


static void store_be(uchar *to, uint64 v, uint bytes)
{
  for (int i= (int) bytes - 1; i >= 0; i--, v>>= 8)
    to[i]= (uchar) v;
}


Hll_domain hll_input_domain(const Item *item)
{
  const Type_handler *th= item->real_type_handler();
  if (!th->is_traditional_scalar_type())
  {
    /* Only pluggable types with an explicit adapter */
    if (th->name().eq("uuid"_LEX_CSTRING))
      return Hll_domain::UUID;
    if (th->name().eq("inet4"_LEX_CSTRING))
      return Hll_domain::INET4;
    if (th->name().eq("inet6"_LEX_CSTRING))
      return Hll_domain::INET6;
    return Hll_domain::UNSUPPORTED;
  }
  switch (th->cmp_type()) {
  case INT_RESULT:
    if (th->real_field_type() == MYSQL_TYPE_YEAR)
      return Hll_domain::YEAR;
    if (th->real_field_type() == MYSQL_TYPE_BIT)
      return Hll_domain::UNSUPPORTED;
    return Hll_domain::INTEGER;
  case REAL_RESULT:
    return th->real_field_type() == MYSQL_TYPE_FLOAT ? Hll_domain::FLOAT :
                                                       Hll_domain::DOUBLE;
  case TIME_RESULT:
    switch (th->field_type()) {
    case MYSQL_TYPE_DATE:      return Hll_domain::DATE;
    case MYSQL_TYPE_TIME:      return Hll_domain::TIME;
    case MYSQL_TYPE_DATETIME:  return Hll_domain::DATETIME;
    case MYSQL_TYPE_TIMESTAMP: return Hll_domain::TIMESTAMP;
    default:                   return Hll_domain::UNSUPPORTED;
    }
  case STRING_RESULT:
    switch (th->real_field_type()) {
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
      /* Collatable text has no stable byte form: require CAST AS BINARY */
      return item->collation.collation == &my_charset_bin ?
             Hll_domain::BINARY : Hll_domain::UNSUPPORTED;
    default:
      return Hll_domain::UNSUPPORTED;
    }
  case DECIMAL_RESULT:
  case ROW_RESULT:
    break;
  }
  return Hll_domain::UNSUPPORTED;
}


/* Packed temporal value; the formula of the contract, not pack_time() */
static longlong hll_packed_time(const MYSQL_TIME *t)
{
  longlong p= ((((((longlong) t->year * 13 + t->month) * 32 + t->day) * 24 +
                 t->hour) * 60 + t->minute) * 60 + t->second) * 1000000LL +
              t->second_part;
  return t->neg ? -p : p;
}


static bool parse_uuid_text(const String *s, uchar *to)
{
  uint n= 0;
  for (size_t i= 0; i < s->length(); i++)
  {
    int c= s->ptr()[i], v;
    if (c == '-')
      continue;
    if (c >= '0' && c <= '9') v= c - '0';
    else if (c >= 'a' && c <= 'f') v= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v= c - 'A' + 10;
    else return true;
    if (n >= 32)
      return true;
    to[n / 2]= (uchar) ((n % 2) ? (to[n / 2] | v) : (v << 4));
    n++;
  }
  return n != 32;
}


bool hll_hash_item(THD *thd, Item *item, Hll_domain domain, String *buf,
                   uint64 *hash, bool *null)
{
  /* Fixed temporal flags: the hash must not depend on the session sql_mode */
  static const date_conv_mode_t temporal_flags= TIME_INVALID_DATES;
  uchar val[1 + 16];
  size_t len= 0;
  *null= false;

  switch (domain) {
  case Hll_domain::UNSUPPORTED:
    DBUG_ASSERT(0);
    return true;

  case Hll_domain::INTEGER:
  {
    longlong v= item->val_int();
    if ((*null= item->null_value))
      return false;
    /* One domain for all integer types: only the value matters */
    val[0]= (item->unsigned_flag || v >= 0) ? HLL_TAG_NONNEG_INT :
                                              HLL_TAG_NEG_INT;
    store_be(val + 1, (uint64) v, 8);
    len= 9;
    break;
  }

  case Hll_domain::FLOAT:
  {
    float f= (float) item->val_real();
    if ((*null= item->null_value))
      return false;
    uint32 bits;
    if (std::isnan(f))
      bits= 0x7fc00000;
    else
    {
      if (f == 0)
        f= 0;                                   // -0 -> +0
      memcpy(&bits, &f, sizeof(bits));
    }
    val[0]= HLL_TAG_FLOAT;
    store_be(val + 1, bits, 4);
    len= 5;
    break;
  }

  case Hll_domain::DOUBLE:
  {
    double d= item->val_real();
    if ((*null= item->null_value))
      return false;
    uint64 bits;
    if (std::isnan(d))
      bits= 0x7ff8000000000000ULL;
    else
    {
      if (d == 0)
        d= 0;                                   // -0 -> +0
      memcpy(&bits, &d, sizeof(bits));
    }
    val[0]= HLL_TAG_DOUBLE;
    store_be(val + 1, bits, 8);
    len= 9;
    break;
  }

  case Hll_domain::BINARY:
  {
    String *s= item->val_str(buf);
    if ((*null= !s))
      return false;
    uchar head[9];
    head[0]= HLL_TAG_BINARY;
    store_be(head + 1, s->length(), 8);
    XXH3_state_t state;
    XXH3_64bits_reset_withSeed(&state, HLL_HASH_SEED_V1);
    XXH3_64bits_update(&state, head, sizeof(head));
    XXH3_64bits_update(&state, s->ptr(), s->length());
    *hash= XXH3_64bits_digest(&state);
    return false;
  }

  case Hll_domain::DATE:
  {
    Date d(thd, item, Date::Options(temporal_flags, TIME_FRAC_TRUNCATE));
    if ((*null= !d.is_valid_date()))
      return false;
    val[0]= HLL_TAG_DATE;
    store_be(val + 1, (uint64) hll_packed_time(d.get_mysql_time()), 8);
    len= 9;
    break;
  }

  case Hll_domain::TIME:
  {
    /* A valid Time has zero YYYYMMDD: days are already folded into hours */
    Time t(thd, item, Time::Options(temporal_flags, TIME_FRAC_TRUNCATE,
                                    Time::DATETIME_TO_TIME_DISALLOW));
    if ((*null= !t.is_valid_time()))
      return false;
    val[0]= HLL_TAG_TIME;
    store_be(val + 1, (uint64) hll_packed_time(t.get_mysql_time()), 8);
    len= 9;
    break;
  }

  case Hll_domain::DATETIME:
  {
    Datetime dt(thd, item, Datetime::Options(temporal_flags,
                                             TIME_FRAC_TRUNCATE));
    if ((*null= !dt.is_valid_datetime()))
      return false;
    val[0]= HLL_TAG_DATETIME;
    store_be(val + 1, (uint64) hll_packed_time(dt.get_mysql_time()), 8);
    len= 9;
    break;
  }

  case Hll_domain::TIMESTAMP:
  {
    /* Native value, not MYSQL_TIME: independent of @@time_zone */
    Timestamp_or_zero_datetime_native_null native(thd, item);
    if ((*null= native.is_null()))
      return false;
    uint64 sec= 0, usec= 0;
    if (!native.is_zero_datetime())
    {
      Timestamp ts(native);
      sec= (uint64) ts.tv_sec;
      usec= (uint64) ts.tv_usec;
    }
    val[0]= HLL_TAG_TIMESTAMP;
    val[1]= native.is_zero_datetime() ? 0 : 1;
    store_be(val + 2, sec, 8);
    store_be(val + 10, usec, 4);
    len= 14;
    break;
  }

  case Hll_domain::YEAR:
  {
    longlong v= item->val_int();
    if ((*null= item->null_value))
      return false;
    val[0]= HLL_TAG_YEAR;
    store_be(val + 1, (uint64) v, 2);
    len= 3;
    break;
  }

  case Hll_domain::UUID:
  {
    /* Canonical form is the RFC 4122 byte order of the text value */
    String *s= item->val_str(buf);
    if ((*null= !s))
      return false;
    val[0]= HLL_TAG_UUID;
    if (parse_uuid_text(s, val + 1))
    {
      my_error(ER_WRONG_VALUE, MYF(0), "UUID", ErrConvString(s).ptr());
      return true;
    }
    len= 17;
    break;
  }

  case Hll_domain::INET4:
  case Hll_domain::INET6:
  {
    /* INET4/INET6 native values are addresses in network byte order */
    NativeBuffer<16> native;
    if ((*null= item->val_native(thd, &native)))
      return false;
    size_t expected= domain == Hll_domain::INET4 ? 4 : 16;
    if (native.length() != expected)
    {
      my_error(ER_WRONG_VALUE, MYF(0), domain == Hll_domain::INET4 ?
               "INET4" : "INET6", "");
      return true;
    }
    val[0]= domain == Hll_domain::INET4 ? HLL_TAG_INET4 : HLL_TAG_INET6;
    memcpy(val + 1, native.ptr(), expected);
    len= 1 + expected;
    break;
  }
  }
  *hash= XXH3_64bits_withSeed(val, len, HLL_HASH_SEED_V1);
  return false;
}


/*****************************************************************/

static Hll_state *alloc_state(THD *thd)
{
  void *p= thd->alloc(sizeof(Hll_state));
  return p ? new (p) Hll_state() : NULL;
}


/*
  HLL functions accept HLL, NULL, or binary byte strings: user variables and
  statement parameters do not keep the HLL type (they become LONGBLOB).
  The bytes are validated by Hll_codec::decode() in every case.
*/
static bool check_hll_arg(const char *func_name, Item *arg)
{
  const Type_handler *th= arg->type_handler();
  if (th == &type_handler_hll || th == &type_handler_null ||
      (th->is_traditional_scalar_type() && th->cmp_type() == STRING_RESULT &&
       arg->collation.collation == &my_charset_bin))
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           th->name().ptr(), func_name);
  return true;
}


/*
  Read and decode an HLL argument.
  @param state  decoded value, or NULL to only validate
  @return       the serialized value; NULL for SQL NULL or on error
                (*error is set and the error is reported)
*/
static String *decode_arg(Item *arg, String *buf, Hll_state *state,
                          bool *error)
{
  *error= false;
  String *s= arg->val_str(buf);
  if (s && Hll_codec::decode((const uchar *) s->ptr(), s->length(), state))
  {
    my_error(ER_WRONG_VALUE, MYF(0), "HLL", ErrConvString(s).ptr());
    *error= true;
    return NULL;
  }
  return s;
}


static String *encode_state(const Hll_state &state, String *to)
{
  if (to->alloc(HLL_MAX_ENCODED_LENGTH))
    return NULL;
  to->length((uint32) Hll_codec::encode(state, HLL_FORMAT_V1,
                                        (uchar *) to->ptr()));
  to->set_charset(&my_charset_bin);
  return to;
}


static bool lazy_state(Hll_state **state)
{
  return !*state && !(*state= alloc_state(current_thd));
}


/*****************************************************************/

const Type_handler *Item_hll_func::type_handler() const
{
  return &type_handler_hll;
}


bool Item_hll_func::fix_length_and_dec(THD *thd)
{
  collation.set(&my_charset_bin, DERIVATION_IMPLICIT);
  max_length= HLL_MAX_ENCODED_LENGTH;
  return false;
}


bool Item_hll_func::val_native(THD *thd, Native *to)
{
  String *s= val_str(&str_value);
  return !s || to->copy(s->ptr(), s->length());
}


String *Item_func_hll_empty::val_str(String *to)
{
  null_value= false;
  static const char empty= 0x11;
  to->copy(&empty, 1, &my_charset_bin);
  return to;
}


bool Item_func_hll_union::fix_length_and_dec(THD *thd)
{
  set_maybe_null();
  return check_hll_arg("hll_union", args[0]) ||
         check_hll_arg("hll_union", args[1]) ||
         Item_hll_func::fix_length_and_dec(thd);
}


String *Item_func_hll_union::val_str(String *to)
{
  bool error;
  if (lazy_state(&m_state[0]) || lazy_state(&m_state[1]) ||
      !decode_arg(args[0], &m_buf[0], m_state[0], &error) ||
      !decode_arg(args[1], &m_buf[1], m_state[1], &error))
  {
    null_value= true;
    return NULL;
  }
  m_state[0]->merge(*m_state[1]);
  null_value= false;
  return encode_state(*m_state[0], to);
}


void Item_func_hll_union::cleanup()
{
  m_state[0]= m_state[1]= NULL;
  Item_hll_func::cleanup();
}


bool Item_func_hll_cardinality::fix_length_and_dec(THD *thd)
{
  set_maybe_null();
  return check_hll_arg("hll_cardinality", args[0]) ||
         Item_real_func::fix_length_and_dec(thd);
}


double Item_func_hll_cardinality::val_real()
{
  bool error;
  if (lazy_state(&m_state) || !decode_arg(args[0], &m_buf, m_state, &error))
  {
    null_value= true;
    return 0;
  }
  null_value= false;
  return m_state->cardinality();
}


void Item_func_hll_cardinality::cleanup()
{
  m_state= NULL;
  Item_real_func::cleanup();
}


bool Item_func_hll_cardinality_round::fix_length_and_dec(THD *thd)
{
  unsigned_flag= true;
  fix_char_length(20);
  set_maybe_null();
  return check_hll_arg("hll_cardinality_round", args[0]);
}


longlong Item_func_hll_cardinality_round::val_int()
{
  bool error;
  if (lazy_state(&m_state) || !decode_arg(args[0], &m_buf, m_state, &error))
  {
    null_value= true;
    return 0;
  }
  double r= std::floor(m_state->cardinality() + 0.5);
  /* 2^64 is the first double above the BIGINT UNSIGNED range */
  if (!std::isfinite(r) || r < 0 || r >= 18446744073709551616.0)
  {
    my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "BIGINT UNSIGNED",
             "hll_cardinality_round");
    null_value= true;
    return 0;
  }
  null_value= false;
  return (longlong) (ulonglong) r;
}


void Item_func_hll_cardinality_round::cleanup()
{
  m_state= NULL;
  Item_longlong_func::cleanup();
}


bool Item_func_hll_format_version::fix_length_and_dec(THD *thd)
{
  set_maybe_null();
  return check_hll_arg("hll_format_version", args[0]) ||
         Item_long_func::fix_length_and_dec(thd);
}


longlong Item_func_hll_format_version::val_int()
{
  bool error;
  String *s= decode_arg(args[0], &m_buf, NULL, &error);
  if ((null_value= !s))
    return 0;
  return (uchar) s->ptr()[0] >> 4;
}


bool Item_func_hll_info::fix_length_and_dec(THD *thd)
{
  set_maybe_null();
  collation.set(&my_charset_latin1, DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
  fix_char_length(64);
  return check_hll_arg("hll_info", args[0]);
}


String *Item_func_hll_info::val_str(String *to)
{
  bool error;
  String *s;
  if (lazy_state(&m_state) ||
      !(s= decode_arg(args[0], &m_buf, m_state, &error)))
  {
    null_value= true;
    return NULL;
  }
  char tmp[64];
  size_t len= 0;
  uint version= (uchar) s->ptr()[0] >> 4;
  switch (m_state->repr) {
  case Hll_state::EMPTY:
    len= my_snprintf(tmp, sizeof(tmp), "version=%u representation=EMPTY",
                     version);
    break;
  case Hll_state::EXPLICIT:
    len= my_snprintf(tmp, sizeof(tmp),
                     "version=%u representation=EXPLICIT elements=%u",
                     version, m_state->n_explicit);
    break;
  case Hll_state::FULL:
  {
    uint zeros= 0;
    for (uint i= 0; i < HLL_M; i++)
      zeros+= m_state->regs[i] == 0;
    len= my_snprintf(tmp, sizeof(tmp),
                     "version=%u representation=FULL zero_registers=%u",
                     version, zeros);
    break;
  }
  }
  null_value= false;
  to->copy(tmp, len, &my_charset_latin1);
  return to;
}


void Item_func_hll_info::cleanup()
{
  m_state= NULL;
  Item_str_func::cleanup();
}


/*****************************************************************/

const Type_handler *Item_sum_hll::type_handler() const
{
  return &type_handler_hll;
}


bool Item_sum_hll::fix_length_and_dec(THD *thd)
{
  collation.set(&my_charset_bin, DERIVATION_IMPLICIT);
  max_length= HLL_MAX_ENCODED_LENGTH;
  /* Empty and all-NULL input give HLL_EMPTY(), never NULL */
  null_value= false;
  return false;
}


bool Item_sum_hll::init_state()
{
  return lazy_state(&m_state);
}


void Item_sum_hll::clear()
{
  if (!init_state())
    m_state->clear();
}


void Item_sum_hll::cleanup()
{
  m_state= NULL;
  Item_sum_plugin::cleanup();
}


String *Item_sum_hll::val_str(String *to)
{
  endup();
  if (init_state())
  {
    null_value= true;
    return NULL;
  }
  null_value= false;
  return encode_state(*m_state, to);
}


bool Item_sum_hll::val_native(THD *thd, Native *to)
{
  String *s= val_str(&m_buf);
  return !s || to->copy(s->ptr(), s->length());
}


bool Item_sum_hll_build::fix_length_and_dec(THD *thd)
{
  if ((m_domain= hll_input_domain(args[0])) == Hll_domain::UNSUPPORTED)
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
             args[0]->type_handler()->name().ptr(), "hll_build_agg");
    return true;
  }
  return Item_sum_hll::fix_length_and_dec(thd);
}


bool Item_sum_hll_build::add_row()
{
  uint64 hash;
  bool null;
  if (init_state() ||
      hll_hash_item(current_thd, aggregation_arg(0), m_domain, &m_arg_buf,
                    &hash, &null))
    return true;
  if (!null)
    m_state->add(hash);
  return false;
}


bool Item_sum_hll_union::fix_length_and_dec(THD *thd)
{
  return check_hll_arg("hll_union_agg", args[0]) ||
         Item_sum_hll::fix_length_and_dec(thd);
}


bool Item_sum_hll_union::add_row()
{
  bool error;
  if (init_state() || lazy_state(&m_arg_state))
    return true;
  if (decode_arg(aggregation_arg(0), &m_buf, m_arg_state, &error))
    m_state->merge(*m_arg_state);
  return error;
}


void Item_sum_hll_union::cleanup()
{
  m_arg_state= NULL;
  Item_sum_hll::cleanup();
}

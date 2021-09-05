/*
   Copyright (c) 2000, 2017, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

/**
  @file

  @brief
  This file defines all string functions

  @warning
    Some string functions don't always put and end-null on a String.
    (This shouldn't be needed)
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"                          // HAVE_*

#include "sql_priv.h"
/*
  It is necessary to include set_var.h instead of item.h because there
  are dependencies on include order for set_var.h and item.h. This
  will be resolved later.
*/
#include "sql_class.h"                          // set_var.h: THD
#include "set_var.h"
#include "sql_base.h"
#include "sql_time.h"
#include "des_key_file.h"       // st_des_keyschedule, st_des_keyblock
#include "password.h"           // my_make_scrambled_password,
                                // my_make_scrambled_password_323
#include <m_ctype.h>
#include <my_md5.h>
C_MODE_START
#include "../mysys/my_static.h"			// For soundex_map
C_MODE_END
#include "sql_show.h"                           // append_identifier
#include <sql_repl.h>
#include "sql_statistics.h"

/* fmtlib include (https://fmt.dev/). */
#define FMT_STATIC_THOUSANDS_SEPARATOR ','
#define FMT_HEADER_ONLY 1
#include "fmt/format-inl.h"

size_t username_char_length= USERNAME_CHAR_LENGTH;

/*
  Calculate max length of string from length argument to LEFT and RIGHT
*/

static uint32 max_length_for_string(Item *item)
{
  ulonglong length= item->val_int();
  /* Note that if value is NULL, val_int() returned 0 */
  if (length > (ulonglong) INT_MAX32)
  {
    /* Limit string length to maxium string length in MariaDB (2G) */
    length= item->unsigned_flag ? (ulonglong) INT_MAX32 : 0;
  }
  return (uint32) length;
}


/*
  For the Items which have only val_str_ascii() method
  and don't have their own "native" val_str(),
  we provide a "wrapper" method to convert from ASCII
  to Item character set when it's necessary.
  Conversion happens only in case of "tricky" Item character set (e.g. UCS2).
  Normally conversion does not happen, and val_str_ascii() is immediately
  returned instead.

  No matter if conversion is needed or not needed,
  the result is always returned in "str" (see MDEV-10306 why).

  @param [OUT] str          - Store the result here
  @param [IN]  ascii_buffer - Use this temporary buffer to call val_str_ascii()
*/
String *Item_func::val_str_from_val_str_ascii(String *str, String *ascii_buffer)
{
  DBUG_ASSERT(fixed());

  if (!(collation.collation->state & MY_CS_NONASCII))
  {
    String *res= val_str_ascii(str);
    if (res)
      res->set_charset(collation.collation);
    return res;
  }
  
  DBUG_ASSERT(str != ascii_buffer);
  
  uint errors;
  String *res= val_str_ascii(ascii_buffer);
  if (!res)
    return 0;
  
  if ((null_value= str->copy(res->ptr(), res->length(),
                             &my_charset_latin1, collation.collation,
                             &errors)))
    return 0;
  
  return str;
}


bool Item_str_func::fix_fields(THD *thd, Item **ref)
{
  bool res= Item_func::fix_fields(thd, ref);
  /*
    In Item_str_func::check_well_formed_result() we may set null_value
    flag on the same condition as in test() below.
  */
  if (thd->is_strict_mode())
    set_maybe_null();
  return res;
}


my_decimal *Item_str_func::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed());
  StringBuffer<64> tmp;
  String *res= val_str(&tmp);
  return res ? decimal_from_string_with_check(decimal_value, res) : 0;
}


double Item_str_func::val_real()
{
  DBUG_ASSERT(fixed());
  StringBuffer<64> tmp;
  String *res= val_str(&tmp);
  return res ? double_from_string_with_check(res) : 0.0;
}


longlong Item_str_func::val_int()
{
  DBUG_ASSERT(fixed());
  StringBuffer<22> tmp;
  String *res= val_str(&tmp);
  return res ? longlong_from_string_with_check(res) : 0;
}


String *Item_func_md5::val_str_ascii(String *str)
{
  DBUG_ASSERT(fixed());
  String * sptr= args[0]->val_str(str);
  if (sptr)
  {
    uchar digest[16];

    null_value=0;
    compute_md5_hash(digest, (const char *) sptr->ptr(), sptr->length());
    if (str->alloc(32))				// Ensure that memory is free
    {
      null_value=1;
      return 0;
    }
    array_to_hex((char *) str->ptr(), digest, 16);
    str->set_charset(&my_charset_numeric);
    str->length((uint) 32);
    return str;
  }
  null_value=1;
  return 0;
}


String *Item_func_sha::val_str_ascii(String *str)
{
  DBUG_ASSERT(fixed());
  String * sptr= args[0]->val_str(str);
  if (sptr)  /* If we got value different from NULL */
  {
    /* Temporary buffer to store 160bit digest */
    uint8 digest[MY_SHA1_HASH_SIZE];
    my_sha1(digest, (const char *) sptr->ptr(), sptr->length());
    /* Ensure that memory is free and we got result */
    if (!str->alloc(MY_SHA1_HASH_SIZE*2))
    {
      array_to_hex((char *) str->ptr(), digest, MY_SHA1_HASH_SIZE);
      str->set_charset(&my_charset_numeric);
      str->length((uint)  MY_SHA1_HASH_SIZE*2);
      null_value=0;
      return str;
    }
  }
  null_value=1;
  return 0;
}

bool Item_func_sha::fix_length_and_dec()
{
  // size of hex representation of hash
  fix_length_and_charset(MY_SHA1_HASH_SIZE * 2, default_charset());
  return FALSE;
}

String *Item_func_sha2::val_str_ascii(String *str)
{
  DBUG_ASSERT(fixed());
  unsigned char digest_buf[512/8]; // enough for SHA512
  String *input_string;
  const char *input_ptr;
  size_t input_len;

  input_string= args[0]->val_str(str);
  str->set_charset(&my_charset_bin);

  if (input_string == NULL)
  {
    null_value= TRUE;
    return (String *) NULL;
  }

  null_value= args[0]->null_value;
  if (null_value)
    return (String *) NULL;

  input_ptr= input_string->ptr();
  input_len= input_string->length();

  longlong digest_length= args[1]->val_int();
  switch (digest_length) {
  case 512:
    my_sha512(digest_buf, input_ptr, input_len);
    break;
  case 384:
    my_sha384(digest_buf, input_ptr, input_len);
    break;
  case 224:
    my_sha224(digest_buf, input_ptr, input_len);
    break;
  case 0: // SHA-256 is the default
    digest_length= 256;
    /* fall through */
  case 256:
    my_sha256(digest_buf, input_ptr, input_len);
    break;
  default:
    if (!args[1]->const_item())
    {
      THD *thd= current_thd;
      push_warning_printf(thd,
                          Sql_condition::WARN_LEVEL_WARN,
                          ER_WRONG_PARAMETERS_TO_NATIVE_FCT,
                          ER_THD(thd, ER_WRONG_PARAMETERS_TO_NATIVE_FCT),
                          "sha2");
    }
    null_value= TRUE;
    return NULL;
  }
  digest_length/= 8; /* bits to bytes */

  /* 
    Since we're subverting the usual String methods, we must make sure that
    the destination has space for the bytes we're about to write.
  */
  str->alloc((uint) digest_length*2 + 1); /* Each byte as two nybbles */

  /* Convert the large number to a string-hex representation. */
  array_to_hex((char *) str->ptr(), digest_buf, (uint)digest_length);

  /* We poked raw bytes in.  We must inform the the String of its length. */
  str->length((uint) digest_length*2); /* Each byte as two nybbles */

  null_value= FALSE;
  return str;
}


bool Item_func_sha2::fix_length_and_dec()
{
  set_maybe_null();
  max_length = 0;

  int sha_variant= (int)(args[1]->const_item() ? args[1]->val_int() : 512);

  switch (sha_variant) {
  case 0: // SHA-256 is the default
    sha_variant= 256;
    /* fall through */
  case 512:
  case 384:
  case 256:
  case 224:
    fix_length_and_charset(sha_variant/8 * 2, default_charset());
    break;
  default:
    THD *thd= current_thd;
    push_warning_printf(thd,
                        Sql_condition::WARN_LEVEL_WARN,
                        ER_WRONG_PARAMETERS_TO_NATIVE_FCT,
                        ER_THD(thd, ER_WRONG_PARAMETERS_TO_NATIVE_FCT),
                        "sha2");
  }
  return FALSE;
}

/* Implementation of AES encryption routines */
void Item_aes_crypt::create_key(String *user_key, uchar *real_key)
{
  uchar *real_key_end= real_key + AES_KEY_LENGTH / 8;
  uchar *ptr;
  const char *sptr= user_key->ptr();
  const char *key_end= sptr + user_key->length();

  bzero(real_key, AES_KEY_LENGTH / 8);

  for (ptr= real_key; sptr < key_end; ptr++, sptr++)
  {
    if (ptr == real_key_end)
      ptr= real_key;
    *ptr ^= (uchar) *sptr;
  }
}


String *Item_aes_crypt::val_str(String *str2)
{
  DBUG_ASSERT(fixed());
  StringBuffer<80> user_key_buf;
  String *sptr= args[0]->val_str(&tmp_value);
  String *user_key=  args[1]->val_str(&user_key_buf);
  uint32 aes_length;

  if (sptr && user_key) // we need both arguments to be not NULL
  {
    null_value=0;
    aes_length=my_aes_get_size(MY_AES_ECB, sptr->length());

    if (!str2->alloc(aes_length))		// Ensure that memory is free
    {
      uchar rkey[AES_KEY_LENGTH / 8];
      create_key(user_key, rkey);

      if (!my_aes_crypt(MY_AES_ECB, what, (uchar*)sptr->ptr(), sptr->length(),
                 (uchar*)str2->ptr(), &aes_length,
                 rkey, AES_KEY_LENGTH / 8, 0, 0))
      {
        str2->length((uint) aes_length);
        return str2;
      }
    }
  }
  null_value=1;
  return 0;
}

bool Item_func_aes_encrypt::fix_length_and_dec()
{
  max_length=my_aes_get_size(MY_AES_ECB, args[0]->max_length);
  what= ENCRYPTION_FLAG_ENCRYPT;
  return FALSE;
}



bool Item_func_aes_decrypt::fix_length_and_dec()
{
  max_length=args[0]->max_length;
  set_maybe_null();
  what= ENCRYPTION_FLAG_DECRYPT;
  return FALSE;
}


bool Item_func_to_base64::fix_length_and_dec()
{
  base_flags|= args[0]->base_flags & item_base_t::MAYBE_NULL;
  collation.set(default_charset(), DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
  if (args[0]->max_length > (uint) my_base64_encode_max_arg_length())
  {
    set_maybe_null();
    fix_char_length_ulonglong((ulonglong) my_base64_encode_max_arg_length());
  }
  else
  {
    int length= my_base64_needed_encoded_length((int) args[0]->max_length);
    DBUG_ASSERT(length > 0);
    fix_char_length_ulonglong((ulonglong) length - 1);
  }
  return FALSE;
}


String *Item_func_to_base64::val_str_ascii(String *str)
{
  String *res= args[0]->val_str(&tmp_value);
  bool too_long= false;
  int length;
  if (!res ||
      res->length() > (uint) my_base64_encode_max_arg_length() ||
      (too_long=
       ((uint) (length= my_base64_needed_encoded_length((int) res->length())) >
        current_thd->variables.max_allowed_packet)) ||
      str->alloc((uint) length))
  {
    null_value= 1; // NULL input, too long input, or OOM.
    if (too_long)
    {
      THD *thd= current_thd;
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                          ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                          func_name(),
                          thd->variables.max_allowed_packet);
    }
    return 0;
  }
  my_base64_encode(res->ptr(), (int) res->length(), (char*) str->ptr());
  DBUG_ASSERT(length > 0);
  str->length((uint) length - 1); // Without trailing '\0'
  null_value= 0;
  return str;
}


bool Item_func_from_base64::fix_length_and_dec()
{
  if (args[0]->max_length > (uint) my_base64_decode_max_arg_length())
  {
    fix_char_length_ulonglong((ulonglong) my_base64_decode_max_arg_length());
  }
  else
  {
    int length= my_base64_needed_decoded_length((int) args[0]->max_length);
    fix_char_length_ulonglong((ulonglong) length);
  }
  // Can be NULL, e.g. in case of badly formed input string
  set_maybe_null();
  return FALSE;
}


String *Item_func_from_base64::val_str(String *str)
{
  String *res= args[0]->val_str_ascii(&tmp_value);
  int length;
  const char *end_ptr;

  if (!res)
    goto err;

  if (res->length() > (uint) my_base64_decode_max_arg_length() ||
      ((uint) (length= my_base64_needed_decoded_length((int) res->length())) >
       current_thd->variables.max_allowed_packet))
  {
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                        ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                        func_name(),
                        thd->variables.max_allowed_packet);
    goto err;
  }

  if (str->alloc((uint) length))
    goto err;

  if ((length= my_base64_decode(res->ptr(), (int) res->length(),
                                (char *) str->ptr(), &end_ptr, 0)) < 0 ||
      end_ptr < res->ptr() + res->length())
  {
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_BAD_BASE64_DATA, ER_THD(thd, ER_BAD_BASE64_DATA),
                        (int) (end_ptr - res->ptr()));
    goto err;
  }

  str->length((uint) length);
  null_value= 0;
  return str;
err:
  null_value= 1; // NULL input, too long input, OOM, or badly formed input
  return 0;
}
///////////////////////////////////////////////////////////////////////////////


const char *histogram_types[] =
           {"SINGLE_PREC_HB", "DOUBLE_PREC_HB", 0};
static TYPELIB histogram_types_typelib=
  { array_elements(histogram_types),
    "histogram_types",
    histogram_types, NULL};
const char *representation_by_type[]= {"%.3f", "%.5f"};

String *Item_func_decode_histogram::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  char buff[STRING_BUFFER_USUAL_SIZE];
  String *res, tmp(buff, sizeof(buff), &my_charset_bin);
  int type;

  tmp.length(0);
  if (!(res= args[0]->val_str(&tmp)) ||
      (type= find_type(res->c_ptr_safe(),
                       &histogram_types_typelib, MYF(0))) <= 0)
  {
    null_value= 1;
    return 0;
  }
  type--;

  tmp.length(0);
  if (!(res= args[1]->val_str(&tmp)))
  {
    null_value= 1;
    return 0;
  }
  if (type == DOUBLE_PREC_HB && res->length() % 2 != 0)
    res->length(res->length() - 1); // one byte is unused

  double prev= 0.0;
  uint i;
  str->length(0);
  char numbuf[32];
  const uchar *p= (uchar*)res->c_ptr_safe();
  for (i= 0; i < res->length(); i++)
  {
    double val;
    switch (type)
    {
    case SINGLE_PREC_HB:
      val= p[i] / ((double)((1 << 8) - 1));
      break;
    case DOUBLE_PREC_HB:
      val= uint2korr(p + i) / ((double)((1 << 16) - 1));
      i++;
      break;
    default:
      val= 0;
      DBUG_ASSERT(0);
    }
    /* show delta with previous value */
    size_t size= my_snprintf(numbuf, sizeof(numbuf),
                          representation_by_type[type], val - prev);
    str->append(numbuf, size);
    str->append(',');
    prev= val;
  }
  /* show delta with max */
  size_t size= my_snprintf(numbuf, sizeof(numbuf),
                        representation_by_type[type], 1.0 - prev);
  str->append(numbuf, size);

  null_value=0;
  return str;
}


///////////////////////////////////////////////////////////////////////////////

/*
  Realloc the result buffer.
  NOTE: We should be prudent in the initial allocation unit -- the
  size of the arguments is a function of data distribution, which
  can be any. Instead of overcommitting at the first row, we grow
  the allocated amount by the factor of 2. This ensures that no
  more than 25% of memory will be overcommitted on average.

  @param IN/OUT str    - the result string
  @param IN     length - new total space required in "str"
  @retval       false  - on success
  @retval       true   - on error
*/

bool Item_func_concat::realloc_result(String *str, uint length) const
{
  if (str->alloced_length() >= length)
    return false; // Alloced space is big enough, nothing to do.

  if (str->alloced_length() == 0)
    return str->alloc(length);

  /*
    Item_func_concat::val_str() makes sure the result length does not grow
    higher than max_allowed_packet. So "length" is limited to 1G here.
    We can't say anything about the current value of str->alloced_length(),
    as str was initially set by args[0]->val_str(str).
    So multiplication by 2 can overflow, if args[0] for some reasons
    did not limit the result to max_alloced_packet. But it's not harmful,
    "str" will be reallocated exactly to "length" bytes in case of overflow.
  */
  uint new_length= MY_MAX(str->alloced_length() * 2, length);
  return str->realloc(new_length);
}


/**
  Concatenate args with the following premises:
  If only one arg (which is ok), return value of arg;
*/

String *Item_func_concat::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  String *res;

  null_value=0;
  if (!(res= args[0]->val_str(str)))
    goto null;

  if (res != str)
    str->copy_or_move(res->ptr(), res->length(), res->charset());

  for (uint i= 1 ; i < arg_count ; i++)
  {
    if (!(res= args[i]->val_str(&tmp_value)) ||
        append_value(thd, str, res))
      goto null;
  }

  str->set_charset(collation.collation);
  return str;

null:
  null_value= true;
  return 0;
}


String *Item_func_concat_operator_oracle::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  String *res= NULL;
  uint i;

  null_value=0;
  // Search first non null argument
  for (i= 0; i < arg_count; i++)
  {
    if ((res= args[i]->val_str(str)))
      break;
  }
  if (!res)
    goto null;

  if (res != str)
    str->copy(res->ptr(), res->length(), res->charset());

  for (i++ ; i < arg_count ; i++)
  {
    if (!(res= args[i]->val_str(&tmp_value)) || res->length() == 0)
     continue;
    if (append_value(thd, str, res))
      goto null;
  }

  str->set_charset(collation.collation);
  return str;

null:
  null_value= true;
  return 0;
}


bool Item_func_concat::append_value(THD *thd, String *res, const String *app)
{
  uint concat_len;
  if ((concat_len= res->length() + app->length()) >
      thd->variables.max_allowed_packet)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                        ER(ER_WARN_ALLOWED_PACKET_OVERFLOWED), func_name(),
                        thd->variables.max_allowed_packet);
    return true;
  }
  DBUG_ASSERT(!res->uses_buffer_owned_by(app));
  DBUG_ASSERT(!app->uses_buffer_owned_by(res));
  return realloc_result(res, concat_len) || res->append(*app);
}


bool Item_func_concat::fix_length_and_dec()
{
  ulonglong char_length= 0;

  if (agg_arg_charsets_for_string_result(collation, args, arg_count))
    return TRUE;

  for (uint i=0 ; i < arg_count ; i++)
    char_length+= args[i]->max_char_length();

  fix_char_length_ulonglong(char_length);
  return FALSE;
}

/**
  @details
  Function des_encrypt() by tonu@spam.ee & monty
  Works only if compiled with OpenSSL library support.
  @return
    A binary string where first character is CHAR(128 | key-number).
    If one uses a string key key_number is 127.
    Encryption result is longer than original by formula:
  @code new_length= org_length + (8-(org_length % 8))+1 @endcode
*/

String *Item_func_des_encrypt::val_str(String *str)
{
  DBUG_ASSERT(fixed());
#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
  uint code= ER_WRONG_PARAMETERS_TO_PROCEDURE;
  DES_cblock ivec;
  struct st_des_keyblock keyblock;
  struct st_des_keyschedule keyschedule;
  const char *append_str="********";
  uint key_number, res_length, tail;
  String *res= args[0]->val_str(&tmp_value);

  if ((null_value= args[0]->null_value))
    return 0;                                   // ENCRYPT(NULL) == NULL
  if ((res_length=res->length()) == 0)
    return make_empty_result();
  if (arg_count == 1)
  {
    /* Protect against someone doing FLUSH DES_KEY_FILE */
    mysql_mutex_lock(&LOCK_des_key_file);
    keyschedule= des_keyschedule[key_number=des_default_key];
    mysql_mutex_unlock(&LOCK_des_key_file);
  }
  else if (args[1]->result_type() == INT_RESULT)
  {
    key_number= (uint) args[1]->val_int();
    if (key_number > 9)
      goto error;
    mysql_mutex_lock(&LOCK_des_key_file);
    keyschedule= des_keyschedule[key_number];
    mysql_mutex_unlock(&LOCK_des_key_file);
  }
  else
  {
    String *keystr= args[1]->val_str(str);
    if (!keystr)
      goto error;
    key_number=127;				// User key string

    /* We make good 24-byte (168 bit) key from given plaintext key with MD5 */
    bzero((char*) &ivec,sizeof(ivec));
    if (!EVP_BytesToKey(EVP_des_ede3_cbc(),EVP_md5(),NULL,
		   (uchar*) keystr->ptr(), (int) keystr->length(),
		   1, (uchar*) &keyblock,ivec))
      goto error;
    DES_set_key_unchecked(&keyblock.key1,&keyschedule.ks1);
    DES_set_key_unchecked(&keyblock.key2,&keyschedule.ks2);
    DES_set_key_unchecked(&keyblock.key3,&keyschedule.ks3);
  }

  /*
     The problem: DES algorithm requires original data to be in 8-bytes
     chunks. Missing bytes get filled with '*'s and result of encryption
     can be up to 8 bytes longer than original string. When decrypted,
     we do not know the size of original string :(
     We add one byte with value 0x1..0x8 as the last byte of the padded
     string marking change of string length.
  */

  tail= 8 - (res_length % 8);                   // 1..8 marking extra length
  res_length+=tail;
  if (tmp_arg.alloc(res_length))
    goto error;
  tmp_arg.length(0);
  tmp_arg.append(res->ptr(), res->length());
  code= ER_OUT_OF_RESOURCES;
  if (tmp_arg.append(append_str, tail) || str->alloc(res_length+1))
    goto error;
  tmp_arg[res_length-1]=tail;                   // save extra length
  str->length(res_length+1);
  str->set_charset(&my_charset_bin);
  (*str)[0]=(char) (128 | key_number);
  // Real encryption
  bzero((char*) &ivec,sizeof(ivec));
  DES_ede3_cbc_encrypt((const uchar*) (tmp_arg.ptr()),
		       (uchar*) (str->ptr()+1),
		       res_length,
		       &keyschedule.ks1,
		       &keyschedule.ks2,
		       &keyschedule.ks3,
		       &ivec, TRUE);
  return str;

error:
  THD *thd= current_thd;
  push_warning_printf(thd,Sql_condition::WARN_LEVEL_WARN,
                      code, ER_THD(thd, code),
                      "des_encrypt");
#else
  THD *thd= current_thd;
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_FEATURE_DISABLED, ER_THD(thd, ER_FEATURE_DISABLED),
                      "des_encrypt", "--with-ssl");
#endif /* defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY) */
  null_value=1;
  return 0;
}


String *Item_func_des_decrypt::val_str(String *str)
{
  DBUG_ASSERT(fixed());
#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
  uint code= ER_WRONG_PARAMETERS_TO_PROCEDURE;
  DES_cblock ivec;
  struct st_des_keyblock keyblock;
  struct st_des_keyschedule keyschedule;
  String *res= args[0]->val_str(&tmp_value);
  uint length,tail;

  if ((null_value= args[0]->null_value))
    return 0;
  length= res->length();
  if (length < 9 || (length % 8) != 1 || !((*res)[0] & 128))
    return res;				// Skip decryption if not encrypted

  if (arg_count == 1)			// If automatic uncompression
  {
    uint key_number=(uint) (*res)[0] & 127;
    // Check if automatic key and that we have privilege to uncompress using it
    if (!(current_thd->security_ctx->master_access & PRIV_DES_DECRYPT_ONE_ARG) ||
        key_number > 9)
      goto error;

    mysql_mutex_lock(&LOCK_des_key_file);
    keyschedule= des_keyschedule[key_number];
    mysql_mutex_unlock(&LOCK_des_key_file);
  }
  else
  {
    // We make good 24-byte (168 bit) key from given plaintext key with MD5
    String *keystr= args[1]->val_str(str);
    if (!keystr)
      goto error;

    bzero((char*) &ivec,sizeof(ivec));
    if (!EVP_BytesToKey(EVP_des_ede3_cbc(),EVP_md5(),NULL,
		   (uchar*) keystr->ptr(),(int) keystr->length(),
		   1,(uchar*) &keyblock,ivec))
      goto error;
    // Here we set all 64-bit keys (56 effective) one by one
    DES_set_key_unchecked(&keyblock.key1,&keyschedule.ks1);
    DES_set_key_unchecked(&keyblock.key2,&keyschedule.ks2);
    DES_set_key_unchecked(&keyblock.key3,&keyschedule.ks3);
  }
  code= ER_OUT_OF_RESOURCES;
  if (str->alloc(length-1))
    goto error;

  bzero((char*) &ivec,sizeof(ivec));
  DES_ede3_cbc_encrypt((const uchar*) res->ptr()+1,
		       (uchar*) (str->ptr()),
		       length-1,
		       &keyschedule.ks1,
		       &keyschedule.ks2,
		       &keyschedule.ks3,
		       &ivec, FALSE);
  /* Restore old length of key */
  if ((tail=(uint) (uchar) (*str)[length-2]) > 8)
    goto wrong_key;				     // Wrong key
  str->length(length-1-tail);
  str->set_charset(&my_charset_bin);
  return str;

error:
  {
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        code, ER_THD(thd, code),
                        "des_decrypt");
  }
wrong_key:
#else
  {
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_FEATURE_DISABLED, ER_THD(thd, ER_FEATURE_DISABLED),
                        "des_decrypt", "--with-ssl");
  }
#endif /* defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY) */
  null_value=1;
  return 0;
}


/**
  concat with separator. First arg is the separator
  concat_ws takes at least two arguments.
*/

String *Item_func_concat_ws::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  char tmp_str_buff[10];
  String tmp_sep_str(tmp_str_buff, sizeof(tmp_str_buff),default_charset_info),
         *sep_str, *res, *res2,*use_as_buff;
  uint i;
  bool is_const= 0;
  THD *thd= 0;

  null_value=0;
  if (!(sep_str= args[0]->val_str(&tmp_sep_str)))
    goto null;

  use_as_buff= &tmp_value;
  str->length(0);				// QQ; Should be removed
  res=str;                                      // If 0 arg_count

  // Skip until non-null argument is found.
  // If not, return the empty string
  for (i=1; i < arg_count; i++)
    if ((res= args[i]->val_str(str)))
    {
      is_const= args[i]->const_item();
      break;
    }

  if (i ==  arg_count)
    return make_empty_result();

  for (i++; i < arg_count ; i++)
  {
    if (!(res2= args[i]->val_str(use_as_buff)))
      continue;					// Skip NULL

    if (!thd)
      thd= current_thd;
    if (res->length() + sep_str->length() + res2->length() >
	thd->variables.max_allowed_packet)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
			  ER_WARN_ALLOWED_PACKET_OVERFLOWED,
			  ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                          func_name(),
			  thd->variables.max_allowed_packet);
      goto null;
    }
    if (!is_const && res->alloced_length() >=
	res->length() + sep_str->length() + res2->length())
    {						// Use old buffer
      res->append(*sep_str);			// res->length() > 0 always
      res->append(*res2);
    }
    else if (str->alloced_length() >=
	     res->length() + sep_str->length() + res2->length())
    {
      /* We have room in str;  We can't get any errors here */
      if (str->ptr() == res2->ptr())
      {						// This is quite uncommon!
	str->replace(0,0,*sep_str);
	str->replace(0,0,*res);
      }
      else
      {
	str->copy(*res);
	str->append(*sep_str);
	str->append(*res2);
      }
      res=str;
      use_as_buff= &tmp_value;
    }
    else if (res == &tmp_value)
    {
      if (res->append(*sep_str) || res->append(*res2))
	goto null; // Must be a blob
    }
    else if (res2 == &tmp_value)
    {						// This can happen only 1 time
      if (tmp_value.replace(0,0,*sep_str) || tmp_value.replace(0,0,*res))
	goto null;
      res= &tmp_value;
      use_as_buff=str;				// Put next arg here
    }
    else if (tmp_value.is_alloced() && res2->ptr() >= tmp_value.ptr() &&
	     res2->ptr() < tmp_value.ptr() + tmp_value.alloced_length())
    {
      /*
	This happens really seldom:
	In this case res2 is sub string of tmp_value.  We will
	now work in place in tmp_value to set it to res | sep_str | res2
      */
      /* Chop the last characters in tmp_value that isn't in res2 */
      tmp_value.length((uint32) (res2->ptr() - tmp_value.ptr()) +
		       res2->length());
      /* Place res2 at start of tmp_value, remove chars before res2 */
      if (tmp_value.replace(0,(uint32) (res2->ptr() - tmp_value.ptr()),
			    *res) ||
	  tmp_value.replace(res->length(),0, *sep_str))
	goto null;
      res= &tmp_value;
      use_as_buff=str;			// Put next arg here
    }
    else
    {						// Two big const strings
      /*
        NOTE: We should be prudent in the initial allocation unit -- the
        size of the arguments is a function of data distribution, which can
        be any. Instead of overcommitting at the first row, we grow the
        allocated amount by the factor of 2. This ensures that no more than
        25% of memory will be overcommitted on average.
      */

      uint concat_len= res->length() + sep_str->length() + res2->length();

      if (tmp_value.alloced_length() < concat_len)
      {
        if (tmp_value.alloced_length() == 0)
        {
          if (tmp_value.alloc(concat_len))
            goto null;
        }
        else
        {
          uint new_len = MY_MAX(tmp_value.alloced_length() * 2, concat_len);

          if (tmp_value.alloc(new_len))
            goto null;
        }
      }

      if (tmp_value.copy(*res) ||
	  tmp_value.append(*sep_str) ||
	  tmp_value.append(*res2))
	goto null;
      res= &tmp_value;
      use_as_buff=str;
    }
  }
  res->set_charset(collation.collation);
  return res;

null:
  null_value=1;
  return 0;
}


bool Item_func_concat_ws::fix_length_and_dec()
{
  ulonglong char_length;

  if (agg_arg_charsets_for_string_result(collation, args, arg_count))
    return TRUE;

  /*
     arg_count cannot be less than 2,
     it is done on parser level in sql_yacc.yy
     so, (arg_count - 2) is safe here.
  */
  char_length= (ulonglong) args[0]->max_char_length() * (arg_count - 2);
  for (uint i=1 ; i < arg_count ; i++)
    char_length+= args[i]->max_char_length();

  fix_char_length_ulonglong(char_length);
  return FALSE;
}


String *Item_func_reverse::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String *res= args[0]->val_str(&tmp_value);
  const char *ptr, *end;
  char *tmp;

  if ((null_value=args[0]->null_value))
    return 0;
  /* An empty string is a special case as the string pointer may be null */
  if (!res->length())
    return make_empty_result();
  if (str->alloc(res->length()))
  {
    null_value= 1;
    return 0;
  }
  str->length(res->length());
  str->set_charset(res->charset());
  ptr= res->ptr();
  end= res->end();
  tmp= (char *) str->end();
#ifdef USE_MB
  if (res->use_mb())
  {
    uint32 l;
    while (ptr < end)
    {
      if ((l= my_ismbchar(res->charset(),ptr,end)))
      {
        tmp-= l;
        DBUG_ASSERT(tmp >= str->ptr());
        memcpy(tmp,ptr,l);
        ptr+= l;
      }
      else
        *--tmp= *ptr++;
    }
  }
  else
#endif /* USE_MB */
  {
    while (ptr < end)
      *--tmp= *ptr++;
  }
  return str;
}


bool Item_func_reverse::fix_length_and_dec()
{
  if (agg_arg_charsets_for_string_result(collation, args, 1))
    return TRUE;
  DBUG_ASSERT(collation.collation != NULL);
  fix_char_length(args[0]->max_char_length());
  return FALSE;
}

/**
  Replace all occurrences of string2 in string1 with string3.

  Don't reallocate val_str() if not needed.

  @todo
    Fix that this works with binary strings when using USE_MB 
*/

String *Item_func_replace::val_str_internal(String *str,
                                            String *empty_string_for_null)
{
  DBUG_ASSERT(fixed());
  String *res,*res2,*res3;
  int offset;
  uint from_length,to_length;
  bool alloced=0;
#ifdef USE_MB
  const char *ptr,*end,*strend,*search,*search_end;
  uint32 l;
  bool binary_cmp;
#endif
  THD *thd= 0;

  null_value=0;
  res=args[0]->val_str(str);
  if (args[0]->null_value)
    goto null;
  res2=args[1]->val_str(&tmp_value);
  if (args[1]->null_value)
  {
    if (!empty_string_for_null)
      goto null;
    res2= empty_string_for_null;
  }
  res->set_charset(collation.collation);

#ifdef USE_MB
  binary_cmp = ((res->charset()->state & MY_CS_BINSORT) || !res->use_mb());
#endif

  if (res2->length() == 0)
    return res;
#ifndef USE_MB
  if ((offset=res->strstr(*res2)) < 0)
    return res;
#else
  offset=0;
  if (binary_cmp && (offset=res->strstr(*res2)) < 0)
    return res;
#endif
  if (!(res3=args[2]->val_str(&tmp_value2)))
  {
    if (!empty_string_for_null)
      goto null;
    res3= empty_string_for_null;
  }
  from_length= res2->length();
  to_length=   res3->length();

#ifdef USE_MB
  if (!binary_cmp)
  {
    search=res2->ptr();
    search_end=search+from_length;
redo:
    DBUG_ASSERT(res->ptr() || !offset);
    ptr=res->ptr()+offset;
    strend=res->ptr()+res->length();
    /*
      In some cases val_str() can return empty string
      with ptr() == NULL and length() == 0.
      Let's check strend to avoid overflow.
    */
    end= strend ? strend - from_length + 1 : NULL;
    while (ptr < end)
    {
      if (*ptr == *search)
      {
        char *i,*j;
        i=(char*) ptr+1; j=(char*) search+1;
        while (j != search_end)
          if (*i++ != *j++) goto skip;
        offset= (int) (ptr-res->ptr());

        if (!thd)
          thd= current_thd;

        if (res->length()-from_length + to_length >
            thd->variables.max_allowed_packet)
        {
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                              ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                              func_name(),
                              thd->variables.max_allowed_packet);

          goto null;
        }
        if (!alloced)
        {
          alloced=1;
          res=copy_if_not_alloced(str,res,res->length()+to_length);
        }
        res->replace((uint) offset,from_length,*res3);
        offset+=(int) to_length;
        goto redo;
      }
  skip:
      if ((l=my_ismbchar(res->charset(), ptr,strend))) ptr+=l;
      else ++ptr;
    }
  }
  else
#endif /* USE_MB */
  {
    thd= current_thd;
    do
    {
      if (res->length()-from_length + to_length >
	  thd->variables.max_allowed_packet)
      {
	push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
			    ER_WARN_ALLOWED_PACKET_OVERFLOWED,
			    ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                            func_name(),
			    thd->variables.max_allowed_packet);
        goto null;
      }
      if (!alloced)
      {
        alloced=1;
        res=copy_if_not_alloced(str,res,res->length()+to_length);
      }
      res->replace((uint) offset,from_length,*res3);
      offset+=(int) to_length;
    }
    while ((offset=res->strstr(*res2,(uint) offset)) >= 0);
  }
  if (empty_string_for_null && !res->length())
    goto null;

  return res;

null:
  null_value=1;
  return 0;
}


bool Item_func_replace::fix_length_and_dec()
{
  ulonglong char_length= (ulonglong) args[0]->max_char_length();
  int diff=(int) (args[2]->max_char_length() - 1);
  if (diff > 0)
  {						// Calculate of maxreplaces
    ulonglong max_substrs= char_length;
    char_length+= max_substrs * (uint) diff;
  }

  if (agg_arg_charsets_for_string_result_with_comparison(collation, args, 3))
    return TRUE;
  fix_char_length_ulonglong(char_length);
  return FALSE;
}


bool Item_func_sformat::fix_length_and_dec()
{
  ulonglong char_length= 0;

  if (agg_arg_charsets_for_string_result(collation, args, arg_count))
    return TRUE;

  for (uint i=0 ; i < arg_count ; i++)
    char_length+= args[i]->max_char_length();

  fix_char_length_ulonglong(char_length);
  return FALSE;
}

/*
  allow fmt to take String arguments directly.
  Inherit from string_view, so all string formatting works.
  but {:p} doesn't, because it's not char*, not a pointer.
*/
namespace fmt {
  template <> struct formatter<String>: formatter<string_view> {
    template <typename FormatContext>
    auto format(String c, FormatContext& ctx) -> decltype(ctx.out()) {
      string_view name = { c.ptr(), c.length() };
      return formatter<string_view>::format(name, ctx);
    };
  };
};

/*
  SFORMAT(format_string, ...)
  This function receives a formatting specification string and N parameters
  (N >= 0), and it returns string formatted using the rules the user passed
  in the specification. It uses fmtlib (https://fmt.dev/).
*/
String *Item_func_sformat::val_str(String *res)
{
  DBUG_ASSERT(fixed());
  using                         ctx=     fmt::format_context;
  String                       *fmt_arg= NULL;
  String                       *parg=    NULL;
  String                       *val_arg= NULL;
  fmt::format_args::format_arg *vargs=   NULL;

  null_value= true;
  if (!(fmt_arg= args[0]->val_str(res)))
    return NULL;

  if (!(vargs= new fmt::format_args::format_arg[arg_count - 1]))
    return NULL;

  if (!(val_arg= new String[arg_count - 1]))
  {
    delete [] vargs;
    return NULL;
  }

  /* Creates the array of arguments for vformat */
  for (uint carg= 1; carg < arg_count; carg++)
  {
    switch (args[carg]->result_type())
    {
    case INT_RESULT:
      vargs[carg-1]= fmt::detail::make_arg<ctx>(args[carg]->val_int());
      break;
    case DECIMAL_RESULT: // TODO
    case REAL_RESULT:
      vargs[carg-1]= fmt::detail::make_arg<ctx>(args[carg]->val_real());
      break;
    case STRING_RESULT:
      if (!(parg= args[carg]->val_str(&val_arg[carg-1])))
      {
        delete [] vargs;
        delete [] val_arg;
        return NULL;
      }
      if (parg->length() == 1)
        vargs[carg-1]= fmt::detail::make_arg<ctx>(parg->ptr()[0]);
      else
        vargs[carg-1]= fmt::detail::make_arg<ctx>(*parg);
      break;
    case TIME_RESULT: // TODO
    case ROW_RESULT: // TODO
    default:
      DBUG_ASSERT(0);
      delete [] vargs;
      delete [] val_arg;
      return NULL;
    }
  }

  null_value= false;
  /* Create the string output  */
  try
  {
    auto text = fmt::vformat(fmt_arg->c_ptr_safe(),
                             fmt::format_args(vargs, arg_count-1));
    res->length(0);
    res->set_charset(collation.collation);
    res->append(text.c_str(), text.size());
  }
  catch (const fmt::format_error &ex)
  {
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        WARN_SFORMAT_ERROR,
                        ER_THD(thd, WARN_SFORMAT_ERROR), ex.what());
    null_value= true;
  }
  delete [] vargs;
  delete [] val_arg;
  return null_value ? NULL : res;
}

/*********************************************************************/
bool Item_func_regexp_replace::fix_length_and_dec()
{
  if (agg_arg_charsets_for_string_result_with_comparison(collation, args, 3))
    return TRUE;
  max_length= MAX_BLOB_WIDTH;
  re.init(collation.collation, 0);
  re.fix_owner(this, args[0], args[1]);
  return FALSE;
}


/*
  Traverse through the replacement string and append to "str".
  Sub-pattern references \0 .. \9 are recognized, which are replaced
  to the chunks of the source string.
*/
bool Item_func_regexp_replace::append_replacement(String *str,
                                                  const LEX_CSTRING *source,
                                                  const LEX_CSTRING *replace)
{
  const char *beg= replace->str;
  const char *end= beg + replace->length;
  CHARSET_INFO *cs= re.library_charset();

  for ( ; ; )
  {
    my_wc_t wc;
    int cnv, n;

    if ((cnv= cs->mb_wc(&wc, (const uchar *) beg,
                             (const uchar *) end)) < 1)
      break; /* End of line */
    beg+= cnv;

    if (wc != '\\')
    {
      if (str->append(beg - cnv, cnv, cs))
        return true;
      continue;
    }

    if ((cnv= cs->mb_wc(&wc, (const uchar *) beg,
                             (const uchar *) end)) < 1)
      break; /* End of line */
    beg+= cnv;

    if ((n= ((int) wc) - '0') >= 0 && n <= 9)
    {
      if (n < re.nsubpatterns())
      {
        /* A valid sub-pattern reference found */
        size_t pbeg= re.subpattern_start(n), plength= re.subpattern_end(n) - pbeg;
        if (str->append(source->str + pbeg, plength, cs))
          return true;
      }
    }
    else
    {
      /*
         A non-digit character following after '\'.
         Just add the character itself.
       */
      if (str->append(beg - cnv, cnv, cs))
        return false;
    }
  }
  return false;
}


String *Item_func_regexp_replace::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  char buff0[MAX_FIELD_WIDTH];
  char buff2[MAX_FIELD_WIDTH];
  String tmp0(buff0,sizeof(buff0),&my_charset_bin);
  String tmp2(buff2,sizeof(buff2),&my_charset_bin);
  String *source= args[0]->val_str(&tmp0);
  String *replace= args[2]->val_str(&tmp2);
  LEX_CSTRING src, rpl;
  size_t startoffset= 0;

  if ((null_value= (args[0]->null_value || args[2]->null_value ||
                    re.recompile(args[1]))))
    return (String *) 0;

  if (!(source= re.convert_if_needed(source, &re.subject_converter)) ||
      !(replace= re.convert_if_needed(replace, &re.replace_converter)))
    goto err;

  source->get_value(&src);
  replace->get_value(&rpl);

  str->length(0);
  str->set_charset(collation.collation);

  for ( ; ; ) // Iterate through all matches
  {

    if (re.exec(src.str, src.length, startoffset))
      goto err;

    if (!re.match() || re.subpattern_length(0) == 0)
    {
      /* 
        No match or an empty match.
        Append the rest of the source string
        starting from startoffset until the end of the source.
      */
      if (str->append(src.str + startoffset, src.length - startoffset,
                      re.library_charset()))
        goto err;
      return str;
    }

    /*
      Append prefix, the part before the matching pattern.
      starting from startoffset until the next match
    */
    if (str->append(src.str + startoffset,
          re.subpattern_start(0) - startoffset, re.library_charset()))
      goto err;

    // Append replacement
    if (append_replacement(str, &src, &rpl))
      goto err;

    // Set the new start point as the end of previous match
    startoffset= re.subpattern_end(0);
  }
  return str;

err:
  null_value= true;
  return (String *) 0;
}


bool Item_func_regexp_substr::fix_length_and_dec()
{
  if (agg_arg_charsets_for_string_result_with_comparison(collation, args, 2))
    return TRUE;
  fix_char_length(args[0]->max_char_length());
  re.init(collation.collation, 0);
  re.fix_owner(this, args[0], args[1]);
  return FALSE;
}


String *Item_func_regexp_substr::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  char buff0[MAX_FIELD_WIDTH];
  String tmp0(buff0,sizeof(buff0),&my_charset_bin);
  String *source= args[0]->val_str(&tmp0);

  if ((null_value= (args[0]->null_value || re.recompile(args[1]))))
    return (String *) 0;

  if (!(source= re.convert_if_needed(source, &re.subject_converter)))
    goto err;

  str->length(0);
  str->set_charset(collation.collation);

  if (re.exec(source->ptr(), source->length(), 0))
    goto err;

  if (!re.match())
    return str;

  if (str->append(source->ptr() + re.subpattern_start(0),
                  re.subpattern_length(0), re.library_charset()))
    goto err;

  return str;

err:
  null_value= true;
  return (String *) 0;
}


/************************************************************************/


String *Item_func_insert::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String *res,*res2;
  longlong start, length;  /* must be longlong to avoid truncation */

  null_value=0;
  res=args[0]->val_str(str);
  res2=args[3]->val_str(&tmp_value);
  start= args[1]->val_int();
  length= args[2]->val_int();

  if (args[0]->null_value || args[1]->null_value || args[2]->null_value ||
      args[3]->null_value)
    goto null; /* purecov: inspected */

  if ((start <= 0) || (start > res->length()))
    return res;                                 // Wrong param; skip insert
  if ((length < 0) || (length > res->length()))
    length= res->length();
  start--;

  /*
    There is one exception not handled (intentionally) by the character set
    aggregation code. If one string is strong side and is binary, and
    another one is weak side and is a multi-byte character string,
    then we need to operate on the second string in terms on bytes when
    calling ::numchars() and ::charpos(), rather than in terms of characters.
    Lets substitute its character set to binary.
  */
  if (collation.collation == &my_charset_bin)
  {
    res->set_charset(&my_charset_bin);
    res2->set_charset(&my_charset_bin);
  }

  /* start and length are now sufficiently valid to pass to charpos function */
   start= res->charpos((int) start);
   length= res->charpos((int) length, (uint32) start);

  /* Re-testing with corrected params */
  if (start + 1 > res->length()) // remember, start = args[1].val_int() - 1
    return res; /* purecov: inspected */        // Wrong param; skip insert
  if (length > res->length() - start)
    length= res->length() - start;

  {
    THD *thd= current_thd;
    if ((ulonglong) (res->length() - length + res2->length()) >
        (ulonglong) thd->variables.max_allowed_packet)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                          ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                          func_name(), thd->variables.max_allowed_packet);
      goto null;
    }
  }
  res=copy_if_not_alloced(str,res,res->length());
  res->replace((uint32) start,(uint32) length,*res2);
  return res;
null:
  null_value=1;
  return 0;
}


bool Item_func_insert::fix_length_and_dec()
{
  ulonglong char_length;

  // Handle character set for args[0] and args[3].
  if (agg_arg_charsets_for_string_result(collation, args, 2, 3))
    return TRUE;
  char_length= ((ulonglong) args[0]->max_char_length() +
                (ulonglong) args[3]->max_char_length());
  fix_char_length_ulonglong(char_length);
  return FALSE;
}


String *Item_str_conv::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String *res;
  size_t alloced_length, len;

  if ((null_value= (!(res= args[0]->val_str(&tmp_value)) ||
                    str->alloc((alloced_length= res->length() * multiply)))))
    return 0;

  len= converter(collation.collation, (char*) res->ptr(), res->length(),
                                      (char*) str->ptr(), alloced_length);
  DBUG_ASSERT(len <= alloced_length);
  str->set_charset(collation.collation);
  str->length(len);
  return str;
}


bool Item_func_lcase::fix_length_and_dec()
{
  if (agg_arg_charsets_for_string_result(collation, args, 1))
    return TRUE;
  DBUG_ASSERT(collation.collation != NULL);
  multiply= collation.collation->casedn_multiply;
  converter= collation.collation->cset->casedn;
  fix_char_length_ulonglong((ulonglong) args[0]->max_char_length() * multiply);
  return FALSE;
}

bool Item_func_ucase::fix_length_and_dec()
{
  if (agg_arg_charsets_for_string_result(collation, args, 1))
    return TRUE;
  DBUG_ASSERT(collation.collation != NULL);
  multiply= collation.collation->caseup_multiply;
  converter= collation.collation->cset->caseup;
  fix_char_length_ulonglong((ulonglong) args[0]->max_char_length() * multiply);
  return FALSE;
}


String *Item_func_left::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String *res= args[0]->val_str(str);

  /* must be longlong to avoid truncation */
  longlong length= args[1]->val_int();
  uint char_pos;

  if ((null_value=(args[0]->null_value || args[1]->null_value)))
    return 0;

  /* if "unsigned_flag" is set, we have a *huge* positive number. */
  if ((length <= 0) && (!args[1]->unsigned_flag))
    return make_empty_result();
  if ((res->length() <= (ulonglong) length) ||
      (res->length() <= (char_pos= res->charpos((int) length))))
    return res;

  tmp_value.set(*res, 0, char_pos);
  return &tmp_value;
}


void Item_str_func::left_right_max_length()
{
  uint32 char_length= args[0]->max_char_length();
  if (args[1]->can_eval_in_optimize())
  {
    uint32 length= max_length_for_string(args[1]);
    set_if_smaller(char_length, length);
  }
  fix_char_length(char_length);
}


bool Item_func_left::fix_length_and_dec()
{
  if (agg_arg_charsets_for_string_result(collation, args, 1))
    return TRUE;
  DBUG_ASSERT(collation.collation != NULL);
  left_right_max_length();
  return FALSE;
}


String *Item_func_right::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String *res= args[0]->val_str(str);
  /* must be longlong to avoid truncation */
  longlong length= args[1]->val_int();

  if ((null_value=(args[0]->null_value || args[1]->null_value)))
    return 0; /* purecov: inspected */

  /* if "unsigned_flag" is set, we have a *huge* positive number. */
  if ((length <= 0) && (!args[1]->unsigned_flag))
    return make_empty_result(); /* purecov: inspected */

  if (res->length() <= (ulonglong) length)
    return res; /* purecov: inspected */

  uint start=res->numchars();
  if (start <= (uint) length)
    return res;
  start=res->charpos(start - (uint) length);
  tmp_value.set(*res,start,res->length()-start);
  return &tmp_value;
}


bool Item_func_right::fix_length_and_dec()
{
  if (agg_arg_charsets_for_string_result(collation, args, 1))
    return TRUE;
  DBUG_ASSERT(collation.collation != NULL);
  left_right_max_length();
  return FALSE;
}


String *Item_func_substr::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String *res  = args[0]->val_str(str);
  /* must be longlong to avoid truncation */
  longlong start= get_position();
  /* Assumes that the maximum length of a String is < INT_MAX32. */
  /* Limit so that code sees out-of-bound value properly. */
  longlong length= arg_count == 3 ? args[2]->val_int() : INT_MAX32;
  longlong tmp_length;

  if ((null_value=(args[0]->null_value || args[1]->null_value ||
		   (arg_count == 3 && args[2]->null_value))))
    return 0; /* purecov: inspected */

  /* Negative or zero length, will return empty string. */
  if ((arg_count == 3) && (length <= 0) && 
      (length == 0 || !args[2]->unsigned_flag))
    return make_empty_result();

  /* Assumes that the maximum length of a String is < INT_MAX32. */
  /* Set here so that rest of code sees out-of-bound value as such. */
  if ((length <= 0) || (length > INT_MAX32))
    length= INT_MAX32;

  /* if "unsigned_flag" is set, we have a *huge* positive number. */
  /* Assumes that the maximum length of a String is < INT_MAX32. */
  if ((!args[1]->unsigned_flag && (start < INT_MIN32 || start > INT_MAX32)) ||
      (args[1]->unsigned_flag && ((ulonglong) start > INT_MAX32)))
    return make_empty_result();

  start= ((start < 0) ? res->numchars() + start : start - 1);
  start= res->charpos((int) start);
  if ((start < 0) || ((uint) start + 1 > res->length()))
    return make_empty_result();

  length= res->charpos((int) length, (uint32) start);
  tmp_length= res->length() - start;
  length= MY_MIN(length, tmp_length);

  if (!start && (longlong) res->length() == length)
    return res;
  tmp_value.set(*res, (uint32) start, (uint32) length);
  return &tmp_value;
}


bool Item_func_substr::fix_length_and_dec()
{
  max_length=args[0]->max_length;

  if (agg_arg_charsets_for_string_result(collation, args, 1))
    return TRUE;
  DBUG_ASSERT(collation.collation != NULL);
  if (args[1]->const_item())
  {
    int32 start= (int32) get_position();
    if (args[1]->null_value)
      max_length= 0;
    else if (start < 0)
      max_length= ((uint)(-start) > max_length) ? 0 : (uint)(-start);
    else
      max_length-= MY_MIN((uint)(start - 1), max_length);
  }
  if (arg_count == 3 && args[2]->const_item())
  {
    int32 length= (int32) args[2]->val_int();
    if (args[2]->null_value || length <= 0)
      max_length=0; /* purecov: inspected */
    else
      set_if_smaller(max_length,(uint) length);
  }
  max_length*= collation.collation->mbmaxlen;
  return FALSE;
}


bool Item_func_substr_index::fix_length_and_dec()
{
  if (agg_arg_charsets_for_string_result_with_comparison(collation, args, 2))
    return TRUE;
  fix_char_length(args[0]->max_char_length());
  return FALSE;
}


String *Item_func_substr_index::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  char buff[MAX_FIELD_WIDTH];
  String tmp(buff,sizeof(buff),system_charset_info);
  String *res= args[0]->val_str(&tmp_value);
  String *delimiter= args[1]->val_str(&tmp);
  int32 count= (int32) args[2]->val_int();
  uint offset;

  if (args[0]->null_value || args[1]->null_value || args[2]->null_value)
  {					// string and/or delim are null
    null_value=1;
    return 0;
  }
  null_value=0;
  uint delimiter_length= delimiter->length();
  if (!res->length() || !delimiter_length || !count)
    return make_empty_result();		// Wrong parameters

  res->set_charset(collation.collation);

#ifdef USE_MB
  if (res->use_mb())
  {
    const char *ptr= res->ptr();
    const char *strend= ptr+res->length();
    const char *end= strend-delimiter_length+1;
    const char *search= delimiter->ptr();
    const char *search_end= search+delimiter_length;
    int32 n=0,c=count,pass;
    uint32 l;
    for (pass=(count>0);pass<2;++pass)
    {
      while (ptr < end)
      {
        if (*ptr == *search)
        {
	  char *i,*j;
	  i=(char*) ptr+1; j=(char*) search+1;
	  while (j != search_end)
	    if (*i++ != *j++) goto skip;
	  if (pass==0) ++n;
	  else if (!--c) break;
	  ptr+= delimiter_length;
	  continue;
	}
    skip:
        if ((l=my_ismbchar(res->charset(), ptr,strend))) ptr+=l;
        else ++ptr;
      } /* either not found or got total number when count<0 */
      if (pass == 0) /* count<0 */
      {
        c+=n+1;
        if (c<=0)
        {
          str->copy(res->ptr(), res->length(), collation.collation);
          return str; // not found, return the original string
        }
        ptr=res->ptr();
      }
      else
      {
        if (c)
        {
          str->copy(res->ptr(), res->length(), collation.collation);
          return str; // not found, return the original string
        }
        if (count>0) /* return left part */
        {
          str->copy(res->ptr(), (uint32) (ptr-res->ptr()), collation.collation);
          return str;
        }
        else /* return right part */
        {
          ptr+= delimiter_length;
          str->copy(res->ptr() + (ptr-res->ptr()), (uint32) (strend - ptr),
                    collation.collation);
          return str;
        }
      }
    }
  }
  else
#endif /* USE_MB */
  {
    if (count > 0)
    {					// start counting from the beginning
      for (offset=0; ; offset+= delimiter_length)
      {
        if ((int) (offset= res->strstr(*delimiter, offset)) < 0)
        {
          str->copy(res->ptr(), res->length(), collation.collation);
          return str; // not found, return the original string
        }
        if (!--count)
        {
          str->copy(res->ptr(), offset, collation.collation);
          return str;
        }
      }
    }
    else
    {
      /*
        Negative index, start counting at the end
      */
      for (offset=res->length(); offset ;)
      {
        /* 
          this call will result in finding the position pointing to one 
          address space less than where the found substring is located
          in res
        */
        if ((int) (offset= res->strrstr(*delimiter, offset)) < 0)
        {
          str->copy(res->ptr(), res->length(), collation.collation);
          return str; // not found, return the original string
        }
        /*
          At this point, we've searched for the substring
          the number of times as supplied by the index value
        */
        if (!++count)
        {
          offset+= delimiter_length;
          str->copy(res->ptr() + offset, res->length() - offset,
                    collation.collation);
          return str;
        }
      }
      if (count)
      {
        str->copy(res->ptr(), res->length(), collation.collation);
        return str; // not found, return the original string
      }
    }
  }
  DBUG_ASSERT(0);
  return NULL;
}

/*
** The trim functions are extension to ANSI SQL because they trim substrings
** They ltrim() and rtrim() functions are optimized for 1 byte strings
** They also return the original string if possible, else they return
** a substring that points at the original string.
*/


String *Item_func_ltrim::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  char buff[MAX_FIELD_WIDTH], *ptr, *end;
  String tmp(buff,sizeof(buff),system_charset_info);
  String *res, *remove_str;
  uint UNINIT_VAR(remove_length);

  res= args[0]->val_str(str);
  if ((null_value=args[0]->null_value))
    return 0;
  remove_str= &remove;                          /* Default value. */
  if (arg_count == 2)
  {
    remove_str= args[1]->val_str(&tmp);
    if ((null_value= args[1]->null_value))
      return 0;
  }

  if ((remove_length= remove_str->length()) == 0 ||
      remove_length > res->length())
    return non_trimmed_value(res);

  ptr= (char*) res->ptr();
  end= ptr+res->length();
  if (remove_length == 1)
  {
    char chr=(*remove_str)[0];
    while (ptr != end && *ptr == chr)
      ptr++;
  }
  else
  {
    const char *r_ptr=remove_str->ptr();
    end-=remove_length;
    while (ptr <= end && !memcmp(ptr, r_ptr, remove_length))
      ptr+=remove_length;
    end+=remove_length;
  }
  if (ptr == res->ptr())
    return non_trimmed_value(res);
  return trimmed_value(res, (uint32) (ptr - res->ptr()), (uint32) (end - ptr));
}


String *Item_func_rtrim::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  char buff[MAX_FIELD_WIDTH], *ptr, *end;
  String tmp(buff, sizeof(buff), system_charset_info);
  String *res, *remove_str;
  uint UNINIT_VAR(remove_length);

  res= args[0]->val_str(str);
  if ((null_value=args[0]->null_value))
    return 0;
  remove_str= &remove;                          /* Default value. */
  if (arg_count == 2)
  {
    remove_str= args[1]->val_str(&tmp);
    if ((null_value= args[1]->null_value))
      return 0;
  }

  if ((remove_length= remove_str->length()) == 0 ||
      remove_length > res->length())
    return non_trimmed_value(res);

  ptr= (char*) res->ptr();
  end= ptr+res->length();
#ifdef USE_MB
  char *p=ptr;
  uint32 l;
#endif
  if (remove_length == 1)
  {
    char chr=(*remove_str)[0];
#ifdef USE_MB
    if (collation.collation->use_mb())
    {
      while (ptr < end)
      {
	if ((l= my_ismbchar(collation.collation, ptr, end))) ptr+= l, p=ptr;
	else ++ptr;
      }
      ptr=p;
    }
#endif
    while (ptr != end  && end[-1] == chr)
      end--;
  }
  else
  {
    const char *r_ptr=remove_str->ptr();
#ifdef USE_MB
    if (collation.collation->use_mb())
    {
  loop:
      while (ptr + remove_length < end)
      {
	if ((l= my_ismbchar(collation.collation, ptr, end))) ptr+= l;
	else ++ptr;
      }
      if (ptr + remove_length == end && !memcmp(ptr,r_ptr,remove_length))
      {
	end-=remove_length;
	ptr=p;
	goto loop;
      }
    }
    else
#endif /* USE_MB */
    {
      while (ptr + remove_length <= end &&
	     !memcmp(end-remove_length, r_ptr, remove_length))
	end-=remove_length;
    }
  }
  if (end == res->ptr()+res->length())
    return non_trimmed_value(res);
  return trimmed_value(res, 0, (uint32) (end - res->ptr()));
}


String *Item_func_trim::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  char buff[MAX_FIELD_WIDTH], *ptr, *end;
  const char *r_ptr;
  String tmp(buff, sizeof(buff), system_charset_info);
  String *res, *remove_str;
  uint UNINIT_VAR(remove_length);

  res= args[0]->val_str(str);
  if ((null_value=args[0]->null_value))
    return 0;
  remove_str= &remove;                          /* Default value. */
  if (arg_count == 2)
  {
    remove_str= args[1]->val_str(&tmp);
    if ((null_value= args[1]->null_value))
      return 0;
  }

  if ((remove_length= remove_str->length()) == 0 ||
      remove_length > res->length())
    return non_trimmed_value(res);

  ptr= (char*) res->ptr();
  end= ptr+res->length();
  r_ptr= remove_str->ptr();
  while (ptr+remove_length <= end && !memcmp(ptr,r_ptr,remove_length))
    ptr+=remove_length;
#ifdef USE_MB
  if (collation.collation->use_mb())
  {
    char *p=ptr;
    uint32 l;
 loop:
    while (ptr + remove_length < end)
    {
      if ((l= my_ismbchar(collation.collation, ptr, end)))
        ptr+= l;
      else
        ++ptr;
    }
    if (ptr + remove_length == end && !memcmp(ptr,r_ptr,remove_length))
    {
      end-=remove_length;
      ptr=p;
      goto loop;
    }
    ptr=p;
  }
  else
#endif /* USE_MB */
  {
    while (ptr + remove_length <= end &&
	   !memcmp(end-remove_length,r_ptr,remove_length))
      end-=remove_length;
  }
  if (ptr == res->ptr() && end == ptr+res->length())
    return non_trimmed_value(res);
  return trimmed_value(res, (uint32) (ptr - res->ptr()), (uint32) (end - ptr));
}

bool Item_func_trim::fix_length_and_dec()
{
  if (arg_count == 1)
  {
    if (agg_arg_charsets_for_string_result(collation, args, 1))
      return TRUE;
    DBUG_ASSERT(collation.collation != NULL);
    remove.set_charset(collation.collation);
    remove.set_ascii(" ",1);
  }
  else
  {
    // Handle character set for args[1] and args[0].
    // Note that we pass args[1] as the first item, and args[0] as the second.
    if (agg_arg_charsets_for_string_result_with_comparison(collation,
                                                           &args[1], 2, -1))
      return TRUE;
  }
  fix_char_length(args[0]->max_char_length());
  return FALSE;
}

void Item_func_trim::print(String *str, enum_query_type query_type)
{
  if (arg_count == 1)
  {
    Item_func::print(str, query_type);
    return;
  }
  str->append(Item_func_trim::func_name_cstring());
  str->append(func_name_ext());
  str->append('(');
  str->append(mode_name());
  str->append(' ');
  args[1]->print(str, query_type);
  str->append(STRING_WITH_LEN(" from "));
  args[0]->print(str, query_type);
  str->append(')');
}


/*
  RTRIM(expr)
  TRIM(TRAILING ' ' FROM expr)
  remove argument's soft dependency on PAD_CHAR_TO_FULL_LENGTH:
*/
Sql_mode_dependency Item_func_trim::value_depends_on_sql_mode() const
{
  DBUG_ASSERT(fixed());
  if (arg_count == 1) // RTRIM(expr)
    return (args[0]->value_depends_on_sql_mode() &
            Sql_mode_dependency(~0, ~MODE_PAD_CHAR_TO_FULL_LENGTH)).
           soft_to_hard();
  // TRIM(... FROM expr)
  DBUG_ASSERT(arg_count == 2);
  if (!args[1]->value_depends_on_sql_mode_const_item())
    return Item_func::value_depends_on_sql_mode();
  StringBuffer<64> trimstrbuf;
  String *trimstr= args[1]->val_str(&trimstrbuf);
  if (!trimstr)
    return Sql_mode_dependency();                  // will return NULL
  if (trimstr->length() == 0)
    return Item_func::value_depends_on_sql_mode(); // will trim nothing
  if (trimstr->lengthsp() != 0)
    return Item_func::value_depends_on_sql_mode(); // will trim not only spaces
  if (trimstr->length() > trimstr->charset()->mbminlen ||
      trimstr->numchars() > 1)
    return Item_func::value_depends_on_sql_mode(); // more than one space
  // TRIM(TRAILING ' ' FROM expr)
  return ((args[0]->value_depends_on_sql_mode() |
           args[1]->value_depends_on_sql_mode()) &
          Sql_mode_dependency(~0, ~MODE_PAD_CHAR_TO_FULL_LENGTH)).
         soft_to_hard();
}


/* Item_func_password */

bool Item_func_password::fix_fields(THD *thd, Item **ref)
{
  if (deflt)
    alg= (thd->variables.old_passwords ? OLD : NEW);
  return Item_str_ascii_func::fix_fields(thd, ref);
}

String *Item_func_password::val_str_ascii(String *str)
{
  DBUG_ASSERT(fixed());
  String *res= args[0]->val_str(str); 
  switch (alg){
  case NEW:
    if (args[0]->null_value || res->length() == 0)
      return make_empty_result();
    my_make_scrambled_password(tmp_value, res->ptr(), res->length());
    str->set(tmp_value, SCRAMBLED_PASSWORD_CHAR_LENGTH, &my_charset_latin1);
    break;
  case OLD:
    if ((null_value=args[0]->null_value))
      return 0;
    if (res->length() == 0)
      return make_empty_result();
    my_make_scrambled_password_323(tmp_value, res->ptr(), res->length());
    str->set(tmp_value, SCRAMBLED_PASSWORD_CHAR_LENGTH_323, &my_charset_latin1);
    break;
  default:
    DBUG_ASSERT(0);
  }
  return str;
}

char *Item_func_password::alloc(THD *thd, const char *password,
                                size_t pass_len, enum PW_Alg al)
{
  char *buff= (char *) thd->alloc((al==NEW)?
                                  SCRAMBLED_PASSWORD_CHAR_LENGTH + 1:
                                  SCRAMBLED_PASSWORD_CHAR_LENGTH_323 + 1);
  if (!buff)
    return NULL;

  switch (al) {
  case NEW:
    my_make_scrambled_password(buff, password, pass_len);
    break;
  case OLD:
    my_make_scrambled_password_323(buff, password, pass_len);
    break;
  default:
    DBUG_ASSERT(0);
  }
  return buff;
}



#define bin_to_ascii(c) ((c)>=38?((c)-38+'a'):(c)>=12?((c)-12+'A'):(c)+'.')

String *Item_func_encrypt::val_str(String *str)
{
  DBUG_ASSERT(fixed());
#ifdef HAVE_CRYPT
  String *res  =args[0]->val_str(str);
  char salt[3],*salt_ptr;
  if ((null_value=args[0]->null_value))
    return 0;
  if (res->length() == 0)
    return make_empty_result();
  if (arg_count == 1)
  {					// generate random salt
    time_t timestamp=current_thd->query_start();
    salt[0] = bin_to_ascii( (ulong) timestamp & 0x3f);
    salt[1] = bin_to_ascii(( (ulong) timestamp >> 5) & 0x3f);
    salt[2] = 0;
    salt_ptr=salt;
  }
  else
  {					// obtain salt from the first two bytes
    String *salt_str=args[1]->val_str(&tmp_value);
    if ((null_value= (args[1]->null_value || salt_str->length() < 2)))
      return 0;
    salt_ptr= salt_str->c_ptr_safe();
  }
  mysql_mutex_lock(&LOCK_crypt);
  char *tmp= crypt(res->c_ptr_safe(),salt_ptr);
  if (!tmp)
  {
    mysql_mutex_unlock(&LOCK_crypt);
    null_value= 1;
    return 0;
  }
  str->set(tmp, (uint) strlen(tmp), &my_charset_bin);
  str->copy();
  mysql_mutex_unlock(&LOCK_crypt);
  return str;
#else
  null_value=1;
  return 0;
#endif	/* HAVE_CRYPT */
}

bool Item_func_encode::seed()
{
  char buf[80];
  ulong rand_nr[2];
  String *key, tmp(buf, sizeof(buf), system_charset_info);

  if (!(key= args[1]->val_str(&tmp)))
    return TRUE;

  hash_password(rand_nr, key->ptr(), key->length());
  sql_crypt.init(rand_nr);

  return FALSE;
}

bool Item_func_encode::fix_length_and_dec()
{
  max_length=args[0]->max_length;
  base_flags|= ((args[0]->base_flags | args[1]->base_flags) &
                item_base_t::MAYBE_NULL);
  collation.set(&my_charset_bin);
  /* Precompute the seed state if the item is constant. */
  seeded= args[1]->const_item() &&
          (args[1]->result_type() == STRING_RESULT) && !seed();
  return FALSE;
}

String *Item_func_encode::val_str(String *str)
{
  String *res;
  DBUG_ASSERT(fixed());

  if (!(res=args[0]->val_str(str)))
  {
    null_value= 1;
    return NULL;
  }

  if (!seeded && seed())
  {
    null_value= 1;
    return NULL;
  }

  null_value= 0;
  res= copy_if_not_alloced(str, res, res->length());
  crypto_transform(res);
  sql_crypt.reinit();

  return res;
}

void Item_func_encode::crypto_transform(String *res)
{
  sql_crypt.encode((char*) res->ptr(),res->length());
  res->set_charset(&my_charset_bin);
}

void Item_func_decode::crypto_transform(String *res)
{
  sql_crypt.decode((char*) res->ptr(),res->length());
}


String *Item_func_database::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  if (thd->db.str == NULL)
  {
    null_value= 1;
    return 0;
  }
  else
    str->copy(thd->db.str, thd->db.length, system_charset_info);
  null_value= 0;
  return str;
}


String *Item_func_sqlerrm::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(!null_value);
  Diagnostics_area::Sql_condition_iterator it=
    current_thd->get_stmt_da()->sql_conditions();
  const Sql_condition *err;
  if ((err= it++))
  {
    str->copy(err->get_message_text(), err->get_message_octet_length(),
              system_charset_info);
    return str;
  }
  str->copy(STRING_WITH_LEN("normal, successful completion"),
            system_charset_info);
  return str;
}


/**
  @note USER() is replicated correctly if binlog_format=ROW or (as of
  BUG#28086) binlog_format=MIXED, but is incorrectly replicated to ''
  if binlog_format=STATEMENT.
*/
bool Item_func_user::init(const char *user, const char *host)
{
  DBUG_ASSERT(fixed());

  // For system threads (e.g. replication SQL thread) user may be empty
  if (user)
  {
    CHARSET_INFO *cs= str_value.charset();
    size_t res_length= (strlen(user)+strlen(host)+2) * cs->mbmaxlen;

    if (str_value.alloc((uint) res_length))
    {
      null_value=1;
      return TRUE;
    }

    res_length=cs->cset->snprintf(cs, (char*)str_value.ptr(), (uint) res_length,
                                  "%s@%s", user, host);
    str_value.length((uint) res_length);
    str_value.mark_as_const();
  }
  return FALSE;
}


Item *Item_func_sysconst::safe_charset_converter(THD *thd, CHARSET_INFO *tocs)
{
  /*
   During view or prepared statement creation, the item should not
   make use of const_charset_converter as it would imply substitution
   with constant items which is not correct. Functions can have different
   values during view creation and view execution based on context.

   Return the identical item during view creation and prepare.
  */
  if (thd->lex->is_ps_or_view_context_analysis())
    return this;
  return const_charset_converter(thd, tocs, true, fully_qualified_func_name());
}

bool Item_func_sysconst::const_item() const
{
  if (current_thd->lex->is_ps_or_view_context_analysis())
    return false;
  return true;
}

bool Item_func_user::fix_fields(THD *thd, Item **ref)
{
  return (Item_func_sysconst::fix_fields(thd, ref) ||
          init(thd->main_security_ctx.user,
               thd->main_security_ctx.host_or_ip));
}


bool Item_func_current_user::fix_fields(THD *thd, Item **ref)
{
  if (Item_func_sysconst::fix_fields(thd, ref))
    return TRUE;

  Security_context *ctx= context && context->security_ctx
                          ? context->security_ctx : thd->security_ctx;
  return init(ctx->priv_user, ctx->priv_host);
}

bool Item_func_current_role::fix_fields(THD *thd, Item **ref)
{
  if (Item_func_sysconst::fix_fields(thd, ref))
    return 1;

  Security_context *ctx= context && context->security_ctx
                          ? context->security_ctx : thd->security_ctx;
  if (ctx->priv_role[0])
  {
    if (str_value.copy(ctx->priv_role, strlen(ctx->priv_role),
                       system_charset_info))
      return 1;
    str_value.mark_as_const();
    null_value= 0;
    base_flags&= ~item_base_t::MAYBE_NULL;
    return 0;
  }
  null_value= 1;
  set_maybe_null();
  return 0;
}

bool Item_func_soundex::fix_length_and_dec()
{
  uint32 char_length= args[0]->max_char_length();
  if (agg_arg_charsets_for_string_result(collation, args, 1))
    return TRUE;
  DBUG_ASSERT(collation.collation != NULL);
  set_if_bigger(char_length, 4);
  fix_char_length(char_length);
  return FALSE;
}


/**
  If alpha, map input letter to soundex code.
  If not alpha and remove_garbage is set then skip to next char
  else return 0
*/

static int soundex_toupper(int ch)
{
  return (ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch;
}


static char get_scode(int wc)
{
  int ch= soundex_toupper(wc);
  if (ch < 'A' || ch > 'Z')
  {
					// Thread extended alfa (country spec)
    return '0';				// as vokal
  }
  return(soundex_map[ch-'A']);
}


static bool my_uni_isalpha(int wc)
{
  /*
    Return true for all Basic Latin letters: a..z A..Z.
    Return true for all Unicode characters with code higher than U+00C0:
    - characters between 'z' and U+00C0 are controls and punctuations.
    - "U+00C0 LATIN CAPITAL LETTER A WITH GRAVE" is the first letter after 'z'.
  */
  return (wc >= 'a' && wc <= 'z') ||
         (wc >= 'A' && wc <= 'Z') ||
         (wc >= 0xC0);
}


String *Item_func_soundex::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String *res= args[0]->val_str(&tmp_value);
  char last_ch,ch;
  CHARSET_INFO *cs= collation.collation;
  my_wc_t wc;
  uint nchars;
  int rc;

  if ((null_value= args[0]->null_value))
    return 0; /* purecov: inspected */

  if (str->alloc(MY_MAX(res->length(), 4 * cs->mbminlen)))
    return &tmp_value; /* purecov: inspected */
  str->set_charset(collation.collation);
  char *to= (char *) str->ptr();
  char *to_end= to + str->alloced_length();
  char *from= (char *) res->ptr(), *end= from + res->length();
  
  for ( ; ; ) /* Skip pre-space */
  {
    if ((rc= cs->mb_wc(&wc, (uchar*) from, (uchar*) end)) <= 0)
      return make_empty_result(); /* EOL or invalid byte sequence */
    
    if (rc == 1 && cs->m_ctype)
    {
      /* Single byte letter found */
      if (my_isalpha(cs, *from))
      {
        last_ch= get_scode(*from);       // Code of the first letter
        *to++= soundex_toupper(*from++); // Copy first letter
        break;
      }
      from++;
    }
    else
    {
      from+= rc;
      if (my_uni_isalpha(wc))
      {
        /* Multibyte letter found */
        wc= soundex_toupper(wc);
        last_ch= get_scode(wc);     // Code of the first letter
        if ((rc= cs->wc_mb(wc, (uchar*) to, (uchar*) to_end)) <= 0)
        {
          /* Extra safety - should not really happen */
          DBUG_ASSERT(false);
          return make_empty_result();
        }
        to+= rc;
        break;
      }
    }
  }
  
  /*
     last_ch is now set to the first 'double-letter' check.
     loop on input letters until end of input
  */
  for (nchars= 1 ; ; )
  {
    if ((rc= cs->mb_wc(&wc, (uchar*) from, (uchar*) end)) <= 0)
      break; /* EOL or invalid byte sequence */

    if (rc == 1 && cs->m_ctype)
    {
      if (!my_isalpha(cs, *from++))
        continue;
    }
    else
    {
      from+= rc;
      if (!my_uni_isalpha(wc))
        continue;
    }
    
    ch= get_scode(wc);
    if ((ch != '0') && (ch != last_ch)) // if not skipped or double
    {
      // letter, copy to output
      if ((rc= cs->wc_mb((my_wc_t) ch, (uchar*) to, (uchar*) to_end)) <= 0)
      {
        // Extra safety - should not really happen
        DBUG_ASSERT(false);
        break;
      }
      to+= rc;
      nchars++;
      last_ch= ch;  // save code of last input letter
    }               // for next double-letter check
  }
  
  /* Pad up to 4 characters with DIGIT ZERO, if the string is shorter */
  if (nchars < 4) 
  {
    uint nbytes= (4 - nchars) * cs->mbminlen;
    cs->fill(to, nbytes, '0');
    to+= nbytes;
  }

  str->length((uint) (to - str->ptr()));
  return str;
}


/**
  Change a number to format '3,333,333,333.000'.

  This should be 'internationalized' sometimes.
*/

/*
  The maximum supported decimal scale:
  38 - starting from 10.2.1
  30 - before 10.2.1
*/
const int FORMAT_MAX_DECIMALS= 38;


bool Item_func_format::fix_length_and_dec()
{
  uint32 char_length= args[0]->type_handler()->Item_decimal_notation_int_digits(args[0]);
  uint dec= FORMAT_MAX_DECIMALS;
  /*
    Format can require one more integer digit if rounding happens:
      FORMAT(9.9,0) -> '10'
    Set need_extra_digit_for_rounding to true by default
    if args[0] has some decimals: if args[1] is not
    a constant, then format can potentially reduce
    the number of decimals and round to the next integer.
  */
  bool need_extra_digit_for_rounding= args[0]->decimals > 0;
  if (args[1]->can_eval_in_optimize())
  {
    Longlong_hybrid tmp= args[1]->to_longlong_hybrid();
    if (!args[1]->null_value)
    {
      dec= tmp.to_uint(FORMAT_MAX_DECIMALS);
      need_extra_digit_for_rounding= (dec < args[0]->decimals);
    }
  }
  /*
    In case of a data type with zero integer digits, e.g. DECIMAL(4,4),
    we'll print at least one integer digit.
  */
  if (need_extra_digit_for_rounding || !char_length)
    char_length++;
  uint32 max_sep_count= (char_length / 3) + (dec ? 1 : 0) + /*sign*/1;
  collation.set(default_charset());
  fix_char_length(char_length + max_sep_count + dec);
  if (arg_count == 3)
    locale= args[2]->basic_const_item() ? args[2]->locale_from_val_str() : NULL;
  else
    locale= &my_locale_en_US; /* Two arguments */
  return FALSE;
}


/**
  @todo
  This needs to be fixed for multi-byte character set where numbers
  are stored in more than one byte
*/

String *Item_func_format::val_str_ascii(String *str)
{
  uint32 str_length;
  /* Number of decimal digits */
  int dec;
  /* Number of characters used to represent the decimals, including '.' */
  uint32 dec_length;
  const MY_LOCALE *lc;
  DBUG_ASSERT(fixed());

  dec= (int) args[1]->val_int();
  if (args[1]->null_value)
  {
    null_value=1;
    return NULL;
  }

  lc= locale ? locale : args[2]->locale_from_val_str();

  dec= set_zone(dec, 0, FORMAT_MAX_DECIMALS);
  dec_length= dec ? dec+1 : 0;
  null_value=0;

  if (args[0]->result_type() == DECIMAL_RESULT ||
      args[0]->result_type() == INT_RESULT)
  {
    VDec res(args[0]);
    if ((null_value= res.is_null()))
      return 0; /* purecov: inspected */
    res.to_string_round(str, dec);
    str_length= str->length();
  }
  else
  {
    double nr= args[0]->val_real();
    if ((null_value=args[0]->null_value))
      return 0; /* purecov: inspected */
    nr= my_double_round(nr, (longlong) dec, FALSE, FALSE);
    str->set_fcvt(nr, dec);
    if (!std::isfinite(nr))
      return str;
    str_length=str->length();
  }
  /* We need this test to handle 'nan' and short values */
  if (lc->grouping[0] > 0 &&
      str_length >= dec_length + 1 + lc->grouping[0])
  {
    /* We need space for ',' between each group of digits as well. */
    char buf[2 * FLOATING_POINT_BUFFER];
    int count;
    const char *grouping= lc->grouping;
    char sign_length= *str->ptr() == '-' ? 1 : 0;
    const char *src= str->ptr() + str_length - dec_length - 1;
    const char *src_begin= str->ptr() + sign_length;
    char *dst= buf + sizeof(buf);
    
    /* Put the fractional part */
    if (dec)
    {
      dst-= (dec + 1);
      *dst= lc->decimal_point;
      memcpy(dst + 1, src + 2, dec);
    }
    
    /* Put the integer part with grouping */
    for (count= *grouping; src >= src_begin; count--)
    {
      /*
        When *grouping==0x80 (which means "end of grouping")
        count will be initialized to -1 and
        we'll never get into this "if" anymore.
      */
      if (count == 0)
      {
        *--dst= lc->thousand_sep;
        if (grouping[1])
          grouping++;
        count= *grouping;
      }
      DBUG_ASSERT(dst > buf);
      *--dst= *src--;
    }
    
    if (sign_length) /* Put '-' */
      *--dst= *str->ptr();
    
    /* Put the rest of the integer part without grouping */
    str->copy(dst, buf + sizeof(buf) - dst, &my_charset_latin1);
  }
  else if (dec_length && lc->decimal_point != '.')
  {
    /*
      For short values without thousands (<1000)
      replace decimal point to localized value.
    */
    DBUG_ASSERT(dec_length <= str_length);
    ((char*) str->ptr())[str_length - dec_length]= lc->decimal_point;
  }
  return str;
}


bool Item_func_elt::fix_length_and_dec()
{
  uint32 char_length= 0;
  decimals=0;

  if (agg_arg_charsets_for_string_result(collation, args + 1, arg_count - 1))
    return TRUE;

  for (uint i= 1 ; i < arg_count ; i++)
  {
    set_if_bigger(char_length, args[i]->max_char_length());
    set_if_bigger(decimals,args[i]->decimals);
  }
  fix_char_length(char_length);
  set_maybe_null(); // NULL if wrong first arg
  return FALSE;
}


double Item_func_elt::val_real()
{
  DBUG_ASSERT(fixed());
  uint tmp;
  null_value=1;
  if ((tmp=(uint) args[0]->val_int()) == 0 || tmp >= arg_count)
    return 0.0;
  double result= args[tmp]->val_real();
  null_value= args[tmp]->null_value;
  return result;
}


longlong Item_func_elt::val_int()
{
  DBUG_ASSERT(fixed());
  uint tmp;
  null_value=1;
  if ((tmp=(uint) args[0]->val_int()) == 0 || tmp >= arg_count)
    return 0;

  longlong result= args[tmp]->val_int();
  null_value= args[tmp]->null_value;
  return result;
}


String *Item_func_elt::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  uint tmp;
  null_value=1;
  if ((tmp=(uint) args[0]->val_int()) == 0 || tmp >= arg_count)
    return NULL;

  String *result= args[tmp]->val_str(str);
  if (result)
    result->set_charset(collation.collation);
  null_value= args[tmp]->null_value;
  return result;
}


bool Item_func_make_set::fix_length_and_dec()
{
  uint32 char_length= arg_count - 2; /* Separators */

  if (agg_arg_charsets_for_string_result(collation, args + 1, arg_count - 1))
    return TRUE;
  
  for (uint i=1 ; i < arg_count ; i++)
    char_length+= args[i]->max_char_length();
  fix_char_length(char_length);
  return FALSE;
}


String *Item_func_make_set::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  ulonglong bits;
  bool first_found=0;
  Item **ptr=args+1;
  String *result= make_empty_result();

  bits=args[0]->val_int();
  if ((null_value=args[0]->null_value))
    return NULL;

  if (arg_count < 65)
    bits &= ((ulonglong) 1 << (arg_count-1))-1;

  for (; bits; bits >>= 1, ptr++)
  {
    if (bits & 1)
    {
      String *res= (*ptr)->val_str(str);
      if (res)					// Skip nulls
      {
	if (!first_found)
	{					// First argument
	  first_found=1;
	  if (res != str)
	    result=res;				// Use original string
	  else
	  {
	    if (tmp_str.copy(*res))		// Don't use 'str'
              return make_empty_result();
	    result= &tmp_str;
	  }
	}
	else
	{
	  if (result != &tmp_str)
	  {					// Copy data to tmp_str
	    if (tmp_str.alloc(result->length()+res->length()+1) ||
		tmp_str.copy(*result))
              return make_empty_result();
	    result= &tmp_str;
	  }
	  if (tmp_str.append(STRING_WITH_LEN(","), &my_charset_bin) || tmp_str.append(*res))
            return make_empty_result();
	}
      }
    }
  }
  return result;
}


void Item_func_char::print(String *str, enum_query_type query_type)
{
  str->append(Item_func_char::func_name_cstring());
  str->append('(');
  print_args(str, 0, query_type);
  if (collation.collation != &my_charset_bin)
  {
    str->append(STRING_WITH_LEN(" using "));
    str->append(collation.collation->cs_name);
  }
  str->append(')');
}


String *Item_func_char::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  str->length(0);
  str->set_charset(collation.collation);
  for (uint i=0 ; i < arg_count ; i++)
  {
    int32 num=(int32) args[i]->val_int();
    if (!args[i]->null_value)
      append_char(str, num);
  }
  str->realloc(str->length());			// Add end 0 (for Purify)
  return check_well_formed_result(str);
}


void Item_func_char::append_char(String *str, int32 num)
{
  char tmp[4];
  if (num & 0xFF000000L)
  {
    mi_int4store(tmp, num);
    str->append(tmp, 4, &my_charset_bin);
  }
  else if (num & 0xFF0000L)
  {
    mi_int3store(tmp, num);
    str->append(tmp, 3, &my_charset_bin);
  }
  else if (num & 0xFF00L)
  {
    mi_int2store(tmp, num);
    str->append(tmp, 2, &my_charset_bin);
  }
  else
  {
    tmp[0]= (char) num;
    str->append(tmp, 1, &my_charset_bin);
  }
}


String *Item_func_chr::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  str->length(0);
  str->set_charset(collation.collation);
  int32 num=(int32) args[0]->val_int();
  if (!args[0]->null_value)
    append_char(str, num);
  else
  {
    null_value= 1;
    return 0;
  }
  str->realloc(str->length());			// Add end 0 (for Purify)
  return check_well_formed_result(str);
}


inline String* alloc_buffer(String *res,String *str,String *tmp_value,
			    ulong length)
{
  if (res->alloced_length() < length)
  {
    if (str->alloced_length() >= length)
    {
      (void) str->copy(*res);
      str->length(length);
      return str;
    }
    if (tmp_value->alloc(length))
      return 0;
    (void) tmp_value->copy(*res);
    tmp_value->length(length);
    return tmp_value;
  }
  res->length(length);
  return res;
}


bool Item_func_repeat::fix_length_and_dec()
{
  if (agg_arg_charsets_for_string_result(collation, args, 1))
    return TRUE;
  DBUG_ASSERT(collation.collation != NULL);
  if (args[1]->can_eval_in_optimize())
  {
    uint32 length= max_length_for_string(args[1]);
    ulonglong char_length= (ulonglong) args[0]->max_char_length() * length;
    fix_char_length_ulonglong(char_length);
    return false;
  }
  max_length= MAX_BLOB_WIDTH;
  set_maybe_null();
  return false;
}

/**
  Item_func_repeat::str is carefully written to avoid reallocs
  as much as possible at the cost of a local buffer
*/

String *Item_func_repeat::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  uint length,tot_length;
  char *to;
  /* must be longlong to avoid truncation */
  longlong count= args[1]->val_int();
  String *res= args[0]->val_str(str);

  if (args[0]->null_value || args[1]->null_value)
    goto err;				// string and/or delim are null
  null_value= 0;

  if (count <= 0 && (count == 0 || !args[1]->unsigned_flag))
    return make_empty_result();

  /* Assumes that the maximum length of a String is < INT_MAX32. */
  /* Bounds check on count:  If this is triggered, we will error. */
  if ((ulonglong) count > INT_MAX32)
    count= INT_MAX32;
  if (count == 1)			// To avoid reallocs
    return res;
  length=res->length();

  // Safe length check
  {
    THD *thd= current_thd;
    if (length > thd->variables.max_allowed_packet / (uint) count)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                          ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                          func_name(), thd->variables.max_allowed_packet);
      goto err;
    }
  }
  tot_length= length*(uint) count;
  if (!(res= alloc_buffer(res,str,&tmp_value,tot_length)))
    goto err;

  to=(char*) res->ptr()+length;
  while (--count)
  {
    memcpy(to,res->ptr(),length);
    to+=length;
  }
  return (res);

err:
  null_value=1;
  return 0;
}


bool Item_func_space::fix_length_and_dec()
{
  collation.set(default_charset(), DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
  if (args[0]->can_eval_in_optimize())
  {
    fix_char_length_ulonglong(max_length_for_string(args[0]));
    return false;
  }
  max_length= MAX_BLOB_WIDTH;
  set_maybe_null();
  return false;
}


String *Item_func_space::val_str(String *str)
{
  uint tot_length;
  longlong count= args[0]->val_int();
  CHARSET_INFO *cs= collation.collation;

  if (args[0]->null_value)
    goto err;				// string and/or delim are null
  null_value= 0;

  if (count <= 0 && (count == 0 || !args[0]->unsigned_flag))
    return make_empty_result();
  /*
   Assumes that the maximum length of a String is < INT_MAX32.
   Bounds check on count:  If this is triggered, we will error.
  */
  if ((ulonglong) count > INT_MAX32)
    count= INT_MAX32;

  // Safe length check
  tot_length= (uint) count * cs->mbminlen;
  {
    THD *thd= current_thd;
    if (tot_length > thd->variables.max_allowed_packet)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                          ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                          func_name(),
                          thd->variables.max_allowed_packet);
      goto err;
    }
  }
  if (str->alloc(tot_length))
    goto err;
  str->length(tot_length);
  str->set_charset(cs);
  cs->fill((char*) str->ptr(), tot_length, ' ');
  return str;

err:
  null_value= 1;
  return 0;
}


bool Item_func_binlog_gtid_pos::fix_length_and_dec()
{
  collation.set(system_charset_info);
  max_length= MAX_BLOB_WIDTH;
  set_maybe_null();
  return FALSE;
}


String *Item_func_binlog_gtid_pos::val_str(String *str)
{
  DBUG_ASSERT(fixed());
#ifndef HAVE_REPLICATION
  null_value= 0;
  str->copy("", 0, system_charset_info);
  return str;
#else
  String name_str, *name;
  longlong pos;

  if (args[0]->null_value || args[1]->null_value)
    goto err;

  name= args[0]->val_str(&name_str);
  pos= args[1]->val_int();

  if (pos < 0 || pos > UINT_MAX32)
    goto err;

  if (gtid_state_from_binlog_pos(name->c_ptr_safe(), (uint32)pos, str))
    goto err;
  null_value= 0;
  return str;

err:
  null_value= 1;
  return NULL;
#endif  /* !HAVE_REPLICATION */
}


static String *default_pad_str(String *pad_str, CHARSET_INFO *collation)
{
  pad_str->set_charset(collation);
  pad_str->length(0);
  pad_str->append(" ", 1);
  return pad_str;
}

bool Item_func_pad::fix_length_and_dec()
{
  if (arg_count == 3)
  {
    String *str;
    if (!args[2]->basic_const_item() || !(str= args[2]->val_str(&pad_str)) ||
        !str->length())
      set_maybe_null();
    // Handle character set for args[0] and args[2].
    if (agg_arg_charsets_for_string_result(collation, &args[0], 2, 2))
      return TRUE;
  }
  else
  {
    if (agg_arg_charsets_for_string_result(collation, &args[0], 1, 1))
      return TRUE;
    default_pad_str(&pad_str, collation.collation);
  }

  DBUG_ASSERT(collation.collation->mbmaxlen > 0);
  if (args[1]->can_eval_in_optimize())
  {
    fix_char_length_ulonglong(max_length_for_string(args[1]));
    return false;
  }
  max_length= MAX_BLOB_WIDTH;
  set_maybe_null();
  return false;
}


/*
  PAD(expr,length,' ')
  removes argument's soft dependency on PAD_CHAR_TO_FULL_LENGTH if the result
  is longer than the argument's maximim possible length.
*/
Sql_mode_dependency Item_func_rpad::value_depends_on_sql_mode() const
{
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(arg_count >= 2);
  if (!args[1]->value_depends_on_sql_mode_const_item() ||
          (arg_count == 3 && !args[2]->value_depends_on_sql_mode_const_item()))
    return Item_func::value_depends_on_sql_mode();
  Longlong_hybrid len= args[1]->to_longlong_hybrid();
  if (args[1]->null_value || len.neg())
    return Sql_mode_dependency();                  // will return NULL
  if (len.abs() > 0 && len.abs() < args[0]->max_char_length())
    return Item_func::value_depends_on_sql_mode();
  StringBuffer<64> padstrbuf;
  String *padstr= arg_count == 3 ? args[2]->val_str(&padstrbuf) :
                               default_pad_str(&padstrbuf, collation.collation);
  if (!padstr || !padstr->length())
    return Sql_mode_dependency();                  // will return NULL
  if (padstr->lengthsp() != 0)
    return Item_func::value_depends_on_sql_mode(); // will pad not only spaces
  // RPAD(expr, length, ' ')  -- with a long enough length
  return ((args[0]->value_depends_on_sql_mode() |
           args[1]->value_depends_on_sql_mode()) &
          Sql_mode_dependency(~0, ~MODE_PAD_CHAR_TO_FULL_LENGTH)).
         soft_to_hard();
}



String *Item_func_rpad::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  uint32 res_byte_length,res_char_length,pad_char_length,pad_byte_length;
  char *to;
  const char *ptr_pad;
  /* must be longlong to avoid truncation */
  longlong count= args[1]->val_int();
  longlong byte_count;
  String *res= args[0]->val_str(str);
  String *rpad= arg_count == 2 ? &pad_str : args[2]->val_str(&pad_str);

  if (!res || args[1]->null_value || !rpad || 
      ((count < 0) && !args[1]->unsigned_flag))
    goto err;

  null_value=0;

  if (count == 0)
    return make_empty_result();

  /* Assumes that the maximum length of a String is < INT_MAX32. */
  /* Set here so that rest of code sees out-of-bound value as such. */
  if ((ulonglong) count > INT_MAX32)
    count= INT_MAX32;
  /*
    There is one exception not handled (intentionally) by the character set
    aggregation code. If one string is strong side and is binary, and
    another one is weak side and is a multi-byte character string,
    then we need to operate on the second string in terms on bytes when
    calling ::numchars() and ::charpos(), rather than in terms of characters.
    Lets substitute its character set to binary.
  */
  if (collation.collation == &my_charset_bin)
  {
    res->set_charset(&my_charset_bin);
    rpad->set_charset(&my_charset_bin);
  }

  if (count <= (res_char_length= res->numchars()))
  {						// String to pad is big enough
    res->length(res->charpos((int) count));	// Shorten result if longer
    return (res);
  }

  byte_count= count * collation.collation->mbmaxlen;
  {
    THD *thd= current_thd;
    if ((ulonglong) byte_count > thd->variables.max_allowed_packet)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                          ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                          func_name(), thd->variables.max_allowed_packet);
      goto err;
    }
  }

  if (arg_count == 3)
  {
    if (args[2]->null_value || !(pad_char_length= rpad->numchars()))
      goto err;
  }
  else
    pad_char_length= 1; // Implicit space

  res_byte_length= res->length();	/* Must be done before alloc_buffer */
  if (!(res= alloc_buffer(res,str,&tmp_value, (ulong) byte_count)))
    goto err;

  to= (char*) res->ptr()+res_byte_length;
  ptr_pad=rpad->ptr();
  pad_byte_length= rpad->length();
  count-= res_char_length;
  for ( ; (uint32) count > pad_char_length; count-= pad_char_length)
  {
    memcpy(to,ptr_pad,pad_byte_length);
    to+= pad_byte_length;
  }
  if (count)
  {
    pad_byte_length= rpad->charpos((int) count);
    memcpy(to,ptr_pad,(size_t) pad_byte_length);
    to+= pad_byte_length;
  }
  res->length((uint) (to- (char*) res->ptr()));
  return (res);

 err:
  null_value=1;
  return 0;
}


String *Item_func_lpad::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  uint32 res_char_length,pad_char_length;
  /* must be longlong to avoid truncation */
  longlong count= args[1]->val_int();
  longlong byte_count;
  String *res= args[0]->val_str(&tmp_value);
  String *pad= arg_count == 2 ? &pad_str : args[2]->val_str(&pad_str);

  if (!res || args[1]->null_value || !pad ||  
      ((count < 0) && !args[1]->unsigned_flag))
    goto err;  

  null_value=0;

  if (count == 0)
    return make_empty_result();

  /* Assumes that the maximum length of a String is < INT_MAX32. */
  /* Set here so that rest of code sees out-of-bound value as such. */
  if ((ulonglong) count > INT_MAX32)
    count= INT_MAX32;

  /*
    There is one exception not handled (intentionally) by the character set
    aggregation code. If one string is strong side and is binary, and
    another one is weak side and is a multi-byte character string,
    then we need to operate on the second string in terms on bytes when
    calling ::numchars() and ::charpos(), rather than in terms of characters.
    Lets substitute its character set to binary.
  */
  if (collation.collation == &my_charset_bin)
  {
    res->set_charset(&my_charset_bin);
    pad->set_charset(&my_charset_bin);
  }

  res_char_length= res->numchars();

  if (count <= res_char_length)
  {
    res->length(res->charpos((int) count));
    return res;
  }
  
  byte_count= count * collation.collation->mbmaxlen;
  
  {
    THD *thd= current_thd;
    if ((ulonglong) byte_count > thd->variables.max_allowed_packet)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                          ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                          func_name(), thd->variables.max_allowed_packet);
      goto err;
    }
  }

  if (str->alloc((uint32) byte_count))
    goto err;

  if (arg_count == 3)
  {
    if (args[2]->null_value || !(pad_char_length= pad->numchars()))
      goto err;
  }
  else
    pad_char_length= 1; // Implicit space
  
  str->length(0);
  str->set_charset(collation.collation);
  count-= res_char_length;
  while (count >= pad_char_length)
  {
    str->append(*pad);
    count-= pad_char_length;
  }
  if (count > 0)
    str->append(pad->ptr(), pad->charpos((int) count), collation.collation);

  str->append(*res);
  null_value= 0;
  return str;

err:
  null_value= 1;
  return 0;
}


String *Item_func_conv::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String *res= args[0]->val_str(str);
  char *endptr,ans[65],*ptr;
  longlong dec;
  int from_base= (int) args[1]->val_int();
  int to_base= (int) args[2]->val_int();
  int err;

  // Note that abs(INT_MIN) is undefined.
  if (args[0]->null_value || args[1]->null_value || args[2]->null_value ||
      from_base == INT_MIN || to_base == INT_MIN ||
      abs(to_base) > 36 || abs(to_base) < 2 ||
      abs(from_base) > 36 || abs(from_base) < 2 || !(res->length()))
  {
    null_value= 1;
    return NULL;
  }
  null_value= 0;
  unsigned_flag= !(from_base < 0);

  if (args[0]->field_type() == MYSQL_TYPE_BIT) 
  {
    /* 
     Special case: The string representation of BIT doesn't resemble the
     decimal representation, so we shouldn't change it to string and then to
     decimal. 
    */
    dec= args[0]->val_int();
  }
  else
  {
    if (from_base < 0)
      dec= res->charset()->strntoll(res->ptr(), res->length(),
                                    -from_base, &endptr, &err);
    else
      dec= (longlong) res->charset()->strntoull(res->ptr(), res->length(),
                                                from_base, &endptr, &err);
  }

  if (!(ptr= longlong2str(dec, ans, to_base)) ||
      str->copy(ans, (uint32) (ptr - ans), default_charset()))
  {
    null_value= 1;
    return NULL;
  }
  return str;
}


/*
  This function is needed as Item_func_conc_charset stores cached values
  in str_value.
*/

int Item_func_conv_charset::save_in_field(Field *field, bool no_conversions)
{
  String *result;
  CHARSET_INFO *cs= collation.collation;

  result= val_str(&str_value);
  if (null_value)
    return set_field_to_null_with_conversions(field, no_conversions);

  /* NOTE: If null_value == FALSE, "result" must be not NULL.  */
  field->set_notnull();
  int error= field->store(result->ptr(),result->length(),cs);
  return error;
}


String *Item_func_conv_charset::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  if (use_cached_value)
    return null_value ? 0 : &str_value;
  String *arg= args[0]->val_str(&tmp_value);
  String_copier_for_item copier(current_thd);
  return ((null_value= args[0]->null_value ||
                       copier.copy_with_warn(collation.collation, str,
                                             arg->charset(), arg->ptr(),
                                             arg->length(), arg->length()))) ?
    0 : str;
}

bool Item_func_conv_charset::fix_length_and_dec()
{
  DBUG_ASSERT(collation.derivation == DERIVATION_IMPLICIT);
  fix_char_length(args[0]->max_char_length());
  return FALSE;
}

void Item_func_conv_charset::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("convert("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" using "));
  str->append(collation.collation->cs_name);
  str->append(')');
}

String *Item_func_set_collation::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  str=args[0]->val_str(str);
  if ((null_value=args[0]->null_value))
    return 0;
  str->set_charset(collation.collation);
  return str;
}

bool Item_func_set_collation::fix_length_and_dec()
{
  if (!my_charset_same(args[0]->collation.collation, m_set_collation))
  {
    my_error(ER_COLLATION_CHARSET_MISMATCH, MYF(0),
             m_set_collation->coll_name.str,
             args[0]->collation.collation->cs_name.str);
    return TRUE;
  }
  collation.set(m_set_collation, DERIVATION_EXPLICIT,
                args[0]->collation.repertoire);
  max_length= args[0]->max_length;
  return FALSE;
}


bool Item_func_set_collation::eq(const Item *item, bool binary_cmp) const
{
  return Item_func::eq(item, binary_cmp) &&
         collation.collation == item->collation.collation;
}


void Item_func_set_collation::print(String *str, enum_query_type query_type)
{
  args[0]->print_parenthesised(str, query_type, precedence());
  str->append(STRING_WITH_LEN(" collate "));
  str->append(m_set_collation->coll_name);
}

String *Item_func_charset::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  uint dummy_errors;

  CHARSET_INFO *cs= args[0]->charset_for_protocol(); 
  null_value= 0;
  str->copy(cs->cs_name.str, cs->cs_name.length,
	    &my_charset_latin1, collation.collation, &dummy_errors);
  return str;
}

String *Item_func_collation::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  uint dummy_errors;
  CHARSET_INFO *cs= args[0]->charset_for_protocol(); 

  null_value= 0;
  str->copy(cs->coll_name.str, cs->coll_name.length, &my_charset_latin1,
            collation.collation, &dummy_errors);
  return str;
}


bool Item_func_weight_string::fix_length_and_dec()
{
  CHARSET_INFO *cs= args[0]->collation.collation;
  collation.set(&my_charset_bin, args[0]->collation.derivation);
  weigth_flags= my_strxfrm_flag_normalize(weigth_flags, cs->levels_for_order);
  /* 
    Use result_length if it was given explicitly in constructor,
    otherwise calculate max_length using argument's max_length
    and "nweights".
  */
  if (!(max_length= result_length))
  {
    size_t char_length;
    char_length= ((cs->state & MY_CS_STRNXFRM_BAD_NWEIGHTS) || !nweights) ?
                 args[0]->max_char_length() : nweights * cs->levels_for_order;
    max_length= (uint32) cs->strnxfrmlen(char_length * cs->mbmaxlen);
  }
  set_maybe_null();
  return FALSE;
}


/* Return a weight_string according to collation */
String *Item_func_weight_string::val_str(String *str)
{
  String *res;
  CHARSET_INFO *cs= args[0]->collation.collation;
  size_t tmp_length, frm_length;
  DBUG_ASSERT(fixed());

  if (args[0]->result_type() != STRING_RESULT ||
      !(res= args[0]->val_str(&tmp_value)))
    goto nl;
  
  /*
    Use result_length if it was given in constructor
    explicitly, otherwise calculate result length
    from argument and "nweights".
  */
  if (!(tmp_length= result_length))
  {
    size_t char_length;
    if (cs->state & MY_CS_STRNXFRM_BAD_NWEIGHTS)
    {
      /*
        latin2_czech_cs and cp1250_czech_cs do not support
        the "nweights" limit in strnxfrm(). Use the full length.
      */
      char_length= res->length();
    }
    else
    {
      /*
        If we don't need to pad the result with spaces, then it should be
        OK to calculate character length of the argument approximately:
        "res->length() / cs->mbminlen" can return a number that is 
        bigger than the real number of characters in the string, so
        we'll allocate a little bit more memory but avoid calling
        the slow res->numchars().
        In case if we do need to pad with spaces, we call res->numchars()
        to know the true number of characters.
      */
      if (!(char_length= nweights))
        char_length= (weigth_flags & MY_STRXFRM_PAD_WITH_SPACE) ?
                      res->numchars() : (res->length() / cs->mbminlen);
    }
    tmp_length= cs->strnxfrmlen(char_length * cs->mbmaxlen);
  }

  {
    THD *thd= current_thd;
    if (tmp_length > current_thd->variables.max_allowed_packet)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                          ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                          func_name(),
                          thd->variables.max_allowed_packet);
      goto nl;
    }
  }

  if (str->alloc(tmp_length))
    goto nl;

  frm_length= cs->strnxfrm((char*) str->ptr(), tmp_length,
                           nweights ? nweights : (uint) tmp_length,
                           res->ptr(), res->length(),
                           weigth_flags);
  DBUG_ASSERT(frm_length <= tmp_length);

  str->length(frm_length);
  null_value= 0;
  return str;

nl:
  null_value= 1;
  return 0;
}


void Item_func_weight_string::print(String *str, enum_query_type query_type)
{
  str->append(func_name_cstring());
  str->append('(');
  args[0]->print(str, query_type);
  str->append(',');
  str->append_ulonglong(result_length);
  str->append(',');
  str->append_ulonglong(nweights);
  str->append(',');
  str->append_ulonglong(weigth_flags);
  str->append(')');
}


String *Item_func_hex::val_str_ascii_from_val_real(String *str)
{
  ulonglong dec;
  double val= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0;
  if ((val <= (double) LONGLONG_MIN) ||
      (val >= (double) (ulonglong) ULONGLONG_MAX))
    dec= ~(longlong) 0;
  else
    dec= (ulonglong) (val + (val > 0 ? 0.5 : -0.5));
  return str->set_hex(dec) ? make_empty_result() : str;
}


String *Item_func_hex::val_str_ascii_from_val_str(String *str)
{
  DBUG_ASSERT(&tmp_value != str);
  String *res= args[0]->val_str(&tmp_value);
  DBUG_ASSERT(res != str);
  if ((null_value= (res == NULL)))
    return NULL;
  return str->set_hex(res->ptr(), res->length()) ? make_empty_result() : str;
}


String *Item_func_hex::val_str_ascii_from_val_int(String *str)
{
  ulonglong dec= (ulonglong) args[0]->val_int();
  if ((null_value= args[0]->null_value))
    return 0;
  return str->set_hex(dec) ? make_empty_result() : str;
}


  /** Convert given hex string to a binary string. */

String *Item_func_unhex::val_str(String *str)
{
  const char *from, *end;
  char *to;
  String *res;
  uint length;
  DBUG_ASSERT(fixed());

  res= args[0]->val_str(&tmp_value);
  if (!res || str->alloc(length= (1+res->length())/2))
  {
    null_value=1;
    return 0;
  }

  from= res->ptr();
  null_value= 0;
  str->length(length);
  to= (char*) str->ptr();
  if (res->length() % 2)
  {
    int hex_char;
    *to++= hex_char= hexchar_to_int(*from++);
    if ((null_value= (hex_char == -1)))
      return 0;
  }
  for (end=res->ptr()+res->length(); from < end ; from+=2, to++)
  {
    int hex_char1, hex_char2;
    hex_char1= hexchar_to_int(from[0]);
    hex_char2= hexchar_to_int(from[1]);
    if ((null_value= (hex_char1 == -1 || hex_char2 == -1)))
      return 0;
    *to= (char) ((hex_char1 << 4) | hex_char2);
  }
  return str;
}


#ifndef DBUG_OFF
String *Item_func_like_range::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  longlong nbytes= args[1]->val_int();
  String *res= args[0]->val_str(str);
  size_t min_len, max_len;
  CHARSET_INFO *cs= collation.collation;

  if (!res || args[0]->null_value || args[1]->null_value ||
      nbytes < 0 || nbytes > MAX_BLOB_WIDTH ||
      min_str.alloc((size_t)nbytes) || max_str.alloc((size_t)nbytes))
    goto err;
  null_value=0;

  if (cs->like_range(res->ptr(), res->length(),
                     '\\', '_', '%', (size_t)nbytes,
                     (char*) min_str.ptr(), (char*) max_str.ptr(),
                     &min_len, &max_len))
    goto err;

  min_str.set_charset(collation.collation);
  max_str.set_charset(collation.collation);
  min_str.length(min_len);
  max_str.length(max_len);

  return is_min ? &min_str : &max_str;

err:
  null_value= 1;
  return 0;
}
#endif


void Item_func_binary::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as binary)"));
}


#include <my_dir.h>				// For my_stat

String *Item_load_file::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String *file_name;
  File file;
  MY_STAT stat_info;
  char path[FN_REFLEN];
  ulonglong file_size;
  DBUG_ENTER("load_file");

  if (!(file_name= args[0]->val_str(str))
#ifndef NO_EMBEDDED_ACCESS_CHECKS
      || !(current_thd->security_ctx->master_access & FILE_ACL)
#endif
      )
    goto err;

  (void) fn_format(path, file_name->c_ptr_safe(), mysql_real_data_home, "",
		   MY_RELATIVE_PATH | MY_UNPACK_FILENAME);

  /* Read only allowed from within dir specified by secure_file_priv */
  if (!is_secure_file_path(path))
    goto err;

  if (!mysql_file_stat(key_file_loadfile, path, &stat_info, MYF(0)))
    goto err;

  if (!(stat_info.st_mode & S_IROTH))
  {
    /* my_error(ER_TEXTFILE_NOT_READABLE, MYF(0), file_name->c_ptr()); */
    goto err;
  }
  file_size= stat_info.st_size;

  {
    THD *thd= current_thd;
    if (file_size >= thd->variables.max_allowed_packet)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                          ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                          func_name(), thd->variables.max_allowed_packet);
      goto err;
    }
  }
  if (tmp_value.alloc((ulong)file_size))
    goto err;
  if ((file= mysql_file_open(key_file_loadfile,
                             file_name->ptr(), O_RDONLY, MYF(0))) < 0)
    goto err;
  if (mysql_file_read(file, (uchar*) tmp_value.ptr(), (size_t)stat_info.st_size,
                      MYF(MY_NABP)))
  {
    mysql_file_close(file, MYF(0));
    goto err;
  }
  tmp_value.length((uint32)stat_info.st_size);
  mysql_file_close(file, MYF(0));
  null_value = 0;
  DBUG_RETURN(&tmp_value);

err:
  null_value = 1;
  DBUG_RETURN(0);
}


String* Item_func_export_set::val_str(String* str)
{
  DBUG_ASSERT(fixed());
  String yes_buf, no_buf, sep_buf;
  const ulonglong the_set = (ulonglong) args[0]->val_int();
  const String *yes= args[1]->val_str(&yes_buf);
  const String *no= args[2]->val_str(&no_buf);
  const String *sep= NULL;

  uint num_set_values = 64;
  str->length(0);
  str->set_charset(collation.collation);

  /* Check if some argument is a NULL value */
  if (args[0]->null_value || args[1]->null_value || args[2]->null_value)
  {
    null_value= true;
    return NULL;
  }
  /*
    Arg count can only be 3, 4 or 5 here. This is guaranteed from the
    grammar for EXPORT_SET()
  */
  switch(arg_count) {
  case 5:
    num_set_values = (uint) args[4]->val_int();
    if (num_set_values > 64)
      num_set_values=64;
    if (args[4]->null_value)
    {
      null_value= true;
      return NULL;
    }
    /* Fall through */
  case 4:
    if (!(sep = args[3]->val_str(&sep_buf)))	// Only true if NULL
    {
      null_value= true;
      return NULL;
    }
    break;
  case 3:
    {
      /* errors is not checked - assume "," can always be converted */
      uint errors;
      sep_buf.copy(STRING_WITH_LEN(","), &my_charset_bin,
                   collation.collation, &errors);
      sep = &sep_buf;
    }
    break;
  default:
    DBUG_ASSERT(0); // cannot happen
  }
  null_value= false;

  THD *thd= current_thd;
  const ulong max_allowed_packet= thd->variables.max_allowed_packet;
  const uint num_separators= num_set_values > 0 ? num_set_values - 1 : 0;
  const ulonglong max_total_length=
    num_set_values * MY_MAX(yes->length(), no->length()) +
    num_separators * sep->length();

  if (unlikely(max_total_length > max_allowed_packet))
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                        ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                        func_name(), max_allowed_packet);
    null_value= true;
    return NULL;
  }

  uint ix;
  ulonglong mask;
  for (ix= 0, mask=0x1; ix < num_set_values; ++ix, mask = (mask << 1))
  {
    if (the_set & mask)
      str->append(*yes);
    else
      str->append(*no);
    if (ix != num_separators)
      str->append(*sep);
  }
  return str;
}

bool Item_func_export_set::fix_length_and_dec()
{
  uint32 length= MY_MAX(args[1]->max_char_length(), args[2]->max_char_length());
  uint32 sep_length= (arg_count > 3 ? args[3]->max_char_length() : 1);

  if (agg_arg_charsets_for_string_result(collation,
                                         args + 1, MY_MIN(4, arg_count) - 1))
    return TRUE;
  fix_char_length(length * 64 + sep_length * 63);
  return FALSE;
}


#define get_esc_bit(mask, num) (1 & (*((mask) + ((num) >> 3))) >> ((num) & 7))

/**
  QUOTE() function returns argument string in single quotes suitable for
  using in a SQL statement.

  Adds a \\ before all characters that needs to be escaped in a SQL string.
  We also escape '^Z' (END-OF-FILE in windows) to avoid problems when
  running commands from a file in windows.

  This function is very useful when you want to generate SQL statements.

  @note
    QUOTE(NULL) returns the string 'NULL' (4 letters, without quotes).

  @retval
    str	   Quoted string
  @retval
    NULL	   Out of memory.
*/

String *Item_func_quote::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  /*
    Bit mask that has 1 for set for the position of the following characters:
    0, \, ' and ^Z
  */

  static uchar escmask[32]=
  {
    0x01, 0x00, 0x00, 0x04, 0x80, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };

  ulong max_allowed_packet= current_thd->variables.max_allowed_packet;
  char *from, *to, *end, *start;
  String *arg= args[0]->val_str(&tmp_value);
  uint arg_length, new_length;
  if (!arg)					// Null argument
  {
    /* Return the string 'NULL' */
    str->copy(STRING_WITH_LEN("NULL"), collation.collation);
    null_value= 0;
    return str;
  }

  arg_length= arg->length();

  if (collation.collation->mbmaxlen == 1)
  {
    new_length= arg_length + 2; /* for beginning and ending ' signs */
    for (from= (char*) arg->ptr(), end= from + arg_length; from < end; from++)
      new_length+= get_esc_bit(escmask, (uchar) *from);
    if (new_length > max_allowed_packet)
      goto toolong;
  }
  else
  {
    new_length= (arg_length * 2) +  /* For string characters */
                (2 * collation.collation->mbmaxlen); /* For quotes */
    set_if_smaller(new_length, max_allowed_packet);
  }

  if (str->alloc(new_length))
    goto null;

  if (collation.collation->mbmaxlen > 1)
  {
    CHARSET_INFO *cs= collation.collation;
    int mblen;
    uchar *to_end;
    to= (char*) str->ptr();
    to_end= (uchar*) to + new_length;

    /* Put leading quote */
    if ((mblen= cs->wc_mb('\'', (uchar *) to, to_end)) <= 0)
      goto toolong;
    to+= mblen;

    for (start= (char*) arg->ptr(), end= start + arg_length; start < end; )
    {
      my_wc_t wc;
      bool escape;
      if ((mblen= cs->mb_wc(&wc, (uchar*) start, (uchar*) end)) <= 0)
        goto null;
      start+= mblen;
      switch (wc) {
        case 0:      escape= 1; wc= '0'; break;
        case '\032': escape= 1; wc= 'Z'; break;
        case '\'':   escape= 1; break;
        case '\\':   escape= 1; break;
        default:     escape= 0; break;
      }
      if (escape)
      {
        if ((mblen= cs->wc_mb('\\', (uchar*) to, to_end)) <= 0)
          goto toolong;
        to+= mblen;
      }
      if ((mblen= cs->wc_mb(wc, (uchar*) to, to_end)) <= 0)
        goto toolong;
      to+= mblen;
    }

    /* Put trailing quote */
    if ((mblen= cs->wc_mb('\'', (uchar *) to, to_end)) <= 0)
      goto toolong;
    to+= mblen;
    new_length= (uint)(to - str->ptr());
    goto ret;
  }

  /*
    We replace characters from the end to the beginning
  */
  to= (char*) str->ptr() + new_length - 1;
  *to--= '\'';
  for (start= (char*) arg->ptr(),end= start + arg_length; end-- != start; to--)
  {
    /*
      We can't use the bitmask here as we want to replace \O and ^Z with 0
      and Z
    */
    switch (*end)  {
    case 0:
      *to--= '0';
      *to=   '\\';
      break;
    case '\032':
      *to--= 'Z';
      *to=   '\\';
      break;
    case '\'':
    case '\\':
      *to--= *end;
      *to=   '\\';
      break;
    default:
      *to= *end;
      break;
    }
  }
  *to= '\'';

ret:
  str->length(new_length);
  str->set_charset(collation.collation);
  null_value= 0;
  return str;

toolong:
  push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                      ER_THD(current_thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                      func_name(), max_allowed_packet);
null:
  null_value= 1;
  return 0;
}

longlong Item_func_uncompressed_length::val_int()
{
  DBUG_ASSERT(fixed());
  String *res= args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  if (res->is_empty()) return 0;

  /*
    If length is <= 4 bytes, data is corrupt. This is the best we can do
    to detect garbage input without decompressing it.
  */
  if (res->length() <= 4)
  {
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_ZLIB_Z_DATA_ERROR,
                        ER_THD(thd, ER_ZLIB_Z_DATA_ERROR));
    null_value= 1;
    return 0;
  }

 /*
    res->ptr() using is safe because we have tested that string is at least
    5 bytes long.
    res->c_ptr() is not used because:
      - we do not need \0 terminated string to get first 4 bytes
      - c_ptr() tests simbol after string end (uninitialized memory) which
        confuse valgrind
  */
  return uint4korr(res->ptr()) & 0x3FFFFFFF;
}

longlong Item_func_crc32::val_int()
{
  DBUG_ASSERT(fixed());
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  return (longlong) my_checksum(0L, (uchar*)res->ptr(), res->length());
}

#ifdef HAVE_COMPRESS
#include "zlib.h"

String *Item_func_compress::val_str(String *str)
{
  int err= Z_OK, code;
  size_t new_size;
  String *res;
  Byte *body;
  char *tmp, *last_char;
  DBUG_ASSERT(fixed());

  if (!(res= args[0]->val_str(&tmp_value)))
  {
    null_value= 1;
    return 0;
  }
  null_value= 0;
  if (res->is_empty()) return res;

  /*
    Citation from zlib.h (comment for compress function):

    Compresses the source buffer into the destination buffer.  sourceLen is
    the byte length of the source buffer. Upon entry, destLen is the total
    size of the destination buffer, which must be at least 0.1% larger than
    sourceLen plus 12 bytes.
    We assume here that the buffer can't grow more than .25 %.
  */
  new_size= res->length() + res->length() / 5 + 12;

  // Check new_size overflow: new_size <= res->length()
  if (((uint32) (new_size+5) <= res->length()) || 
      str->alloc((uint32) new_size + 4 + 1))
  {
    null_value= 1;
    return 0;
  }

  body= ((Byte*)str->ptr()) + 4;

  // As far as we have checked res->is_empty() we can use ptr()
  if ((err= my_compress_buffer(body, &new_size, (const uchar *)res->ptr(),
                               res->length())) != Z_OK)
  {
    THD *thd= current_thd;
    code= err==Z_MEM_ERROR ? ER_ZLIB_Z_MEM_ERROR : ER_ZLIB_Z_BUF_ERROR;
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN, code,
                 ER_THD(thd, code));
    null_value= 1;
    return 0;
  }

  tmp= (char*) str->ptr(); // int4store is a macro; avoid side effects
  int4store(tmp, res->length() & 0x3FFFFFFF);

  /* This is to ensure that things works for CHAR fields, which trim ' ': */
  last_char= ((char*)body)+new_size-1;
  if (*last_char == ' ')
  {
    *++last_char= '.';
    new_size++;
  }

  str->length((uint32)new_size + 4);
  return str;
}


String *Item_func_uncompress::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String *res= args[0]->val_str(&tmp_value);
  ulong new_size;
  int err;
  uint code;

  if (!res)
    goto err;
  null_value= 0;
  if (res->is_empty())
    return res;

  /* If length is less than 4 bytes, data is corrupt */
  if (res->length() <= 4)
  {
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ZLIB_Z_DATA_ERROR,
			ER_THD(thd, ER_ZLIB_Z_DATA_ERROR));
    goto err;
  }

  /* Size of uncompressed data is stored as first 4 bytes of field */
  new_size= uint4korr(res->ptr()) & 0x3FFFFFFF;
  if (new_size > current_thd->variables.max_allowed_packet)
  {
    THD *thd= current_thd;
    push_warning_printf(thd,Sql_condition::WARN_LEVEL_WARN,
			ER_TOO_BIG_FOR_UNCOMPRESS,
			ER_THD(thd, ER_TOO_BIG_FOR_UNCOMPRESS),
                        static_cast<int>(thd->variables.
                                         max_allowed_packet));
    goto err;
  }
  if (str->alloc((uint32)new_size))
    goto err;

  if ((err= uncompress((Byte*)str->ptr(), &new_size,
		       ((const Bytef*)res->ptr())+4,res->length()-4)) == Z_OK)
  {
    str->length((uint32) new_size);
    return str;
  }

  code= ((err == Z_BUF_ERROR) ? ER_ZLIB_Z_BUF_ERROR :
	 ((err == Z_MEM_ERROR) ? ER_ZLIB_Z_MEM_ERROR : ER_ZLIB_Z_DATA_ERROR));
  {
    THD *thd= current_thd;
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN, code, ER_THD(thd, code));
  }

err:
  null_value= 1;
  return 0;
}
#endif


String *Item_func_uuid::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  uchar guid[MY_UUID_SIZE];
  size_t length= (without_separators ?
                  MY_UUID_ORACLE_STRING_LENGTH :
                  MY_UUID_STRING_LENGTH);

  str->alloc(length+1);
  str->length(length);
  str->set_charset(system_charset_info);
  my_uuid(guid);
  if (without_separators)
    my_uuid2str_oracle(guid, (char *)str->ptr());
  else
    my_uuid2str(guid, (char *)str->ptr());
  return str;
}


Item_func_dyncol_create::Item_func_dyncol_create(THD *thd, List<Item> &args,
                                                 DYNCALL_CREATE_DEF *dfs):
  Item_str_func(thd, args), defs(dfs), vals(0), keys_num(NULL), keys_str(NULL),
  names(FALSE), force_names(FALSE)
{
  DBUG_ASSERT((args.elements & 0x1) == 0); // even number of arguments
}


bool Item_func_dyncol_create::fix_fields(THD *thd, Item **ref)
{
  uint i;
  bool res= Item_func::fix_fields(thd, ref); // no need Item_str_func here
  if (!res)
  {
    vals= (DYNAMIC_COLUMN_VALUE *) alloc_root(thd->mem_root,
                                              sizeof(DYNAMIC_COLUMN_VALUE) *
                                              (arg_count / 2));
    for (i= 0;
         i + 1 < arg_count && args[i]->result_type() == INT_RESULT;
         i+= 2)
      ;
    if (i + 1 < arg_count)
    {
      names= TRUE;
    }

    keys_num= (uint *) alloc_root(thd->mem_root,
                               (sizeof(LEX_STRING) > sizeof(uint) ?
                                sizeof(LEX_STRING) :
                                sizeof(uint)) *
                               (arg_count / 2));
    keys_str= (LEX_STRING *) keys_num;
    status_var_increment(thd->status_var.feature_dynamic_columns);
  }
  return res || vals == 0 || keys_num == 0;
}


bool Item_func_dyncol_create::fix_length_and_dec()
{
  max_length= MAX_BLOB_WIDTH;
  set_maybe_null();
  collation.set(&my_charset_bin);
  decimals= 0;
  return FALSE;
}

bool Item_func_dyncol_create::prepare_arguments(THD *thd, bool force_names_arg)
{
  char buff[STRING_BUFFER_USUAL_SIZE];
  String *res, tmp(buff, sizeof(buff), &my_charset_bin);
  uint column_count= (arg_count / 2);
  uint i;
  my_decimal dtmp, *dres;
  force_names= force_names_arg;

  if (!(names || force_names))
  {
    for (i= 0; i < column_count; i++)
    {
      uint valpos= i * 2 + 1;
      DYNAMIC_COLUMN_TYPE type= defs[i].type;
      if (type == DYN_COL_NULL)
        type= args[valpos]->type_handler()->dyncol_type(args[valpos]);
      if (type == DYN_COL_STRING &&
          args[valpos]->type() == Item::FUNC_ITEM &&
          ((Item_func *)args[valpos])->functype() == DYNCOL_FUNC)
      {
        force_names= 1;
        break;
      }
    }
  }

  /* get values */
  for (i= 0; i < column_count; i++)
  {
    uint valpos= i * 2 + 1;
    DYNAMIC_COLUMN_TYPE type= defs[i].type;
    if (type == DYN_COL_NULL) // auto detect
      type= args[valpos]->type_handler()->dyncol_type(args[valpos]);
    if (type == DYN_COL_STRING &&
        args[valpos]->type() == Item::FUNC_ITEM &&
        ((Item_func *)args[valpos])->functype() == DYNCOL_FUNC)
    {
      DBUG_ASSERT(names || force_names);
      type= DYN_COL_DYNCOL;
    }
    if (names || force_names)
    {
      res= args[i * 2]->val_str(&tmp);
      if (res)
      {
        // guaranty UTF-8 string for names
        if (my_charset_same(res->charset(), DYNCOL_UTF))
        {
          keys_str[i].length= res->length();
          keys_str[i].str= thd->strmake(res->ptr(), res->length());
        }
        else
        {
          uint strlen= res->length() * DYNCOL_UTF->mbmaxlen + 1;
          uint dummy_errors;
          if (char *str= (char *) thd->alloc(strlen))
          {
            keys_str[i].length=
              copy_and_convert(str, strlen, DYNCOL_UTF,
                               res->ptr(), res->length(), res->charset(),
                               &dummy_errors);
              keys_str[i].str= str;
          }
          else
            keys_str[i].length= 0;

        }
      }
      else
      {
        keys_str[i].length= 0;
        keys_str[i].str= NULL;
      }
    }
    else
      keys_num[i]= (uint) args[i * 2]->val_int();
    if (args[i * 2]->null_value)
    {
      /* to make cleanup possible */
      for (; i < column_count; i++)
        vals[i].type= DYN_COL_NULL;
      return 1;
    }
    vals[i].type= type;
    switch (type) {
    case DYN_COL_NULL:
      DBUG_ASSERT(args[valpos]->field_type() == MYSQL_TYPE_NULL);
      break;
    case DYN_COL_INT:
      vals[i].x.long_value= args[valpos]->val_int();
      break;
    case DYN_COL_UINT:
      vals[i].x.ulong_value= args[valpos]->val_int();
      break;
    case DYN_COL_DOUBLE:
      vals[i].x.double_value= args[valpos]->val_real();
      break;
    case DYN_COL_DYNCOL:
    case DYN_COL_STRING:
      res= args[valpos]->val_str(&tmp);
      if (res && defs[i].cs)
        res->set_charset(defs[i].cs);
      if (res &&
          (vals[i].x.string.value.str= thd->strmake(res->ptr(), res->length())))
      {
	vals[i].x.string.value.length= res->length();
	vals[i].x.string.charset= res->charset();
      }
      else
      {
        args[valpos]->null_value= 1;            // In case of out of memory
        vals[i].x.string.value.str= NULL;
        vals[i].x.string.value.length= 0;         // just to be safe
      }
      break;
    case DYN_COL_DECIMAL:
      if ((dres= args[valpos]->val_decimal(&dtmp)))
      {
	mariadb_dyncol_prepare_decimal(&vals[i]);
        DBUG_ASSERT(vals[i].x.decimal.value.len == dres->len);
        vals[i].x.decimal.value.intg= dres->intg;
        vals[i].x.decimal.value.frac= dres->frac;
        vals[i].x.decimal.value.sign= dres->sign();
        memcpy(vals[i].x.decimal.buffer, dres->buf,
               sizeof(vals[i].x.decimal.buffer));
      }
      else
      {
	mariadb_dyncol_prepare_decimal(&vals[i]); // just to be safe
        DBUG_ASSERT(args[valpos]->null_value);
      }
      break;
    case DYN_COL_DATETIME:
    case DYN_COL_DATE:
      args[valpos]->get_date(thd, &vals[i].x.time_value,
                             Datetime::Options(thd));
      break;
    case DYN_COL_TIME:
      args[valpos]->get_time(thd, &vals[i].x.time_value);
      break;
    default:
      DBUG_ASSERT(0);
      vals[i].type= DYN_COL_NULL;
    }
    if (vals[i].type != DYN_COL_NULL && args[valpos]->null_value)
    {
      vals[i].type= DYN_COL_NULL;
    }
  }
  return FALSE;
}


String *Item_func_dyncol_create::val_str(String *str)
{
  DYNAMIC_COLUMN col;
  String *res;
  uint column_count= (arg_count / 2);
  enum enum_dyncol_func_result rc;
  DBUG_ASSERT((arg_count & 0x1) == 0); // even number of arguments

  /* FIXME: add thd argument to Item::val_str() */
  if (prepare_arguments(current_thd, FALSE))
  {
    res= NULL;
    null_value= 1;
  }
  else
  {
    if ((rc= ((names || force_names) ?
              mariadb_dyncol_create_many_named(&col, column_count, keys_str,
                                               vals, TRUE) :
              mariadb_dyncol_create_many_num(&col, column_count, keys_num,
                                             vals, TRUE))))
    {
      dynamic_column_error_message(rc);
      mariadb_dyncol_free(&col);
      res= NULL;
      null_value= TRUE;
    }
    else
    {
      /* Move result from DYNAMIC_COLUMN to str_value */
      char *ptr;
      size_t length, alloc_length;
      dynstr_reassociate(&col, &ptr, &length, &alloc_length);
      str_value.reset(ptr, length, alloc_length, &my_charset_bin);
      res= &str_value;
      null_value= FALSE;
    }
  }

  return res;
}

void Item_func_dyncol_create::print_arguments(String *str,
                                              enum_query_type query_type)
{
  uint i;
  uint column_count= (arg_count / 2);
  for (i= 0; i < column_count; i++)
  {
    args[i*2]->print(str, query_type);
    str->append(',');
    args[i*2 + 1]->print(str, query_type);
    switch (defs[i].type) {
    case DYN_COL_NULL: // automatic type => write nothing
      break;
    case DYN_COL_INT:
      str->append(STRING_WITH_LEN(" AS int"));
      break;
    case DYN_COL_UINT:
      str->append(STRING_WITH_LEN(" AS unsigned int"));
      break;
    case DYN_COL_DOUBLE:
      str->append(STRING_WITH_LEN(" AS double"));
      break;
    case DYN_COL_DYNCOL:
    case DYN_COL_STRING:
      str->append(STRING_WITH_LEN(" AS char"));
      if (defs[i].cs)
      {
        str->append(STRING_WITH_LEN(" charset "));
        str->append(defs[i].cs->cs_name);
        str->append(' ');
      }
      break;
    case DYN_COL_DECIMAL:
      str->append(STRING_WITH_LEN(" AS decimal"));
      break;
    case DYN_COL_DATETIME:
      str->append(STRING_WITH_LEN(" AS datetime"));
      break;
    case DYN_COL_DATE:
      str->append(STRING_WITH_LEN(" AS date"));
      break;
    case DYN_COL_TIME:
      str->append(STRING_WITH_LEN(" AS time"));
      break;
    }
    if (i < column_count - 1)
      str->append(',');
  }
}


void Item_func_dyncol_create::print(String *str,
                                    enum_query_type query_type)
{
  DBUG_ASSERT((arg_count & 0x1) == 0); // even number of arguments
  str->append(STRING_WITH_LEN("column_create("));
  print_arguments(str, query_type);
  str->append(')');
}

String *Item_func_dyncol_json::val_str(String *str)
{
  DYNAMIC_STRING json, col;
  String *res;
  enum enum_dyncol_func_result rc;

  res= args[0]->val_str(str);
  if (args[0]->null_value)
    goto null;

  col.str= (char *)res->ptr();
  col.length= res->length();
  if ((rc= mariadb_dyncol_json(&col, &json)))
  {
    dynamic_column_error_message(rc);
    goto null;
  }
  bzero(&col, sizeof(col));
  {
    /* Move result from DYNAMIC_COLUMN to str */
    char *ptr;
    size_t length, alloc_length;
    dynstr_reassociate(&json, &ptr, &length, &alloc_length);
    str->reset(ptr, length, alloc_length, DYNCOL_UTF);
    null_value= FALSE;
  }
  str->set_charset(DYNCOL_UTF);
  return str;

null:
  bzero(&col, sizeof(col));
  null_value= TRUE;
  return NULL;
}

String *Item_func_dyncol_add::val_str(String *str)
{
  DYNAMIC_COLUMN col;
  String *res;
  uint column_count=  (arg_count / 2);
  enum enum_dyncol_func_result rc;
  DBUG_ASSERT((arg_count & 0x1) == 1); // odd number of arguments

  /* We store the packed data last */
  res= args[arg_count - 1]->val_str(str);
  if (args[arg_count - 1]->null_value ||
      init_dynamic_string(&col, NULL, res->length() + STRING_BUFFER_USUAL_SIZE,
                          STRING_BUFFER_USUAL_SIZE))
    goto null;

  col.length= res->length();
  memcpy(col.str, res->ptr(), col.length);

  /* FIXME: add thd argument to Item::val_str() */
  if (prepare_arguments(current_thd, mariadb_dyncol_has_names(&col)))
    goto null;

  if ((rc= ((names || force_names) ?
            mariadb_dyncol_update_many_named(&col, column_count,
                                             keys_str, vals) :
            mariadb_dyncol_update_many_num(&col, column_count,
                                           keys_num, vals))))
  {
    dynamic_column_error_message(rc);
    mariadb_dyncol_free(&col);
    goto null;
  }

  {
    /* Move result from DYNAMIC_COLUMN to str */
    char *ptr;
    size_t length, alloc_length;
    dynstr_reassociate(&col, &ptr, &length, &alloc_length);
    str->reset(ptr, length, alloc_length, &my_charset_bin);
    null_value= FALSE;
  }

  return str;

null:
  null_value= TRUE;
  return NULL;
}


void Item_func_dyncol_add::print(String *str,
                                 enum_query_type query_type)
{
  DBUG_ASSERT((arg_count & 0x1) == 1); // odd number of arguments
  str->append(STRING_WITH_LEN("column_add("));
  args[arg_count - 1]->print(str, query_type);
  str->append(',');
  print_arguments(str, query_type);
  str->append(')');
}


/**
  Get value for a column stored in a dynamic column

  @notes
  This function ensures that null_value is set correctly
*/

bool Item_dyncol_get::get_dyn_value(THD *thd, DYNAMIC_COLUMN_VALUE *val,
                                    String *tmp)
{
  DYNAMIC_COLUMN dyn_str;
  String *res;
  longlong num= 0;
  LEX_STRING buf, *name= NULL;
  char nmstrbuf[11];
  String nmbuf(nmstrbuf, sizeof(nmstrbuf), system_charset_info);
  enum enum_dyncol_func_result rc;

  if (args[1]->result_type() == INT_RESULT)
    num= args[1]->val_int();
  else
  {
    String *nm= args[1]->val_str(&nmbuf);
    if (!nm || args[1]->null_value)
    {
      null_value= 1;
      return 1;
    }

    if (my_charset_same(nm->charset(), DYNCOL_UTF))
    {
      buf.str= (char *) nm->ptr();
      buf.length= nm->length();
    }
    else
    {
      uint strlen= nm->length() * DYNCOL_UTF->mbmaxlen + 1;
      uint dummy_errors;
      buf.str= (char *) thd->alloc(strlen);
      if (buf.str)
      {
        buf.length=
          copy_and_convert(buf.str, strlen, DYNCOL_UTF,
                           nm->ptr(), nm->length(), nm->charset(),
                           &dummy_errors);
      }
      else
        buf.length= 0;
    }
    name= &buf;
  }


  if (args[1]->null_value || num < 0 || num > INT_MAX)
  {
    null_value= 1;
    return 1;
  }

  res= args[0]->val_str(tmp);
  if (args[0]->null_value)
  {
    null_value= 1;
    return 1;
  }

  dyn_str.str=   (char*) res->ptr();
  dyn_str.length= res->length();
  if ((rc= ((name == NULL) ?
            mariadb_dyncol_get_num(&dyn_str, (uint) num, val) :
            mariadb_dyncol_get_named(&dyn_str, name, val))))
  {
    dynamic_column_error_message(rc);
    null_value= 1;
    return 1;
  }

  null_value= 0;
  return 0;                                     // ok
}


String *Item_dyncol_get::val_str(String *str_result)
{
  DYNAMIC_COLUMN_VALUE val;
  char buff[STRING_BUFFER_USUAL_SIZE];
  String tmp(buff, sizeof(buff), &my_charset_bin);

  if (get_dyn_value(current_thd, &val, &tmp))
    return NULL;

  switch (val.type) {
  case DYN_COL_NULL:
    goto null;
  case DYN_COL_INT:
  case DYN_COL_UINT:
    str_result->set_int(val.x.long_value, MY_TEST(val.type == DYN_COL_UINT),
                       &my_charset_latin1);
    break;
  case DYN_COL_DOUBLE:
    str_result->set_real(val.x.double_value, NOT_FIXED_DEC, &my_charset_latin1);
    break;
  case DYN_COL_DYNCOL:
  case DYN_COL_STRING:
    if ((char*) tmp.ptr() <= val.x.string.value.str &&
        (char*) tmp.ptr() + tmp.length() >= val.x.string.value.str)
    {
      /* value is allocated in tmp buffer; We have to make a copy */
      str_result->copy(val.x.string.value.str, val.x.string.value.length,
                      val.x.string.charset);
    }
    else
    {
      /*
        It's safe to use the current value because it's either pointing
        into a field or in a buffer for another item and this buffer
        is not going to be deleted during expression evaluation
      */
      str_result->set(val.x.string.value.str, val.x.string.value.length,
                      val.x.string.charset);
    }
    break;
  case DYN_COL_DECIMAL:
  {
    int res;
    int length= decimal_string_size(&val.x.decimal.value);
    if (str_result->alloc(length))
      goto null;
    if ((res= decimal2string(&val.x.decimal.value, (char*) str_result->ptr(),
                             &length, 0, 0, ' ')) != E_DEC_OK)
    {
      char buff[40];
      int len= sizeof(buff);
      DBUG_ASSERT(length < (int)sizeof(buff));
      decimal2string(&val.x.decimal.value, buff, &len, 0, 0, ' ');
      decimal_operation_results(res, buff, "CHAR");
    }
    str_result->set_charset(&my_charset_latin1);
    str_result->length(length);
    break;
  }
  case DYN_COL_DATETIME:
  case DYN_COL_DATE:
  case DYN_COL_TIME:
  {
    int length;
    /*
      We use AUTO_SEC_PART_DIGITS here to ensure that we do not loose
      any microseconds from the data. This is safe to do as we are
      asked to return the time argument as a string.
    */
    if (str_result->alloc(MAX_DATE_STRING_REP_LENGTH) ||
        !(length= my_TIME_to_str(&val.x.time_value, (char*) str_result->ptr(),
                                 AUTO_SEC_PART_DIGITS)))
      goto null;
    str_result->set_charset(&my_charset_latin1);
    str_result->length(length);
    break;
  }
  }
  return str_result;

null:
  null_value= TRUE;
  return 0;
}


longlong Item_dyncol_get::val_int()
{
  THD *thd= current_thd;
  DYNAMIC_COLUMN_VALUE val;
  char buff[STRING_BUFFER_USUAL_SIZE];
  String tmp(buff, sizeof(buff), &my_charset_bin);

  if (get_dyn_value(thd, &val, &tmp))
    return 0;

  switch (val.type) {
  case DYN_COL_DYNCOL:
  case DYN_COL_NULL:
    goto null;
  case DYN_COL_UINT:
    unsigned_flag= 1;            // Make it possible for caller to detect sign
    return val.x.long_value;
  case DYN_COL_INT:
    unsigned_flag= 0;            // Make it possible for caller to detect sign
    return val.x.long_value;
  case DYN_COL_DOUBLE:
    return Converter_double_to_longlong_with_warn(thd, val.x.double_value,
                                                  unsigned_flag).result();
  case DYN_COL_STRING:
  {
    int error;
    longlong num;
    char *end= val.x.string.value.str + val.x.string.value.length, *org_end= end;

    num= my_strtoll10(val.x.string.value.str, &end, &error);
    if (unlikely(end != org_end || error > 0))
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_BAD_DATA,
                          ER_THD(thd, ER_BAD_DATA),
                          ErrConvString(val.x.string.value.str,
                                        val.x.string.value.length,
                                        val.x.string.charset).ptr(),
                          unsigned_flag ? "UNSIGNED INT" : "INT");
    }
    unsigned_flag= error >= 0;
    return num;
  }
  case DYN_COL_DECIMAL:
  {
    longlong num;
    my_decimal2int(E_DEC_FATAL_ERROR, &val.x.decimal.value, unsigned_flag,
                   &num);
    return num;
  }
  case DYN_COL_DATETIME:
  case DYN_COL_DATE:
  case DYN_COL_TIME:
    unsigned_flag= !val.x.time_value.neg;
    if (unsigned_flag)
      return TIME_to_ulonglong(&val.x.time_value);
    else
      return -(longlong)TIME_to_ulonglong(&val.x.time_value);
  }

null:
  null_value= TRUE;
  return 0;
}


double Item_dyncol_get::val_real()
{
  THD *thd= current_thd;
  DYNAMIC_COLUMN_VALUE val;
  char buff[STRING_BUFFER_USUAL_SIZE];
  String tmp(buff, sizeof(buff), &my_charset_bin);

  if (get_dyn_value(thd, &val, &tmp))
    return 0.0;

  switch (val.type) {
  case DYN_COL_DYNCOL:
  case DYN_COL_NULL:
    goto null;
  case DYN_COL_UINT:
    return ulonglong2double(val.x.ulong_value);
  case DYN_COL_INT:
    return (double) val.x.long_value;
  case DYN_COL_DOUBLE:
    return (double) val.x.double_value;
  case DYN_COL_STRING:
  {
    int error;
    char *end;
    double res= val.x.string.charset->strntod((char*) val.x.string.value.str,
                                              val.x.string.value.length, &end, &error);

    if (end != (char*) val.x.string.value.str + val.x.string.value.length ||
        error)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_BAD_DATA,
                          ER_THD(thd, ER_BAD_DATA),
                          ErrConvString(val.x.string.value.str,
                                        val.x.string.value.length,
                                        val.x.string.charset).ptr(),
                          "DOUBLE");
    }
    return res;
  }
  case DYN_COL_DECIMAL:
  {
    double res;
    /* This will always succeed */
    decimal2double(&val.x.decimal.value, &res);
    return res;
  }
  case DYN_COL_DATETIME:
  case DYN_COL_DATE:
  case DYN_COL_TIME:
    return TIME_to_double(&val.x.time_value);
  }

null:
  null_value= TRUE;
  return 0.0;
}


my_decimal *Item_dyncol_get::val_decimal(my_decimal *decimal_value)
{
  THD *thd= current_thd;
  DYNAMIC_COLUMN_VALUE val;
  char buff[STRING_BUFFER_USUAL_SIZE];
  String tmp(buff, sizeof(buff), &my_charset_bin);

  if (get_dyn_value(thd, &val, &tmp))
    return NULL;

  switch (val.type) {
  case DYN_COL_DYNCOL:
  case DYN_COL_NULL:
    goto null;
  case DYN_COL_UINT:
    int2my_decimal(E_DEC_FATAL_ERROR, val.x.long_value, TRUE, decimal_value);
    break;
  case DYN_COL_INT:
    int2my_decimal(E_DEC_FATAL_ERROR, val.x.long_value, FALSE, decimal_value);
    break;
  case DYN_COL_DOUBLE:
    double2my_decimal(E_DEC_FATAL_ERROR, val.x.double_value, decimal_value);
    break;
  case DYN_COL_STRING:
  {
    const char *end;
    int rc;
    rc= str2my_decimal(0, val.x.string.value.str, val.x.string.value.length,
                       val.x.string.charset, decimal_value, &end);
    if (rc != E_DEC_OK ||
        end != val.x.string.value.str + val.x.string.value.length)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_BAD_DATA,
                          ER_THD(thd, ER_BAD_DATA),
                          ErrConvString(val.x.string.value.str,
                                        val.x.string.value.length,
                                        val.x.string.charset).ptr(),
                          "DECIMAL");
    }
    break;
  }
  case DYN_COL_DECIMAL:
    decimal2my_decimal(&val.x.decimal.value, decimal_value);
    break;
  case DYN_COL_DATETIME:
  case DYN_COL_DATE:
  case DYN_COL_TIME:
    decimal_value= TIME_to_my_decimal(&val.x.time_value, decimal_value);
    break;
  }
  return decimal_value;

null:
  null_value= TRUE;
  return 0;
}


bool Item_dyncol_get::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DYNAMIC_COLUMN_VALUE val;
  char buff[STRING_BUFFER_USUAL_SIZE];
  String tmp(buff, sizeof(buff), &my_charset_bin);
  bool signed_value= 0;

  if (get_dyn_value(current_thd, &val, &tmp))
    return 1;                                   // Error

  switch (val.type) {
  case DYN_COL_DYNCOL:
  case DYN_COL_NULL:
    goto null;
  case DYN_COL_INT:
    signed_value= 1;                                  // For error message
    /* fall through */
  case DYN_COL_UINT:
    if (signed_value || val.x.ulong_value <= LONGLONG_MAX)
    {
      longlong llval = (longlong)val.x.ulong_value;
      if (int_to_datetime_with_warn(thd, Longlong_hybrid(llval, !signed_value),
                                    ltime, fuzzydate, 0, 0 /* TODO */))
        goto null;
      return 0;
    }
    /* let double_to_datetime_with_warn() issue the warning message */
    val.x.double_value= static_cast<double>(ULONGLONG_MAX);
    /* fall through */
  case DYN_COL_DOUBLE:
    if (double_to_datetime_with_warn(thd, val.x.double_value, ltime, fuzzydate,
                                     0, 0 /* TODO */))
      goto null;
    return 0;
  case DYN_COL_DECIMAL:
    if (decimal_to_datetime_with_warn(thd, (my_decimal*)&val.x.decimal.value,
                                      ltime, fuzzydate, 0, 0 /* TODO */))
      goto null;
    return 0;
  case DYN_COL_STRING:
    if (str_to_datetime_with_warn(thd, &my_charset_numeric,
                                  val.x.string.value.str,
                                  val.x.string.value.length,
                                  ltime, fuzzydate))
      goto null;
    return 0;
  case DYN_COL_DATETIME:
  case DYN_COL_DATE:
  case DYN_COL_TIME:
    *ltime= val.x.time_value;
    return 0;
  }

null:
  null_value= TRUE;
  return 1;
}

void Item_dyncol_get::print(String *str, enum_query_type query_type)
{
  /*
    Parent cast doesn't exist yet, only print dynamic column name. This happens
    when called from create_func_cast() / wrong_precision_error().
  */
  if (!str->length())
  {
    args[1]->print(str, query_type);
    return;
  }

  /* see create_func_dyncol_get */
  DBUG_ASSERT(str->length() >= 5);
  DBUG_ASSERT(strncmp(str->ptr() + str->length() - 5, "cast(", 5) == 0);

  str->length(str->length() - 5);    // removing "cast("
  str->append(STRING_WITH_LEN("column_get("));
  args[0]->print(str, query_type);
  str->append(',');
  args[1]->print(str, query_type);
  /* let the parent cast item add " as <type>)" */
}


String *Item_func_dyncol_list::val_str(String *str)
{
  uint i;
  enum enum_dyncol_func_result rc;
  LEX_STRING *names= 0;
  uint count;
  DYNAMIC_COLUMN col;
  String *res= args[0]->val_str(str);

  if (args[0]->null_value)
    goto null;
  col.length= res->length();
  /* We do not change the string, so could do this trick */
  col.str= (char *)res->ptr();
  if ((rc= mariadb_dyncol_list_named(&col, &count, &names)))
  {
    bzero(&col, sizeof(col));
    dynamic_column_error_message(rc);
    goto null;
  }
  bzero(&col, sizeof(col));

  /*
    We estimate average name length as 10
  */
  if (str->alloc(count * 13))
    goto null;

  str->length(0);
  for (i= 0; i < count; i++)
  {
    append_identifier(current_thd, str, names[i].str, names[i].length);
    if (i < count - 1)
      str->qs_append(',');
  }
  null_value= FALSE;
  if (names)
    my_free(names);
  str->set_charset(DYNCOL_UTF);
  return str;

null:
  null_value= TRUE;
  if (names)
    my_free(names);
  return NULL;
}

Item_temptable_rowid::Item_temptable_rowid(TABLE *table_arg)
  : Item_str_func(table_arg->in_use), table(table_arg)
{
  max_length= table->file->ref_length;
}

bool Item_temptable_rowid::fix_length_and_dec()
{
  used_tables_cache= table->map;
  const_item_cache= false;
  return FALSE;
}

String *Item_temptable_rowid::val_str(String *str)
{
  if (!((null_value= table->null_row)))
    table->file->position(table->record[0]);
  str_value.set((char*)(table->file->ref), max_length, &my_charset_bin);
  return &str_value;
}

#ifdef WITH_WSREP

#include "wsrep_mysqld.h"
/* Format is %d-%d-%llu */
#define WSREP_MAX_WSREP_SERVER_GTID_STR_LEN 10+1+10+1+20

String *Item_func_wsrep_last_written_gtid::val_str_ascii(String *str)
{
  if (gtid_str.alloc(WSREP_MAX_WSREP_SERVER_GTID_STR_LEN+1))
  {
    my_error(ER_OUTOFMEMORY, WSREP_MAX_WSREP_SERVER_GTID_STR_LEN);
    null_value= TRUE;
    return 0;
  }

  ssize_t gtid_len= my_snprintf((char*)gtid_str.ptr(), 
                                WSREP_MAX_WSREP_SERVER_GTID_STR_LEN+1,
                                "%u-%u-%llu", wsrep_gtid_server.domain_id,
                                wsrep_gtid_server.server_id,
                                current_thd->wsrep_last_written_gtid_seqno);
  if (gtid_len < 0)
  {
    my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0), func_name(),
            "wsrep_gtid_print failed");
    null_value= TRUE;
    return 0;
  }
  gtid_str.length(gtid_len);
  return &gtid_str;
}

String *Item_func_wsrep_last_seen_gtid::val_str_ascii(String *str)
{
  if (gtid_str.alloc(WSREP_MAX_WSREP_SERVER_GTID_STR_LEN+1))
  {
    my_error(ER_OUTOFMEMORY, WSREP_MAX_WSREP_SERVER_GTID_STR_LEN);
    null_value= TRUE;
    return 0;
  }
  ssize_t gtid_len= my_snprintf((char*)gtid_str.ptr(), 
                                WSREP_MAX_WSREP_SERVER_GTID_STR_LEN+1,
                                "%u-%u-%llu", wsrep_gtid_server.domain_id,
                                wsrep_gtid_server.server_id,
                                wsrep_gtid_server.seqno());
  if (gtid_len < 0)
  {
    my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0), func_name(),
             "wsrep_gtid_print failed");
    null_value= TRUE;
    return 0;
  }
  gtid_str.length(gtid_len);
  return &gtid_str;
}

longlong Item_func_wsrep_sync_wait_upto::val_int()
{
  String *gtid_str __attribute__((unused)) = args[0]->val_str(&value);
  null_value=0;
  uint timeout;
  rpl_gtid *gtid_list;
  uint32 count;
  int wait_gtid_ret= 0;
  int ret= 1;

  if (args[0]->null_value)
  {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    null_value= TRUE;
    return 0;
  }

  if (arg_count==2 && !args[1]->null_value)
    timeout= (uint)(args[1]->val_real());
  else
    timeout= (uint)-1;

  if (!(gtid_list= gtid_parse_string_to_list(gtid_str->ptr(), gtid_str->length(),
                                             &count)))
  {
    my_error(ER_INCORRECT_GTID_STATE, MYF(0), func_name());
    null_value= TRUE;
    return 0;
  }
  if (count == 1)
  {
    if (wsrep_check_gtid_seqno(gtid_list[0].domain_id, gtid_list[0].server_id,
                               gtid_list[0].seq_no))
    {
      wait_gtid_ret= wsrep_gtid_server.wait_gtid_upto(gtid_list[0].seq_no, timeout);
      if ((wait_gtid_ret == ETIMEDOUT) || (wait_gtid_ret == ETIME))
      {
        my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0), func_name());
        ret= 0;
      }
      else if (wait_gtid_ret == ENOMEM)
      {
        my_error(ER_OUTOFMEMORY, MYF(0), func_name());
        ret= 0;
      }
    }
  }
  else
  { 
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    null_value= TRUE;
    ret= 0;
  }
  my_free(gtid_list);
  return ret;
}

#endif /* WITH_WSREP */

#ifndef ITEM_STRFUNC_INCLUDED
#define ITEM_STRFUNC_INCLUDED

/*
   Copyright (c) 2000, 2011, Oracle and/or its affiliates.
   Copyright (c) 2009, 2015, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */


/* This file defines all string functions */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

extern size_t username_char_length;

class MY_LOCALE;

class Item_str_func :public Item_func
{
protected:
  /**
     Sets the result value of the function an empty string, using the current
     character set. No memory is allocated.
     @retval A pointer to the str_value member.
   */
  String *make_empty_result()
  {
    /*
      Reset string length to an empty string. We don't use str_value.set() as
      we don't want to free and potentially have to reallocate the buffer
      for each call.
    */
    str_value.length(0);
    str_value.set_charset(collation.collation);
    return &str_value; 
  }
public:
  Item_str_func(THD *thd): Item_func(thd) { decimals=NOT_FIXED_DEC; }
  Item_str_func(THD *thd, Item *a): Item_func(thd, a) {decimals=NOT_FIXED_DEC; }
  Item_str_func(THD *thd, Item *a, Item *b):
    Item_func(thd, a, b) { decimals=NOT_FIXED_DEC; }
  Item_str_func(THD *thd, Item *a, Item *b, Item *c):
    Item_func(thd, a, b, c) { decimals=NOT_FIXED_DEC; }
  Item_str_func(THD *thd, Item *a, Item *b, Item *c, Item *d):
    Item_func(thd, a, b, c, d) { decimals=NOT_FIXED_DEC; }
  Item_str_func(THD *thd, Item *a, Item *b, Item *c, Item *d, Item* e):
    Item_func(thd, a, b, c, d, e) { decimals=NOT_FIXED_DEC; }
  Item_str_func(THD *thd, List<Item> &list):
    Item_func(thd, list) { decimals=NOT_FIXED_DEC; }
  longlong val_int();
  double val_real();
  my_decimal *val_decimal(my_decimal *);
  enum Item_result result_type () const { return STRING_RESULT; }
  void left_right_max_length();
  bool fix_fields(THD *thd, Item **ref);
  void update_null_value()
  {
    StringBuffer<MAX_FIELD_WIDTH> tmp;
    (void) val_str(&tmp);
  }
};



/*
  Functions that return values with ASCII repertoire
*/
class Item_str_ascii_func :public Item_str_func
{
  String ascii_buf;
public:
  Item_str_ascii_func(THD *thd): Item_str_func(thd) {}
  Item_str_ascii_func(THD *thd, Item *a): Item_str_func(thd, a) {}
  Item_str_ascii_func(THD *thd, Item *a, Item *b): Item_str_func(thd, a, b) {}
  Item_str_ascii_func(THD *thd, Item *a, Item *b, Item *c):
    Item_str_func(thd, a, b, c) {}
  String *val_str(String *str)
  {
    return val_str_from_val_str_ascii(str, &ascii_buf);
  }
  virtual String *val_str_ascii(String *)= 0;
};


/**
  Functions that return a checksum or a hash of the argument,
  or somehow else encode or decode the argument,
  returning an ASCII-repertoire string.
*/
class Item_str_ascii_checksum_func: public Item_str_ascii_func
{
public:
  Item_str_ascii_checksum_func(THD *thd, Item *a)
   :Item_str_ascii_func(thd, a) { }
  Item_str_ascii_checksum_func(THD *thd, Item *a, Item *b)
   :Item_str_ascii_func(thd, a, b) { }
  bool eq(const Item *item, bool binary_cmp) const
  {
    // Always use binary argument comparison: MD5('x') != MD5('X')
    return Item_func::eq(item, true);
  }
};


/**
  Functions that return a checksum or a hash of the argument,
  or somehow else encode or decode the argument,
  returning a binary string.
*/
class Item_str_binary_checksum_func: public Item_str_func
{
public:
  Item_str_binary_checksum_func(THD *thd, Item *a)
   :Item_str_func(thd, a) { }
  Item_str_binary_checksum_func(THD *thd, Item *a, Item *b)
   :Item_str_func(thd, a, b) { }
  bool eq(const Item *item, bool binary_cmp) const
  {
    /*
      Always use binary argument comparison:
        FROM_BASE64('test') != FROM_BASE64('TEST')
    */
    return Item_func::eq(item, true);
  }
};


class Item_func_md5 :public Item_str_ascii_checksum_func
{
  String tmp_value;
public:
  Item_func_md5(THD *thd, Item *a): Item_str_ascii_checksum_func(thd, a) {}
  String *val_str_ascii(String *);
  void fix_length_and_dec()
  {
    fix_length_and_charset(32, default_charset());
  }
  const char *func_name() const { return "md5"; }
};


class Item_func_sha :public Item_str_ascii_checksum_func
{
public:
  Item_func_sha(THD *thd, Item *a): Item_str_ascii_checksum_func(thd, a) {}
  String *val_str_ascii(String *);    
  void fix_length_and_dec();      
  const char *func_name() const { return "sha"; }	
};

class Item_func_sha2 :public Item_str_ascii_checksum_func
{
public:
  Item_func_sha2(THD *thd, Item *a, Item *b)
   :Item_str_ascii_checksum_func(thd, a, b) {}
  String *val_str_ascii(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "sha2"; }
};

class Item_func_to_base64 :public Item_str_ascii_checksum_func
{
  String tmp_value;
public:
  Item_func_to_base64(THD *thd, Item *a)
   :Item_str_ascii_checksum_func(thd, a) {}
  String *val_str_ascii(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "to_base64"; }
};

class Item_func_from_base64 :public Item_str_binary_checksum_func
{
  String tmp_value;
public:
  Item_func_from_base64(THD *thd, Item *a)
   :Item_str_binary_checksum_func(thd, a) { }
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "from_base64"; }
};

#include <my_crypt.h>

class Item_aes_crypt :public Item_str_binary_checksum_func
{
  enum { AES_KEY_LENGTH = 128 };
  void create_key(String *user_key, uchar* key);

protected:
  int what;
public:
  Item_aes_crypt(THD *thd, Item *a, Item *b)
   :Item_str_binary_checksum_func(thd, a, b) {}
  String *val_str(String *);
};

class Item_func_aes_encrypt :public Item_aes_crypt
{
public:
  Item_func_aes_encrypt(THD *thd, Item *a, Item *b):
    Item_aes_crypt(thd, a, b) {}
  void fix_length_and_dec();
  const char *func_name() const { return "aes_encrypt"; }
};

class Item_func_aes_decrypt :public Item_aes_crypt
{
public:
  Item_func_aes_decrypt(THD *thd, Item *a, Item *b):
    Item_aes_crypt(thd, a, b) {}
  void fix_length_and_dec();
  const char *func_name() const { return "aes_decrypt"; }
};


class Item_func_concat :public Item_str_func
{
  String tmp_value;
public:
  Item_func_concat(THD *thd, List<Item> &list): Item_str_func(thd, list) {}
  Item_func_concat(THD *thd, Item *a, Item *b): Item_str_func(thd, a, b) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "concat"; }
};

class Item_func_decode_histogram :public Item_str_func
{
  String tmp_value;
public:
  Item_func_decode_histogram(THD *thd, Item *a, Item *b):
    Item_str_func(thd, a, b) {}
  String *val_str(String *);
  void fix_length_and_dec()
  {
    collation.set(system_charset_info);
    max_length= MAX_BLOB_WIDTH;
    maybe_null= 1;
  }
  const char *func_name() const { return "decode_histogram"; }
};

class Item_func_concat_ws :public Item_str_func
{
  String tmp_value;
public:
  Item_func_concat_ws(THD *thd, List<Item> &list): Item_str_func(thd, list) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "concat_ws"; }
  table_map not_null_tables() const { return 0; }
};

class Item_func_reverse :public Item_str_func
{
  String tmp_value;
public:
  Item_func_reverse(THD *thd, Item *a): Item_str_func(thd, a) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "reverse"; }
};


class Item_func_replace :public Item_str_func
{
  String tmp_value,tmp_value2;
public:
  Item_func_replace(THD *thd, Item *org, Item *find, Item *replace):
    Item_str_func(thd, org, find, replace) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "replace"; }
};


class Item_func_regexp_replace :public Item_str_func
{
  Regexp_processor_pcre re;
  bool append_replacement(String *str,
                          const LEX_CSTRING *source,
                          const LEX_CSTRING *replace);
public:
  Item_func_regexp_replace(THD *thd, Item *a, Item *b, Item *c):
    Item_str_func(thd, a, b, c)
    {}
  void cleanup()
  {
    DBUG_ENTER("Item_func_regex::cleanup");
    Item_str_func::cleanup();
    re.cleanup();
    DBUG_VOID_RETURN;
  }
  String *val_str(String *str);
  void fix_length_and_dec();
  const char *func_name() const { return "regexp_replace"; }
};


class Item_func_regexp_substr :public Item_str_func
{
  Regexp_processor_pcre re;
public:
  Item_func_regexp_substr(THD *thd, Item *a, Item *b):
    Item_str_func(thd, a, b)
    {}
  void cleanup()
  {
    DBUG_ENTER("Item_func_regex::cleanup");
    Item_str_func::cleanup();
    re.cleanup();
    DBUG_VOID_RETURN;
  }
  String *val_str(String *str);
  void fix_length_and_dec();
  const char *func_name() const { return "regexp_substr"; }
};


class Item_func_insert :public Item_str_func
{
  String tmp_value;
public:
  Item_func_insert(THD *thd, Item *org, Item *start, Item *length,
                   Item *new_str):
    Item_str_func(thd, org, start, length, new_str) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "insert"; }
};


class Item_str_conv :public Item_str_func
{
protected:
  uint multiply;
  my_charset_conv_case converter;
  String tmp_value;
public:
  Item_str_conv(THD *thd, Item *item): Item_str_func(thd, item) {}
  String *val_str(String *);
};


class Item_func_lcase :public Item_str_conv
{
public:
  Item_func_lcase(THD *thd, Item *item): Item_str_conv(thd, item) {}
  const char *func_name() const { return "lcase"; }
  void fix_length_and_dec();
};

class Item_func_ucase :public Item_str_conv
{
public:
  Item_func_ucase(THD *thd, Item *item): Item_str_conv(thd, item) {}
  const char *func_name() const { return "ucase"; }
  void fix_length_and_dec();
};


class Item_func_left :public Item_str_func
{
  String tmp_value;
public:
  Item_func_left(THD *thd, Item *a, Item *b): Item_str_func(thd, a, b) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "left"; }
};


class Item_func_right :public Item_str_func
{
  String tmp_value;
public:
  Item_func_right(THD *thd, Item *a, Item *b): Item_str_func(thd, a, b) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "right"; }
};


class Item_func_substr :public Item_str_func
{
  String tmp_value;
public:
  Item_func_substr(THD *thd, Item *a, Item *b): Item_str_func(thd, a, b) {}
  Item_func_substr(THD *thd, Item *a, Item *b, Item *c): Item_str_func(thd, a, b, c) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "substr"; }
};


class Item_func_substr_index :public Item_str_func
{
  String tmp_value;
public:
  Item_func_substr_index(THD *thd, Item *a,Item *b,Item *c):
    Item_str_func(thd, a, b, c) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "substring_index"; }
};


class Item_func_trim :public Item_str_func
{
protected:
  String tmp_value;
  String remove;
  String *trimmed_value(String *res, uint32 offset, uint32 length)
  {
    tmp_value.set(*res, offset, length);
    /*
      Make sure to return correct charset and collation:
      TRIM(0x000000 FROM _ucs2 0x0061)
      should set charset to "binary" rather than to "ucs2".
    */
    tmp_value.set_charset(collation.collation);
    return &tmp_value;
  }
  String *non_trimmed_value(String *res)
  {
    return trimmed_value(res, 0, res->length());
  }
public:
  Item_func_trim(THD *thd, Item *a, Item *b): Item_str_func(thd, a, b) {}
  Item_func_trim(THD *thd, Item *a): Item_str_func(thd, a) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "trim"; }
  virtual void print(String *str, enum_query_type query_type);
  virtual const char *mode_name() const { return "both"; }
};


class Item_func_ltrim :public Item_func_trim
{
public:
  Item_func_ltrim(THD *thd, Item *a, Item *b): Item_func_trim(thd, a, b) {}
  Item_func_ltrim(THD *thd, Item *a): Item_func_trim(thd, a) {}
  String *val_str(String *);
  const char *func_name() const { return "ltrim"; }
  const char *mode_name() const { return "leading"; }
};


class Item_func_rtrim :public Item_func_trim
{
public:
  Item_func_rtrim(THD *thd, Item *a, Item *b): Item_func_trim(thd, a, b) {}
  Item_func_rtrim(THD *thd, Item *a): Item_func_trim(thd, a) {}
  String *val_str(String *);
  const char *func_name() const { return "rtrim"; }
  const char *mode_name() const { return "trailing"; }
};


/*
  Item_func_password -- new (4.1.1) PASSWORD() function implementation.
  Returns strcat('*', octet2hex(sha1(sha1(password)))). '*' stands for new
  password format, sha1(sha1(password) is so-called hash_stage2 value.
  Length of returned string is always 41 byte. To find out how entire
  authentication procedure works, see comments in password.c.
*/

class Item_func_password :public Item_str_ascii_checksum_func
{
public:
  enum PW_Alg {OLD, NEW};
private:
  char tmp_value[SCRAMBLED_PASSWORD_CHAR_LENGTH+1]; 
  enum PW_Alg alg;
  bool deflt;
public:
  Item_func_password(THD *thd, Item *a):
    Item_str_ascii_checksum_func(thd, a), alg(NEW), deflt(1) {}
  Item_func_password(THD *thd, Item *a, PW_Alg al):
    Item_str_ascii_checksum_func(thd, a), alg(al), deflt(0) {}
  String *val_str_ascii(String *str);
  bool fix_fields(THD *thd, Item **ref);
  void fix_length_and_dec()
  {
    fix_length_and_charset((alg == 1 ?
                            SCRAMBLED_PASSWORD_CHAR_LENGTH :
                            SCRAMBLED_PASSWORD_CHAR_LENGTH_323),
                           default_charset());
  }
  const char *func_name() const { return ((deflt || alg == 1) ?
                                          "password" : "old_password"); }
  static char *alloc(THD *thd, const char *password, size_t pass_len,
                     enum PW_Alg al);
};



class Item_func_des_encrypt :public Item_str_binary_checksum_func
{
  String tmp_value,tmp_arg;
public:
  Item_func_des_encrypt(THD *thd, Item *a)
   :Item_str_binary_checksum_func(thd, a) {}
  Item_func_des_encrypt(THD *thd, Item *a, Item *b)
   :Item_str_binary_checksum_func(thd, a, b) {}
  String *val_str(String *);
  void fix_length_and_dec()
  {
    maybe_null=1;
    /* 9 = MAX ((8- (arg_len % 8)) + 1) */
    max_length = args[0]->max_length + 9;
  }
  const char *func_name() const { return "des_encrypt"; }
};

class Item_func_des_decrypt :public Item_str_binary_checksum_func
{
  String tmp_value;
public:
  Item_func_des_decrypt(THD *thd, Item *a)
   :Item_str_binary_checksum_func(thd, a) {}
  Item_func_des_decrypt(THD *thd, Item *a, Item *b)
   :Item_str_binary_checksum_func(thd, a, b) {}
  String *val_str(String *);
  void fix_length_and_dec()
  {
    maybe_null=1;
    /* 9 = MAX ((8- (arg_len % 8)) + 1) */
    max_length= args[0]->max_length;
    if (max_length >= 9U)
      max_length-= 9U;
  }
  const char *func_name() const { return "des_decrypt"; }
};


/**
  QQ: Item_func_encrypt should derive from Item_str_ascii_checksum_func.
  However, it should be fixed to handle UCS2, UTF16, UTF32 properly first,
  as the underlying crypt() call expects a null-terminated input string.
*/
class Item_func_encrypt :public Item_str_binary_checksum_func
{
  String tmp_value;

  /* Encapsulate common constructor actions */
  void constructor_helper()
  {
    collation.set(&my_charset_bin);
  }
public:
  Item_func_encrypt(THD *thd, Item *a): Item_str_binary_checksum_func(thd, a)
  {
    constructor_helper();
  }
  Item_func_encrypt(THD *thd, Item *a, Item *b)
   :Item_str_binary_checksum_func(thd, a, b)
  {
    constructor_helper();
  }
  String *val_str(String *);
  void fix_length_and_dec() { maybe_null=1; max_length = 13; }
  const char *func_name() const { return "encrypt"; }
  bool check_vcol_func_processor(uchar *int_arg) 
  {
    return trace_unsupported_by_check_vcol_func_processor(func_name());
  }
};

#include "sql_crypt.h"


class Item_func_encode :public Item_str_binary_checksum_func
{
private:
  /** Whether the PRNG has already been seeded. */
  bool seeded;
protected:
  SQL_CRYPT sql_crypt;
public:
  Item_func_encode(THD *thd, Item *a, Item *seed_arg):
    Item_str_binary_checksum_func(thd, a, seed_arg) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "encode"; }
protected:
  virtual void crypto_transform(String *);
private:
  /** Provide a seed for the PRNG sequence. */
  bool seed();
};


class Item_func_decode :public Item_func_encode
{
public:
  Item_func_decode(THD *thd, Item *a, Item *seed_arg): Item_func_encode(thd, a, seed_arg) {}
  const char *func_name() const { return "decode"; }
protected:
  void crypto_transform(String *);
};


class Item_func_sysconst :public Item_str_func
{
public:
  Item_func_sysconst(THD *thd): Item_str_func(thd)
  { collation.set(system_charset_info,DERIVATION_SYSCONST); }
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs)
  {
    return const_charset_converter(thd, tocs, true, fully_qualified_func_name());
  }
  /*
    Used to create correct Item name in new converted item in
    safe_charset_converter, return string representation of this function
    call
  */
  virtual const char *fully_qualified_func_name() const = 0;
  bool check_vcol_func_processor(uchar *int_arg) 
  {
    return trace_unsupported_by_check_vcol_func_processor(
                                           fully_qualified_func_name());
  }
};


class Item_func_database :public Item_func_sysconst
{
public:
  Item_func_database(THD *thd): Item_func_sysconst(thd) {}
  String *val_str(String *);
  void fix_length_and_dec()
  {
    max_length= MAX_FIELD_NAME * system_charset_info->mbmaxlen;
    maybe_null=1;
  }
  const char *func_name() const { return "database"; }
  const char *fully_qualified_func_name() const { return "database()"; }
};


class Item_func_user :public Item_func_sysconst
{
protected:
  bool init (const char *user, const char *host);

public:
  Item_func_user(THD *thd): Item_func_sysconst(thd)
  {
    str_value.set("", 0, system_charset_info);
  }
  String *val_str(String *)
  {
    DBUG_ASSERT(fixed == 1);
    return (null_value ? 0 : &str_value);
  }
  bool fix_fields(THD *thd, Item **ref);
  void fix_length_and_dec()
  {
    max_length= (username_char_length +
                 HOSTNAME_LENGTH + 1) * SYSTEM_CHARSET_MBMAXLEN;
  }
  const char *func_name() const { return "user"; }
  const char *fully_qualified_func_name() const { return "user()"; }
  int save_in_field(Field *field, bool no_conversions)
  {
    return save_str_value_in_field(field, &str_value);
  }
};


class Item_func_current_user :public Item_func_user
{
  Name_resolution_context *context;

public:
  Item_func_current_user(THD *thd, Name_resolution_context *context_arg):
    Item_func_user(thd), context(context_arg) {}
  bool fix_fields(THD *thd, Item **ref);
  const char *func_name() const { return "current_user"; }
  const char *fully_qualified_func_name() const { return "current_user()"; }
};


class Item_func_current_role :public Item_func_sysconst
{
  Name_resolution_context *context;

public:
  Item_func_current_role(THD *thd, Name_resolution_context *context_arg):
    Item_func_sysconst(thd), context(context_arg) {}
  bool fix_fields(THD *thd, Item **ref);
  void fix_length_and_dec()
  { max_length= username_char_length * SYSTEM_CHARSET_MBMAXLEN; }
  int save_in_field(Field *field, bool no_conversions)
  { return save_str_value_in_field(field, &str_value); }
  const char *func_name() const { return "current_role"; }
  const char *fully_qualified_func_name() const { return "current_role()"; }
  String *val_str(String *)
  {
    DBUG_ASSERT(fixed == 1);
    return (null_value ? 0 : &str_value);
  }
};


class Item_func_soundex :public Item_str_func
{
  String tmp_value;
public:
  Item_func_soundex(THD *thd, Item *a): Item_str_func(thd, a) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "soundex"; }
};


class Item_func_elt :public Item_str_func
{
public:
  Item_func_elt(THD *thd, List<Item> &list): Item_str_func(thd, list) {}
  double val_real();
  longlong val_int();
  String *val_str(String *str);
  void fix_length_and_dec();
  const char *func_name() const { return "elt"; }
};


class Item_func_make_set :public Item_str_func
{
  String tmp_str;

public:
  Item_func_make_set(THD *thd, List<Item> &list): Item_str_func(thd, list) {}
  String *val_str(String *str);
  void fix_length_and_dec();
  const char *func_name() const { return "make_set"; }
};


class Item_func_format :public Item_str_ascii_func
{
  String tmp_str;
  MY_LOCALE *locale;
public:
  Item_func_format(THD *thd, Item *org, Item *dec):
    Item_str_ascii_func(thd, org, dec) {}
  Item_func_format(THD *thd, Item *org, Item *dec, Item *lang):
    Item_str_ascii_func(thd, org, dec, lang) {}

  MY_LOCALE *get_locale(Item *item);
  String *val_str_ascii(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "format"; }
  virtual void print(String *str, enum_query_type query_type);
};


class Item_func_char :public Item_str_func
{
public:
  Item_func_char(THD *thd, List<Item> &list): Item_str_func(thd, list)
  { collation.set(&my_charset_bin); }
  Item_func_char(THD *thd, List<Item> &list, CHARSET_INFO *cs):
    Item_str_func(thd, list)
  { collation.set(cs); }
  String *val_str(String *);
  void fix_length_and_dec() 
  {
    max_length= arg_count * 4;
  }
  const char *func_name() const { return "char"; }
};


class Item_func_repeat :public Item_str_func
{
  String tmp_value;
public:
  Item_func_repeat(THD *thd, Item *arg1, Item *arg2):
    Item_str_func(thd, arg1, arg2) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "repeat"; }
};


class Item_func_space :public Item_str_func
{
public:
  Item_func_space(THD *thd, Item *arg1): Item_str_func(thd, arg1) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "space"; }
};


class Item_func_binlog_gtid_pos :public Item_str_func
{
  String tmp_value;
public:
  Item_func_binlog_gtid_pos(THD *thd, Item *arg1, Item *arg2):
    Item_str_func(thd, arg1, arg2) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "binlog_gtid_pos"; }
};


class Item_func_rpad :public Item_str_func
{
  String tmp_value, rpad_str;
public:
  Item_func_rpad(THD *thd, Item *arg1, Item *arg2, Item *arg3):
    Item_str_func(thd, arg1, arg2, arg3) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "rpad"; }
};


class Item_func_lpad :public Item_str_func
{
  String tmp_value, lpad_str;
public:
  Item_func_lpad(THD *thd, Item *arg1, Item *arg2, Item *arg3):
    Item_str_func(thd, arg1, arg2, arg3) {}
  String *val_str(String *);
  void fix_length_and_dec();
  const char *func_name() const { return "lpad"; }
};


class Item_func_conv :public Item_str_func
{
public:
  Item_func_conv(THD *thd, Item *a, Item *b, Item *c):
    Item_str_func(thd, a, b, c) {}
  const char *func_name() const { return "conv"; }
  String *val_str(String *);
  void fix_length_and_dec()
  {
    collation.set(default_charset());
    max_length=64;
    maybe_null= 1;
  }
};


class Item_func_hex :public Item_str_ascii_checksum_func
{
  String tmp_value;
public:
  Item_func_hex(THD *thd, Item *a):
    Item_str_ascii_checksum_func(thd, a) {}
  const char *func_name() const { return "hex"; }
  String *val_str_ascii(String *);
  void fix_length_and_dec()
  {
    collation.set(default_charset());
    decimals=0;
    fix_char_length(args[0]->max_length * 2);
  }
};

class Item_func_unhex :public Item_str_func
{
  String tmp_value;
public:
  Item_func_unhex(THD *thd, Item *a): Item_str_func(thd, a)
  {
    /* there can be bad hex strings */
    maybe_null= 1;
  }
  const char *func_name() const { return "unhex"; }
  String *val_str(String *);
  void fix_length_and_dec()
  {
    collation.set(&my_charset_bin);
    decimals=0;
    max_length=(1+args[0]->max_length)/2;
  }
};


#ifndef DBUG_OFF
class Item_func_like_range :public Item_str_func
{
protected:
  String min_str;
  String max_str;
  const bool is_min;
public:
  Item_func_like_range(THD *thd, Item *a, Item *b, bool is_min_arg):
    Item_str_func(thd, a, b), is_min(is_min_arg)
  { maybe_null= 1; }
  String *val_str(String *);
  void fix_length_and_dec()
  {
    collation.set(args[0]->collation);
    decimals=0;
    max_length= MAX_BLOB_WIDTH;
  }
};


class Item_func_like_range_min :public Item_func_like_range
{
public:
  Item_func_like_range_min(THD *thd, Item *a, Item *b):
    Item_func_like_range(thd, a, b, true) { }
  const char *func_name() const { return "like_range_min"; }
};


class Item_func_like_range_max :public Item_func_like_range
{
public:
  Item_func_like_range_max(THD *thd, Item *a, Item *b):
    Item_func_like_range(thd, a, b, false) { }
  const char *func_name() const { return "like_range_max"; }
};
#endif


class Item_func_binary :public Item_str_func
{
public:
  Item_func_binary(THD *thd, Item *a): Item_str_func(thd, a) {}
  String *val_str(String *a)
  {
    DBUG_ASSERT(fixed == 1);
    String *tmp=args[0]->val_str(a);
    null_value=args[0]->null_value;
    if (tmp)
      tmp->set_charset(&my_charset_bin);
    return tmp;
  }
  void fix_length_and_dec()
  {
    collation.set(&my_charset_bin);
    max_length=args[0]->max_length;
  }
  virtual void print(String *str, enum_query_type query_type);
  const char *func_name() const { return "cast_as_binary"; }
};


class Item_load_file :public Item_str_func
{
  String tmp_value;
public:
  Item_load_file(THD *thd, Item *a): Item_str_func(thd, a) {}
  String *val_str(String *);
  const char *func_name() const { return "load_file"; }
  void fix_length_and_dec()
  {
    collation.set(&my_charset_bin, DERIVATION_COERCIBLE);
    maybe_null=1;
    max_length=MAX_BLOB_WIDTH;
  }
  bool check_vcol_func_processor(uchar *int_arg) 
  {
    return trace_unsupported_by_check_vcol_func_processor(func_name());
  }
};


class Item_func_export_set: public Item_str_func
{
 public:
  Item_func_export_set(THD *thd, Item *a, Item *b, Item* c):
    Item_str_func(thd, a, b, c) {}
  Item_func_export_set(THD *thd, Item *a, Item *b, Item* c, Item* d):
    Item_str_func(thd, a, b, c, d) {}
  Item_func_export_set(THD *thd, Item *a, Item *b, Item* c, Item* d, Item* e):
    Item_str_func(thd, a, b, c, d, e) {}
  String  *val_str(String *str);
  void fix_length_and_dec();
  const char *func_name() const { return "export_set"; }
};


class Item_func_quote :public Item_str_func
{
  String tmp_value;
public:
  Item_func_quote(THD *thd, Item *a): Item_str_func(thd, a) {}
  const char *func_name() const { return "quote"; }
  String *val_str(String *);
  void fix_length_and_dec()
  {
    collation.set(args[0]->collation);
    ulonglong max_result_length= (ulonglong) args[0]->max_length * 2 +
                                  2 * collation.collation->mbmaxlen;
    max_length= (uint32) MY_MIN(max_result_length, MAX_BLOB_WIDTH);
  }
};

class Item_func_conv_charset :public Item_str_func
{
  bool use_cached_value;
  String tmp_value;
public:
  bool safe;
  Item_func_conv_charset(THD *thd, Item *a, CHARSET_INFO *cs):
    Item_str_func(thd, a)
  {
    collation.set(cs, DERIVATION_IMPLICIT);
    use_cached_value= 0; safe= 0;
  }
  Item_func_conv_charset(THD *thd, Item *a, CHARSET_INFO *cs, bool cache_if_const):
    Item_str_func(thd, a)
  {
    collation.set(cs, DERIVATION_IMPLICIT);
    if (cache_if_const && args[0]->const_item() && !args[0]->is_expensive())
    {
      uint errors= 0;
      String tmp, *str= args[0]->val_str(&tmp);
      if (!str || str_value.copy(str->ptr(), str->length(),
                                 str->charset(), cs, &errors))
        null_value= 1;
      use_cached_value= 1;
      str_value.mark_as_const();
      safe= (errors == 0);
    }
    else
    {
      use_cached_value= 0;
      /*
        Conversion from and to "binary" is safe.
        Conversion to Unicode is safe.
        Other kind of conversions are potentially lossy.
      */
      safe= (args[0]->collation.collation == &my_charset_bin ||
             cs == &my_charset_bin ||
             (cs->state & MY_CS_UNICODE));
    }
  }
  String *val_str(String *);
  longlong val_int()
  {
    if (args[0]->result_type() == STRING_RESULT)
      return Item_str_func::val_int();
    longlong res= args[0]->val_int();
    if ((null_value= args[0]->null_value))
      return 0;
    return res;
  }
  double val_real()
  {
    if (args[0]->result_type() == STRING_RESULT)
      return Item_str_func::val_real();
    double res= args[0]->val_real();
    if ((null_value= args[0]->null_value))
      return 0;
    return res;
  }
  my_decimal *val_decimal(my_decimal *d)
  {
    if (args[0]->result_type() == STRING_RESULT)
      return Item_str_func::val_decimal(d);
    my_decimal *res= args[0]->val_decimal(d);
    if ((null_value= args[0]->null_value))
      return NULL;
    return res;
  }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate)
  {
    if (args[0]->result_type() == STRING_RESULT)
      return Item_str_func::get_date(ltime, fuzzydate);
    bool res= args[0]->get_date(ltime, fuzzydate);
    if ((null_value= args[0]->null_value))
      return 1;
    return res;
  }
  void fix_length_and_dec();
  const char *func_name() const { return "convert"; }
  virtual void print(String *str, enum_query_type query_type);
};

class Item_func_set_collation :public Item_str_func
{
public:
  Item_func_set_collation(THD *thd, Item *a, Item *b):
    Item_str_func(thd, a, b) {}
  String *val_str(String *);
  void fix_length_and_dec();
  bool eq(const Item *item, bool binary_cmp) const;
  const char *func_name() const { return "collate"; }
  enum Functype functype() const { return COLLATE_FUNC; }
  virtual void print(String *str, enum_query_type query_type);
  Item_field *field_for_view_update()
  {
    /* this function is transparent for view updating */
    return args[0]->field_for_view_update();
  }
};


class Item_func_expr_str_metadata :public Item_str_func
{
public:
  Item_func_expr_str_metadata(THD *thd, Item *a): Item_str_func(thd, a) { }
  void fix_length_and_dec()
  {
     collation.set(system_charset_info);
     max_length= 64 * collation.collation->mbmaxlen; // should be enough
     maybe_null= 0;
  };
  table_map not_null_tables() const { return 0; }
  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
  { return this; }
  bool const_item() const { return true; }
};


class Item_func_charset :public Item_func_expr_str_metadata
{
public:
  Item_func_charset(THD *thd, Item *a)
    :Item_func_expr_str_metadata(thd, a) { }
  String *val_str(String *);
  const char *func_name() const { return "charset"; }
};


class Item_func_collation :public Item_func_expr_str_metadata
{
public:
  Item_func_collation(THD *thd, Item *a)
    :Item_func_expr_str_metadata(thd, a) {}
  String *val_str(String *);
  const char *func_name() const { return "collation"; }
};


class Item_func_weight_string :public Item_str_func
{
  String tmp_value;
  uint flags;
  uint nweights;
  uint result_length;
public:
  Item_func_weight_string(THD *thd, Item *a, uint result_length_arg,
                          uint nweights_arg, uint flags_arg):
    Item_str_func(thd, a)
  {
    nweights= nweights_arg;
    flags= flags_arg;
    result_length= result_length_arg;
  }
  const char *func_name() const { return "weight_string"; }
  String *val_str(String *);
  void fix_length_and_dec();
  bool eq(const Item *item, bool binary_cmp) const
  {
    if (!Item_str_func::eq(item, binary_cmp))
      return false;
    Item_func_weight_string *that= (Item_func_weight_string *)item;
    return this->flags == that->flags &&
           this->nweights == that->nweights &&
           this->result_length == that->result_length;
  }
  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
  { return this; }
};

class Item_func_crc32 :public Item_int_func
{
  String value;
public:
  Item_func_crc32(THD *thd, Item *a): Item_int_func(thd, a)
  { unsigned_flag= 1; }
  const char *func_name() const { return "crc32"; }
  void fix_length_and_dec() { max_length=10; }
  longlong val_int();
};

class Item_func_uncompressed_length : public Item_int_func
{
  String value;
public:
  Item_func_uncompressed_length(THD *thd, Item *a): Item_int_func(thd, a) {}
  const char *func_name() const{return "uncompressed_length";}
  void fix_length_and_dec() { max_length=10; maybe_null= true; }
  longlong val_int();
};

#ifdef HAVE_COMPRESS
#define ZLIB_DEPENDED_FUNCTION ;
#else
#define ZLIB_DEPENDED_FUNCTION { null_value=1; return 0; }
#endif

class Item_func_compress: public Item_str_binary_checksum_func
{
  String buffer;
public:
  Item_func_compress(THD *thd, Item *a)
   :Item_str_binary_checksum_func(thd, a) {}
  void fix_length_and_dec(){max_length= (args[0]->max_length*120)/100+12;}
  const char *func_name() const{return "compress";}
  String *val_str(String *) ZLIB_DEPENDED_FUNCTION
};

class Item_func_uncompress: public Item_str_binary_checksum_func
{
  String buffer;
public:
  Item_func_uncompress(THD *thd, Item *a)
   :Item_str_binary_checksum_func(thd, a) {}
  void fix_length_and_dec(){ maybe_null= 1; max_length= MAX_BLOB_WIDTH; }
  const char *func_name() const{return "uncompress";}
  String *val_str(String *) ZLIB_DEPENDED_FUNCTION
};


class Item_func_uuid: public Item_str_func
{
public:
  Item_func_uuid(THD *thd): Item_str_func(thd) {}
  void fix_length_and_dec()
  {
    collation.set(system_charset_info,
                  DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
    fix_char_length(MY_UUID_STRING_LENGTH);
  }
  const char *func_name() const{ return "uuid"; }
  String *val_str(String *);
  bool check_vcol_func_processor(uchar *int_arg) 
  {
    return trace_unsupported_by_check_vcol_func_processor(func_name());
  }
};


class Item_func_dyncol_create: public Item_str_func
{
protected:
  DYNCALL_CREATE_DEF *defs;
  DYNAMIC_COLUMN_VALUE *vals;
  uint *keys_num;
  LEX_STRING *keys_str;
  bool names, force_names;
  bool prepare_arguments(bool force_names);
  void print_arguments(String *str, enum_query_type query_type);
public:
  Item_func_dyncol_create(THD *thd, List<Item> &args, DYNCALL_CREATE_DEF *dfs);
  bool fix_fields(THD *thd, Item **ref);
  void fix_length_and_dec();
  const char *func_name() const{ return "column_create"; }
  String *val_str(String *);
  virtual void print(String *str, enum_query_type query_type);
  virtual enum Functype functype() const   { return DYNCOL_FUNC; }
};


class Item_func_dyncol_add: public Item_func_dyncol_create
{
public:
  Item_func_dyncol_add(THD *thd, List<Item> &args_arg, DYNCALL_CREATE_DEF *dfs):
    Item_func_dyncol_create(thd, args_arg, dfs)
  {}
  const char *func_name() const{ return "column_add"; }
  String *val_str(String *);
  virtual void print(String *str, enum_query_type query_type);
};

class Item_func_dyncol_json: public Item_str_func
{
public:
  Item_func_dyncol_json(THD *thd, Item *str): Item_str_func(thd, str) {}
  const char *func_name() const{ return "column_json"; }
  String *val_str(String *);
  void fix_length_and_dec()
  {
    max_length= MAX_BLOB_WIDTH;
    maybe_null= 1;
    collation.set(&my_charset_bin);
    decimals= 0;
  }
};

/*
  The following functions is always called from an Item_cast function
*/

class Item_dyncol_get: public Item_str_func
{
public:
  Item_dyncol_get(THD *thd, Item *str, Item *num): Item_str_func(thd, str, num)
  {}
  void fix_length_and_dec()
  { maybe_null= 1;; max_length= MAX_BLOB_WIDTH; }
  /* Mark that collation can change between calls */
  bool dynamic_result() { return 1; }

  const char *func_name() const { return "column_get"; }
  String *val_str(String *);
  longlong val_int();
  double val_real();
  my_decimal *val_decimal(my_decimal *);
  bool get_dyn_value(DYNAMIC_COLUMN_VALUE *val, String *tmp);
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  void print(String *str, enum_query_type query_type);
};


class Item_func_dyncol_list: public Item_str_func
{
public:
  Item_func_dyncol_list(THD *thd, Item *str): Item_str_func(thd, str) {};
  void fix_length_and_dec() { maybe_null= 1; max_length= MAX_BLOB_WIDTH; };
  const char *func_name() const{ return "column_list"; }
  String *val_str(String *);
};

#endif /* ITEM_STRFUNC_INCLUDED */


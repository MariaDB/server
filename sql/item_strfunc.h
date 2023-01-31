#ifndef ITEM_STRFUNC_INCLUDED
#define ITEM_STRFUNC_INCLUDED

/*
   Copyright (c) 2000, 2011, Oracle and/or its affiliates.
   Copyright (c) 2009, 2022, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */


/* This file defines all string functions */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

extern size_t username_char_length;

class Item_str_func :public Item_func
{
protected:
  /**
     Sets the result value of the function an empty string, using the current
     character set. No memory is allocated.
     @retval A pointer to the str_value member.
   */
  virtual String *make_empty_result(String *str)
  {
    /*
      Reset string length to an empty string. We don't use str_value.set() as
      we don't want to free and potentially have to reallocate the buffer
      for each call.
    */
    if (!str->is_alloced())
      str->set("", 0, collation.collation); /* Avoid null ptrs */
    else
    {
      str->length(0);                      /* Reuse allocated area */
      str->set_charset(collation.collation);
    }
    return str;
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
  longlong val_int() override;
  double val_real() override;
  my_decimal *val_decimal(my_decimal *) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  { return get_date_from_string(thd, ltime, fuzzydate); }
  const Type_handler *type_handler() const override
  { return string_type_handler(); }
  void left_right_max_length();
  bool fix_fields(THD *thd, Item **ref) override;
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
  String *val_str(String *str) override
  {
    return val_str_from_val_str_ascii(str, &ascii_buf);
  }
  String *val_str_ascii(String *) override= 0;
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
public:
  Item_func_md5(THD *thd, Item *a): Item_str_ascii_checksum_func(thd, a) {}
  String *val_str_ascii(String *) override;
  bool fix_length_and_dec(THD *thd) override
  {
    fix_length_and_charset(32, default_charset());
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("md5") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_md5>(thd, this); }
};


class Item_func_sha :public Item_str_ascii_checksum_func
{
public:
  Item_func_sha(THD *thd, Item *a): Item_str_ascii_checksum_func(thd, a) {}
  String *val_str_ascii(String *) override;    
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("sha") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_sha>(thd, this); }
};

class Item_func_sha2 :public Item_str_ascii_checksum_func
{
public:
  Item_func_sha2(THD *thd, Item *a, Item *b)
   :Item_str_ascii_checksum_func(thd, a, b) {}
  String *val_str_ascii(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("sha2") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_sha2>(thd, this); }
};

class Item_func_to_base64 :public Item_str_ascii_checksum_func
{
  String tmp_value;
public:
  Item_func_to_base64(THD *thd, Item *a)
   :Item_str_ascii_checksum_func(thd, a) {}
  String *val_str_ascii(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("to_base64") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_to_base64>(thd, this); }
};

class Item_func_from_base64 :public Item_str_binary_checksum_func
{
  String tmp_value;
public:
  Item_func_from_base64(THD *thd, Item *a)
   :Item_str_binary_checksum_func(thd, a) { }
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("from_base64") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_from_base64>(thd, this); }
};

#include <my_crypt.h>

class Item_aes_crypt :public Item_str_binary_checksum_func
{
  enum { AES_KEY_LENGTH = 128 };
  void create_key(String *user_key, uchar* key);

protected:
  int what;
  String tmp_value;
public:
  Item_aes_crypt(THD *thd, Item *a, Item *b)
   :Item_str_binary_checksum_func(thd, a, b) {}
  String *val_str(String *);
};

class Item_func_aes_encrypt :public Item_aes_crypt
{
public:
  Item_func_aes_encrypt(THD *thd, Item *a, Item *b)
   :Item_aes_crypt(thd, a, b) {}
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("aes_encrypt") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_aes_encrypt>(thd, this); }
};

class Item_func_aes_decrypt :public Item_aes_crypt
{
public:
  Item_func_aes_decrypt(THD *thd, Item *a, Item *b):
    Item_aes_crypt(thd, a, b) {}
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("aes_decrypt") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_aes_decrypt>(thd, this); }
};

class Item_func_natural_sort_key : public Item_str_func
{
public:
  Item_func_natural_sort_key(THD *thd, Item *a)
      : Item_str_func(thd, a){};
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("natural_sort_key")};
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  Item *get_copy(THD *thd) override
  {
    return get_item_copy<Item_func_natural_sort_key>(thd, this);
  }

  bool check_vcol_func_processor(void *arg) override;
};

class Item_func_concat :public Item_str_func
{
protected:
  String tmp_value;
  /*
    Append a non-NULL value to the result.
    @param [IN]     thd          - The current thread.
    @param [IN/OUT] res          - The current val_str() return value.
    @param [IN]     app          - The value to be appended.
    @retval                      - false on success, true on error
  */
  bool append_value(THD *thd, String *res, const String *app);
  bool realloc_result(String *str, uint length) const;
public:
  Item_func_concat(THD *thd, List<Item> &list): Item_str_func(thd, list) {}
  Item_func_concat(THD *thd, Item *a, Item *b): Item_str_func(thd, a, b) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("concat") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_concat>(thd, this); }
};


/*
  This class handles the || operator in sql_mode=ORACLE.
  Unlike the traditional MariaDB concat(), it treats NULL arguments as ''.
*/
class Item_func_concat_operator_oracle :public Item_func_concat
{
public:
  Item_func_concat_operator_oracle(THD *thd, List<Item> &list)
   :Item_func_concat(thd, list)
  { }
  Item_func_concat_operator_oracle(THD *thd, Item *a, Item *b)
   :Item_func_concat(thd, a, b)
  { }
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("concat_operator_oracle") };
    return name;
  }
  Item *get_copy(THD *thd) override
  {
    return get_item_copy<Item_func_concat_operator_oracle>(thd, this);
  }
};


class Item_func_decode_histogram :public Item_str_func
{
public:
  Item_func_decode_histogram(THD *thd, Item *a, Item *b):
    Item_str_func(thd, a, b) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(system_charset_info);
    max_length= MAX_BLOB_WIDTH;
    set_maybe_null();
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("decode_histogram") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_decode_histogram>(thd, this); }
};

class Item_func_concat_ws :public Item_str_func
{
  String tmp_value;
public:
  Item_func_concat_ws(THD *thd, List<Item> &list): Item_str_func(thd, list) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("concat_ws") };
    return name;
  }
  table_map not_null_tables() const override { return 0; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_concat_ws>(thd, this); }
};


class Item_func_random_bytes : public Item_str_func
{
public:
  Item_func_random_bytes(THD *thd, Item *arg1) : Item_str_func(thd, arg1) {}
  bool fix_length_and_dec(THD *thd) override;
  void update_used_tables() override;
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("random_bytes")};
    return name;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg,
                                     VCOL_NON_DETERMINISTIC | VCOL_NEXTVAL);
  }
  Item *get_copy(THD *thd) override
  {
    return get_item_copy<Item_func_random_bytes>(thd, this);
  }
  static const int MAX_RANDOM_BYTES= 1024;
};


class Item_func_reverse :public Item_str_func
{
  String tmp_value;
public:
  Item_func_reverse(THD *thd, Item *a): Item_str_func(thd, a) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("reverse") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_reverse>(thd, this); }
};


class Item_func_replace :public Item_str_func
{
  String tmp_value,tmp_value2;
public:
  Item_func_replace(THD *thd, Item *org, Item *find, Item *replace):
    Item_str_func(thd, org, find, replace) {}
  String *val_str(String *to) override { return val_str_internal(to, NULL); };
  bool fix_length_and_dec(THD *thd) override;
  String *val_str_internal(String *str, String *empty_string_for_null);
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("replace") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_replace>(thd, this); }
};


class Item_func_replace_oracle :public Item_func_replace
{
  String tmp_emtpystr;
public:
  Item_func_replace_oracle(THD *thd, Item *org, Item *find, Item *replace):
    Item_func_replace(thd, org, find, replace) {}
  String *val_str(String *to) override
  { return val_str_internal(to, &tmp_emtpystr); };
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("replace_oracle") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_replace_oracle>(thd, this); }
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
  void cleanup() override
  {
    DBUG_ENTER("Item_func_regexp_replace::cleanup");
    Item_str_func::cleanup();
    re.cleanup();
    DBUG_VOID_RETURN;
  }
  String *val_str(String *str) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("regexp_replace") };
    return name;
  }
  Item *get_copy(THD *thd) override { return 0;}
};


class Item_func_regexp_substr :public Item_str_func
{
  Regexp_processor_pcre re;
public:
  Item_func_regexp_substr(THD *thd, Item *a, Item *b):
    Item_str_func(thd, a, b)
    {}
  void cleanup() override
  {
    DBUG_ENTER("Item_func_regexp_substr::cleanup");
    Item_str_func::cleanup();
    re.cleanup();
    DBUG_VOID_RETURN;
  }
  String *val_str(String *str) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("regexp_substr") };
    return name;
  }
  Item *get_copy(THD *thd) override { return 0; }
};


class Item_func_insert :public Item_str_func
{
  String tmp_value;
public:
  Item_func_insert(THD *thd, Item *org, Item *start, Item *length,
                   Item *new_str):
    Item_str_func(thd, org, start, length, new_str) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("insert") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_insert>(thd, this); }
};


class Item_str_conv :public Item_str_func
{
protected:
  uint multiply;
  my_charset_conv_case converter;
  String tmp_value;
public:
  Item_str_conv(THD *thd, Item *item): Item_str_func(thd, item) {}
  String *val_str(String *) override;
};


class Item_func_lcase :public Item_str_conv
{
public:
  Item_func_lcase(THD *thd, Item *item): Item_str_conv(thd, item) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("lcase") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_lcase>(thd, this); }
};

class Item_func_ucase :public Item_str_conv
{
public:
  Item_func_ucase(THD *thd, Item *item): Item_str_conv(thd, item) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("ucase") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_ucase>(thd, this); }
};


class Item_func_left :public Item_str_func
{
  String tmp_value;
public:
  Item_func_left(THD *thd, Item *a, Item *b): Item_str_func(thd, a, b) {}
  bool hash_not_null(Hasher *hasher) override;
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("left") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_left>(thd, this); }
};


class Item_func_right :public Item_str_func
{
  String tmp_value;
public:
  Item_func_right(THD *thd, Item *a, Item *b): Item_str_func(thd, a, b) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("right") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_right>(thd, this); }
};


class Item_func_substr :public Item_str_func
{
  String tmp_value;
protected:
  virtual longlong get_position() { return args[1]->val_int(); }
public:
  Item_func_substr(THD *thd, Item *a, Item *b): Item_str_func(thd, a, b) {}
  Item_func_substr(THD *thd, Item *a, Item *b, Item *c):
    Item_str_func(thd, a, b, c) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("substr") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_substr>(thd, this); }
};

class Item_func_sformat :public Item_str_func
{
  String *val_arg;
public:
  Item_func_sformat(THD *thd, List<Item> &list);
  ~Item_func_sformat() { delete [] val_arg; }
  String *val_str(String*) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("sformat") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_sformat>(thd, this); }
};

class Item_func_substr_oracle :public Item_func_substr
{
protected:
  longlong get_position() override
  { longlong pos= args[1]->val_int(); return pos == 0 ? 1 : pos; }
  String *make_empty_result(String *str) override
  { null_value= 1; return NULL; }
public:
  Item_func_substr_oracle(THD *thd, Item *a, Item *b):
    Item_func_substr(thd, a, b) {}
  Item_func_substr_oracle(THD *thd, Item *a, Item *b, Item *c):
    Item_func_substr(thd, a, b, c) {}
  bool fix_length_and_dec(THD *thd) override
  {
    bool res= Item_func_substr::fix_length_and_dec(thd);
    set_maybe_null();
    return res;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("substr_oracle") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_substr_oracle>(thd, this); }
};

class Item_func_substr_index :public Item_str_func
{
  String tmp_value;
public:
  Item_func_substr_index(THD *thd, Item *a,Item *b,Item *c):
    Item_str_func(thd, a, b, c) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("substring_index") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_substr_index>(thd, this); }

};


class Item_func_trim :public Item_str_func
{
protected:
  String tmp_value;
  String remove;
  String *trimmed_value(String *res, uint32 offset, uint32 length)
  {
    if (length == 0)
      return make_empty_result(&tmp_value);

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
  virtual LEX_CSTRING func_name_ext() const
  {
    static LEX_CSTRING name_ext= {STRING_WITH_LEN("") };
    return name_ext;
  }
public:
  Item_func_trim(THD *thd, Item *a, Item *b): Item_str_func(thd, a, b) {}
  Item_func_trim(THD *thd, Item *a): Item_str_func(thd, a) {}
  Sql_mode_dependency value_depends_on_sql_mode() const override;
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("trim") };
    return name;
  }
  void print(String *str, enum_query_type query_type) override;
  virtual LEX_CSTRING mode_name() const { return { "both", 4}; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_trim>(thd, this); }
};


class Item_func_trim_oracle :public Item_func_trim
{
protected:
  String *make_empty_result(String *str) override
  { null_value= 1; return NULL; }
  LEX_CSTRING func_name_ext() const override
  {
    static LEX_CSTRING name_ext= {STRING_WITH_LEN("_oracle") };
    return name_ext;
  }
public:
  Item_func_trim_oracle(THD *thd, Item *a, Item *b):
    Item_func_trim(thd, a, b) {}
  Item_func_trim_oracle(THD *thd, Item *a): Item_func_trim(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("trim_oracle") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    bool res= Item_func_trim::fix_length_and_dec(thd);
    set_maybe_null();
    return res;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_trim_oracle>(thd, this); }
};


class Item_func_ltrim :public Item_func_trim
{
public:
  Item_func_ltrim(THD *thd, Item *a, Item *b): Item_func_trim(thd, a, b) {}
  Item_func_ltrim(THD *thd, Item *a): Item_func_trim(thd, a) {}
  Sql_mode_dependency value_depends_on_sql_mode() const override
  {
    return Item_func::value_depends_on_sql_mode();
  }
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("ltrim") };
    return name;
  }
  LEX_CSTRING mode_name() const override
  { return { STRING_WITH_LEN("leading") }; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_ltrim>(thd, this); }
};


class Item_func_ltrim_oracle :public Item_func_ltrim
{
protected:
  String *make_empty_result(String *str) override
  { null_value= 1; return NULL; }
  LEX_CSTRING func_name_ext() const override
  {
    static LEX_CSTRING name_ext= {STRING_WITH_LEN("_oracle") };
    return name_ext;
  }
public:
  Item_func_ltrim_oracle(THD *thd, Item *a, Item *b):
    Item_func_ltrim(thd, a, b) {}
  Item_func_ltrim_oracle(THD *thd, Item *a): Item_func_ltrim(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("ltrim_oracle") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    bool res= Item_func_ltrim::fix_length_and_dec(thd);
    set_maybe_null();
    return res;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_ltrim_oracle>(thd, this); }
};


class Item_func_rtrim :public Item_func_trim
{
public:
  Item_func_rtrim(THD *thd, Item *a, Item *b): Item_func_trim(thd, a, b) {}
  Item_func_rtrim(THD *thd, Item *a): Item_func_trim(thd, a) {}
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("rtrim") };
    return name;
  }
  LEX_CSTRING mode_name() const override
  { return { STRING_WITH_LEN("trailing") }; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_rtrim>(thd, this); }
};


class Item_func_rtrim_oracle :public Item_func_rtrim
{
protected:
  String *make_empty_result(String *str) override
  { null_value= 1; return NULL; }
  LEX_CSTRING func_name_ext() const override
  {
    static LEX_CSTRING name_ext= {STRING_WITH_LEN("_oracle") };
    return name_ext;
  }
public:
  Item_func_rtrim_oracle(THD *thd, Item *a, Item *b):
    Item_func_rtrim(thd, a, b) {}
  Item_func_rtrim_oracle(THD *thd, Item *a): Item_func_rtrim(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("rtrim_oracle") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    bool res= Item_func_rtrim::fix_length_and_dec(thd);
    set_maybe_null();
    return res;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_rtrim_oracle>(thd, this); }
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
  String *val_str_ascii(String *str) override;
  bool fix_fields(THD *thd, Item **ref) override;
  bool fix_length_and_dec(THD *thd) override
  {
    fix_length_and_charset((alg == 1 ?
                            SCRAMBLED_PASSWORD_CHAR_LENGTH :
                            SCRAMBLED_PASSWORD_CHAR_LENGTH_323),
                           default_charset());
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING password_normal= {STRING_WITH_LEN("password") };
    static LEX_CSTRING password_old= {STRING_WITH_LEN("old_password") };
    return (deflt || alg == 1) ? password_normal : password_old;
  }
  static char *alloc(THD *thd, const char *password, size_t pass_len,
                     enum PW_Alg al);
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_password>(thd, this); }
};



class Item_func_des_encrypt :public Item_str_binary_checksum_func
{
  String tmp_value,tmp_arg;
public:
  Item_func_des_encrypt(THD *thd, Item *a)
   :Item_str_binary_checksum_func(thd, a) {}
  Item_func_des_encrypt(THD *thd, Item *a, Item *b)
   :Item_str_binary_checksum_func(thd, a, b) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("des_encrypt") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_des_encrypt>(thd, this); }
};

class Item_func_des_decrypt :public Item_str_binary_checksum_func
{
  String tmp_value;
public:
  Item_func_des_decrypt(THD *thd, Item *a)
   :Item_str_binary_checksum_func(thd, a) {}
  Item_func_des_decrypt(THD *thd, Item *a, Item *b)
   :Item_str_binary_checksum_func(thd, a, b) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("des_decrypt") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_des_decrypt>(thd, this); }
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
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    max_length = 13;
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("encrypt") };
    return name;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return FALSE;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_encrypt>(thd, this); }
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
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("encode") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_encode>(thd, this); }
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
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("decode") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_decode>(thd, this); }
protected:
  void crypto_transform(String *) override;
};


class Item_func_sysconst :public Item_str_func
{
public:
  Item_func_sysconst(THD *thd): Item_str_func(thd)
  { collation.set(system_charset_info,DERIVATION_SYSCONST); }
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs);
  /*
    Used to create correct Item name in new converted item in
    safe_charset_converter, return string representation of this function
    call
  */
  virtual const char *fully_qualified_func_name() const = 0;
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(fully_qualified_func_name(), arg,
                                     VCOL_SESSION_FUNC);
  }
  bool const_item() const;
};


class Item_func_database :public Item_func_sysconst
{
public:
  Item_func_database(THD *thd): Item_func_sysconst(thd) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= NAME_CHAR_LEN * system_charset_info->mbmaxlen;
    set_maybe_null();
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("database") };
    return name;
  }
  const char *fully_qualified_func_name() const override
  { return "database()"; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_database>(thd, this); }
};


class Item_func_sqlerrm :public Item_func_sysconst
{
public:
  Item_func_sqlerrm(THD *thd): Item_func_sysconst(thd) {}
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("SQLERRM") };
    return name;
  }
  const char *fully_qualified_func_name() const override
  { return "SQLERRM"; }
  void print(String *str, enum_query_type query_type) override
  {
    str->append(func_name_cstring());
  }
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= 512 * system_charset_info->mbmaxlen;
    null_value= false;
    base_flags&= ~item_base_t::MAYBE_NULL;
    return FALSE;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_sqlerrm>(thd, this); }
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
  String *val_str(String *) override
  {
    DBUG_ASSERT(fixed());
    return (null_value ? 0 : &str_value);
  }
  bool fix_fields(THD *thd, Item **ref) override;
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= (uint32) (username_char_length +
                 HOSTNAME_LENGTH + 1) * SYSTEM_CHARSET_MBMAXLEN;
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("user") };
    return name;
  }
  const char *fully_qualified_func_name() const override
  { return "user()"; }
  int save_in_field(Field *field, bool no_conversions) override
  {
    return save_str_value_in_field(field, &str_value);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_user>(thd, this); }
};


class Item_func_current_user :public Item_func_user
{
  Name_resolution_context *context;

public:
  Item_func_current_user(THD *thd, Name_resolution_context *context_arg):
    Item_func_user(thd), context(context_arg) {}
  bool fix_fields(THD *thd, Item **ref) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("current_user") };
    return name;
  }
  const char *fully_qualified_func_name() const override
  { return "current_user()"; }
  bool check_vcol_func_processor(void *arg) override
  {
    context= 0;
    return mark_unsupported_function(fully_qualified_func_name(), arg,
                                     VCOL_SESSION_FUNC);
  }
};


class Item_func_current_role :public Item_func_sysconst
{
  Name_resolution_context *context;

public:
  Item_func_current_role(THD *thd, Name_resolution_context *context_arg):
    Item_func_sysconst(thd), context(context_arg) {}
  bool fix_fields(THD *thd, Item **ref) override;
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= (uint32) username_char_length * SYSTEM_CHARSET_MBMAXLEN;
    return FALSE;
  }
  int save_in_field(Field *field, bool no_conversions) override
  { return save_str_value_in_field(field, &str_value); }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("current_role") };
    return name;
  }
  const char *fully_qualified_func_name() const override
  { return "current_role()"; }
  String *val_str(String *) override
  {
    DBUG_ASSERT(fixed());
    return null_value ? NULL : &str_value;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    context= 0;
    return mark_unsupported_function(fully_qualified_func_name(), arg,
                                     VCOL_SESSION_FUNC);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_current_role>(thd, this); }
};


class Item_func_soundex :public Item_str_func
{
  String tmp_value;
public:
  Item_func_soundex(THD *thd, Item *a): Item_str_func(thd, a) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("soundex") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_soundex>(thd, this); }
};


class Item_func_elt :public Item_str_func
{
public:
  Item_func_elt(THD *thd, List<Item> &list): Item_str_func(thd, list) {}
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *str) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("elt") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_elt>(thd, this); }
};


class Item_func_make_set :public Item_str_func
{
  String tmp_str;

public:
  Item_func_make_set(THD *thd, List<Item> &list): Item_str_func(thd, list) {}
  String *val_str(String *str) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("make_set") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_make_set>(thd, this); }
};


class Item_func_format :public Item_str_ascii_func
{
  const MY_LOCALE *locale;
public:
  Item_func_format(THD *thd, Item *org, Item *dec):
    Item_str_ascii_func(thd, org, dec) {}
  Item_func_format(THD *thd, Item *org, Item *dec, Item *lang):
    Item_str_ascii_func(thd, org, dec, lang) {}

  String *val_str_ascii(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("format") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_format>(thd, this); }
};


class Item_func_char :public Item_str_func
{
public:
  Item_func_char(THD *thd, List<Item> &list): Item_str_func(thd, list)
  { collation.set(&my_charset_bin); }
  Item_func_char(THD *thd, List<Item> &list, CHARSET_INFO *cs):
    Item_str_func(thd, list)
  { collation.set(cs); }
  Item_func_char(THD *thd, Item *arg1, CHARSET_INFO *cs):
    Item_str_func(thd, arg1)
  { collation.set(cs); }
  String *val_str(String *) override;
  void append_char(String * str, int32 num);
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= arg_count * 4;
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("char") };
    return name;
  }
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_char>(thd, this); }
};

class Item_func_chr :public Item_func_char
{
public:
  Item_func_chr(THD *thd, Item *arg1, CHARSET_INFO *cs):
    Item_func_char(thd, arg1, cs) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= 4;
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("chr") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_chr>(thd, this); }
};

class Item_func_repeat :public Item_str_func
{
  String tmp_value;
public:
  Item_func_repeat(THD *thd, Item *arg1, Item *arg2):
    Item_str_func(thd, arg1, arg2) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("repeat") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_repeat>(thd, this); }
};


class Item_func_space :public Item_str_func
{
public:
  Item_func_space(THD *thd, Item *arg1): Item_str_func(thd, arg1) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("space") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_space>(thd, this); }
};


class Item_func_binlog_gtid_pos :public Item_str_func
{
public:
  Item_func_binlog_gtid_pos(THD *thd, Item *arg1, Item *arg2):
    Item_str_func(thd, arg1, arg2) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("binlog_gtid_pos") };
    return name;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_binlog_gtid_pos>(thd, this); }
};


class Item_func_pad: public Item_str_func
{
protected:
  String tmp_value, pad_str;
public:
  Item_func_pad(THD *thd, Item *arg1, Item *arg2, Item *arg3):
    Item_str_func(thd, arg1, arg2, arg3) {}
  Item_func_pad(THD *thd, Item *arg1, Item *arg2):
    Item_str_func(thd, arg1, arg2) {}
  bool fix_length_and_dec(THD *thd) override;
};


class Item_func_rpad :public Item_func_pad
{
public:
  Item_func_rpad(THD *thd, Item *arg1, Item *arg2, Item *arg3):
    Item_func_pad(thd, arg1, arg2, arg3) {}
  Item_func_rpad(THD *thd, Item *arg1, Item *arg2):
    Item_func_pad(thd, arg1, arg2) {}
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("rpad") };
    return name;
  }
  Sql_mode_dependency value_depends_on_sql_mode() const override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_rpad>(thd, this); }
};


class Item_func_rpad_oracle :public Item_func_rpad
{
  String *make_empty_result(String *str) override
  { null_value= 1; return NULL; }
public:
  Item_func_rpad_oracle(THD *thd, Item *arg1, Item *arg2, Item *arg3):
    Item_func_rpad(thd, arg1, arg2, arg3) {}
  Item_func_rpad_oracle(THD *thd, Item *arg1, Item *arg2):
    Item_func_rpad(thd, arg1, arg2) {}
  bool fix_length_and_dec(THD *thd) override
  {
    bool res= Item_func_rpad::fix_length_and_dec(thd);
    set_maybe_null();
    return res;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("rpad_oracle") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_rpad_oracle>(thd, this); }
};


class Item_func_lpad :public Item_func_pad
{
public:
  Item_func_lpad(THD *thd, Item *arg1, Item *arg2, Item *arg3):
    Item_func_pad(thd, arg1, arg2, arg3) {}
  Item_func_lpad(THD *thd, Item *arg1, Item *arg2):
    Item_func_pad(thd, arg1, arg2) {}
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("lpad") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_lpad>(thd, this); }
};


class Item_func_lpad_oracle :public Item_func_lpad
{
  String *make_empty_result(String *str) override
  { null_value= 1; return NULL; }
public:
  Item_func_lpad_oracle(THD *thd, Item *arg1, Item *arg2, Item *arg3):
    Item_func_lpad(thd, arg1, arg2, arg3) {}
  Item_func_lpad_oracle(THD *thd, Item *arg1, Item *arg2):
    Item_func_lpad(thd, arg1, arg2) {}
  bool fix_length_and_dec(THD *thd) override
  {
    bool res= Item_func_lpad::fix_length_and_dec(thd);
    set_maybe_null();
    return res;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("lpad_oracle") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_lpad_oracle>(thd, this); }
};


class Item_func_conv :public Item_str_func
{
public:
  Item_func_conv(THD *thd, Item *a, Item *b, Item *c):
    Item_str_func(thd, a, b, c) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("conv") };
    return name;
  }
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(default_charset());
    fix_char_length(64);
    set_maybe_null();
    return FALSE;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_conv>(thd, this); }
};


class Item_func_hex :public Item_str_ascii_checksum_func
{
protected:
  String tmp_value;
  /*
    Calling arg[0]->type_handler() can be expensive on every row.
    It's a virtual method, and in case if args[0] is a complex Item,
    its type_handler() can call more virtual methods.
    So let's cache it during fix_length_and_dec().
  */
  const Type_handler *m_arg0_type_handler;
public:
  Item_func_hex(THD *thd, Item *a):
    Item_str_ascii_checksum_func(thd, a), m_arg0_type_handler(NULL) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("hex") };
    return name;
  }
  String *val_str_ascii_from_val_int(String *str);
  String *val_str_ascii_from_val_real(String *str);
  String *val_str_ascii_from_val_str(String *str);
  String *val_str_ascii(String *str) override
  {
    DBUG_ASSERT(fixed());
    return m_arg0_type_handler->Item_func_hex_val_str_ascii(this, str);
  }
  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(default_charset(), DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
    decimals=0;
    fix_char_length(args[0]->max_length * 2);
    m_arg0_type_handler= args[0]->type_handler();
    return FALSE;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_hex>(thd, this); }
};

class Item_func_unhex :public Item_str_func
{
  String tmp_value;
public:
  Item_func_unhex(THD *thd, Item *a): Item_str_func(thd, a)
  {
    /* there can be bad hex strings */
    set_maybe_null();
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("unhex") };
    return name;
  }
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(&my_charset_bin);
    decimals=0;
    max_length=(1+args[0]->max_length)/2;
    return FALSE;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_unhex>(thd, this); }
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
  {
    set_maybe_null();
  }
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(args[0]->collation);
    decimals=0;
    max_length= MAX_BLOB_WIDTH;
    return FALSE;
  }
};


class Item_func_like_range_min :public Item_func_like_range
{
public:
  Item_func_like_range_min(THD *thd, Item *a, Item *b):
    Item_func_like_range(thd, a, b, true) { }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("like_range_min") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_like_range_min>(thd, this); }
};


class Item_func_like_range_max :public Item_func_like_range
{
public:
  Item_func_like_range_max(THD *thd, Item *a, Item *b):
    Item_func_like_range(thd, a, b, false) { }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("like_range_max") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_like_range_max>(thd, this); }
};
#endif


class Item_func_binary :public Item_str_func
{
public:
  Item_func_binary(THD *thd, Item *a): Item_str_func(thd, a) {}
  String *val_str(String *a) override
  {
    DBUG_ASSERT(fixed());
    String *tmp=args[0]->val_str(a);
    null_value=args[0]->null_value;
    if (tmp)
      tmp->set_charset(&my_charset_bin);
    return tmp;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(&my_charset_bin);
    max_length=args[0]->max_length;
    return FALSE;
  }
  void print(String *str, enum_query_type query_type) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("cast_as_binary") };
    return name;
  }
  bool need_parentheses_in_default() override { return true; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_binary>(thd, this); }
};


class Item_load_file :public Item_str_func
{
  String tmp_value;
public:
  Item_load_file(THD *thd, Item *a): Item_str_func(thd, a) {}
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("load_file") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(&my_charset_bin, DERIVATION_COERCIBLE);
    set_maybe_null();
    max_length=MAX_BLOB_WIDTH;
    return FALSE;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_load_file>(thd, this); }
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
  String  *val_str(String *str) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("export_set") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_export_set>(thd, this); }
};


class Item_func_quote :public Item_str_func
{
  String tmp_value;
public:
  Item_func_quote(THD *thd, Item *a): Item_str_func(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("quote") };
    return name;
  }
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(args[0]->collation);
    ulonglong max_result_length= (ulonglong) args[0]->max_length * 2 +
                                  2 * collation.collation->mbmaxlen;
    max_length= (uint32) MY_MIN(max_result_length, MAX_BLOB_WIDTH);
    return FALSE;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_quote>(thd, this); }
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
    if (cache_if_const && args[0]->can_eval_in_optimize())
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
        Conversion from an expression with the ASCII repertoire
        to any character set that can store characters U+0000..U+007F
        is safe:
        - All supported multibyte character sets can store U+0000..U+007F
        - All supported 7bit character sets can store U+0000..U+007F
          except those marked with MY_CS_NONASCII (e.g. swe7).
        Other kind of conversions are potentially lossy.
      */
      safe= (args[0]->collation.collation == &my_charset_bin ||
             cs == &my_charset_bin ||
             (cs->state & MY_CS_UNICODE) ||
             (args[0]->collation.repertoire == MY_REPERTOIRE_ASCII &&
              (cs->mbmaxlen > 1 || !(cs->state & MY_CS_NONASCII))));
    }
  }
  String *val_str(String *) override;
  longlong val_int() override
  {
    if (args[0]->result_type() == STRING_RESULT)
      return Item_str_func::val_int();
    longlong res= args[0]->val_int();
    if ((null_value= args[0]->null_value))
      return 0;
    return res;
  }
  double val_real() override
  {
    if (args[0]->result_type() == STRING_RESULT)
      return Item_str_func::val_real();
    double res= args[0]->val_real();
    if ((null_value= args[0]->null_value))
      return 0;
    return res;
  }
  my_decimal *val_decimal(my_decimal *d) override
  {
    if (args[0]->result_type() == STRING_RESULT)
      return Item_str_func::val_decimal(d);
    my_decimal *res= args[0]->val_decimal(d);
    if ((null_value= args[0]->null_value))
      return NULL;
    return res;
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    if (args[0]->result_type() == STRING_RESULT)
      return Item_str_func::get_date(thd, ltime, fuzzydate);
    bool res= args[0]->get_date(thd, ltime, fuzzydate);
    if ((null_value= args[0]->null_value))
      return 1;
    return res;
  }
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("convert") };
    return name;
  }
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_conv_charset>(thd, this); }
  int save_in_field(Field*, bool) override;
};

class Item_func_set_collation :public Item_str_func
{
  Lex_extended_collation_st m_set_collation;
public:
  Item_func_set_collation(THD *thd, Item *a,
                          const Lex_extended_collation_st &set_collation):
    Item_str_func(thd, a), m_set_collation(set_collation) {}
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  bool eq(const Item *item, bool binary_cmp) const override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("collate") };
    return name;
  }
  enum precedence precedence() const override { return COLLATE_PRECEDENCE; }
  enum Functype functype() const override { return COLLATE_FUNC; }
  void print(String *str, enum_query_type query_type) override;
  Item_field *field_for_view_update() override
  {
    /* this function is transparent for view updating */
    return args[0]->field_for_view_update();
  }
  bool need_parentheses_in_default() override { return true; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_set_collation>(thd, this); }
};


class Item_func_expr_str_metadata :public Item_str_func
{
public:
  Item_func_expr_str_metadata(THD *thd, Item *a): Item_str_func(thd, a) { }
  bool fix_length_and_dec(THD *thd) override
  {
     collation.set(system_charset_info);
     max_length= 64 * collation.collation->mbmaxlen; // should be enough
     base_flags&= ~item_base_t::MAYBE_NULL;
     return FALSE;
  };
  table_map not_null_tables() const override { return 0; }
  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
    override
  { return this; }
  bool const_item() const override { return true; }
};


class Item_func_charset :public Item_func_expr_str_metadata
{
public:
  Item_func_charset(THD *thd, Item *a)
    :Item_func_expr_str_metadata(thd, a) { }
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("charset") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_charset>(thd, this); }
};


class Item_func_collation :public Item_func_expr_str_metadata
{
public:
  Item_func_collation(THD *thd, Item *a)
    :Item_func_expr_str_metadata(thd, a) {}
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("collation") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_collation>(thd, this); }
};


class Item_func_weight_string :public Item_str_func
{
  String tmp_value;
  uint weigth_flags;
  uint nweights;
  uint result_length;
public:
  Item_func_weight_string(THD *thd, Item *a, uint result_length_arg,
                          uint nweights_arg, uint flags_arg):
    Item_str_func(thd, a)
  {
    nweights= nweights_arg;
    weigth_flags= flags_arg;
    result_length= result_length_arg;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("weight_string") };
    return name;
  }
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  bool eq(const Item *item, bool binary_cmp) const override
  {
    if (!Item_str_func::eq(item, binary_cmp))
      return false;
    Item_func_weight_string *that= (Item_func_weight_string *)item;
    return this->weigth_flags == that->weigth_flags &&
           this->nweights == that->nweights &&
           this->result_length == that->result_length;
  }
  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
    override
  { return this; }
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_weight_string>(thd, this); }
};

class Item_func_crc32 :public Item_long_func
{
  bool check_arguments() const override
  {
    return args[0]->check_type_can_return_str(func_name_cstring()) &&
      (arg_count == 1 ||
       args[1]->check_type_can_return_int(func_name_cstring()));
  }
  String value;
  uint32 (*const crc_func)(uint32, const void*, size_t);
public:
  Item_func_crc32(THD *thd, bool Castagnoli, Item *a) :
    Item_long_func(thd, a),
    crc_func(Castagnoli ? my_crc32c : my_checksum)
  { unsigned_flag= 1; }
  Item_func_crc32(THD *thd, bool Castagnoli, Item *a, Item *b) :
    Item_long_func(thd, a, b),
    crc_func(Castagnoli ? my_crc32c : my_checksum)
  { unsigned_flag= 1; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING crc32_name= {STRING_WITH_LEN("crc32") };
    static LEX_CSTRING crc32c_name= {STRING_WITH_LEN("crc32c") };
    return crc_func == my_crc32c ? crc32c_name : crc32_name;
  }
  bool fix_length_and_dec(THD *thd) override { max_length=10; return FALSE; }
  longlong val_int() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_crc32>(thd, this); }
};

class Item_func_uncompressed_length : public Item_long_func_length
{
  String value;
public:
  Item_func_uncompressed_length(THD *thd, Item *a)
   :Item_long_func_length(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("uncompressed_length") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    max_length=10;
    set_maybe_null();
    return FALSE; }
  longlong val_int() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_uncompressed_length>(thd, this); }
};

#ifdef HAVE_COMPRESS
#define ZLIB_DEPENDED_FUNCTION ;
#else
#define ZLIB_DEPENDED_FUNCTION { null_value=1; return 0; }
#endif

class Item_func_compress: public Item_str_binary_checksum_func
{
  String tmp_value;
public:
  Item_func_compress(THD *thd, Item *a)
   :Item_str_binary_checksum_func(thd, a) {}
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= (args[0]->max_length * 120) / 100 + 12;
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("compress") };
    return name;
  }
  String *val_str(String *) override ZLIB_DEPENDED_FUNCTION
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_compress>(thd, this); }
};

class Item_func_uncompress: public Item_str_binary_checksum_func
{
  String tmp_value;
public:
  Item_func_uncompress(THD *thd, Item *a)
   :Item_str_binary_checksum_func(thd, a) {}
  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    max_length= MAX_BLOB_WIDTH;
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("uncompress") };
    return name;
  }
  String *val_str(String *) override ZLIB_DEPENDED_FUNCTION
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_uncompress>(thd, this); }
};


class Item_func_dyncol_create: public Item_str_func
{
protected:
  DYNCALL_CREATE_DEF *defs;
  DYNAMIC_COLUMN_VALUE *vals;
  uint *keys_num;
  LEX_STRING *keys_str;
  bool names, force_names;
  bool prepare_arguments(THD *thd, bool force_names);
  void print_arguments(String *str, enum_query_type query_type);
public:
  Item_func_dyncol_create(THD *thd, List<Item> &args, DYNCALL_CREATE_DEF *dfs);
  bool fix_fields(THD *thd, Item **ref) override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("column_create") };
    return name;
  }
  String *val_str(String *) override;
  void print(String *str, enum_query_type query_type) override;
  enum Functype functype() const override { return DYNCOL_FUNC; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_dyncol_create>(thd, this); }
};


class Item_func_dyncol_add: public Item_func_dyncol_create
{
public:
  Item_func_dyncol_add(THD *thd, List<Item> &args_arg, DYNCALL_CREATE_DEF *dfs):
    Item_func_dyncol_create(thd, args_arg, dfs)
  {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("column_add") };
    return name;
  }
  String *val_str(String *) override;
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_dyncol_add>(thd, this); }
};

class Item_func_dyncol_json: public Item_str_func
{
public:
  Item_func_dyncol_json(THD *thd, Item *str): Item_str_func(thd, str)
    {collation.set(DYNCOL_UTF);}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("column_json") };
    return name;
  }
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= MAX_BLOB_WIDTH;
    set_maybe_null();
    decimals= 0;
    return FALSE;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_dyncol_json>(thd, this); }
};

/*
  The following functions is always called from an Item_cast function
*/

class Item_dyncol_get: public Item_str_func
{
public:
  Item_dyncol_get(THD *thd, Item *str, Item *num): Item_str_func(thd, str, num)
  {}
  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    max_length= MAX_BLOB_WIDTH;
    return FALSE;
  }
  /* Mark that collation can change between calls */
  bool dynamic_result() override { return 1; }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("column_get") };
    return name;
  }
  String *val_str(String *) override;
  longlong val_int() override;
  longlong val_int_signed_typecast() override
  {
    unsigned_flag= false;   // Mark that we want to have a signed value
    longlong value= val_int(); // val_int() can change unsigned_flag
    if (!null_value && unsigned_flag && value < 0)
      push_note_converted_to_negative_complement(current_thd);
    return value;
  }
  longlong val_int_unsigned_typecast() override
  {
    unsigned_flag= true; // Mark that we want to have an unsigned value
    longlong value= val_int(); // val_int() can change unsigned_flag
    if (!null_value && unsigned_flag == 0 && value < 0)
      push_note_converted_to_positive_complement(current_thd);
    return value;
  }
  double val_real() override;
  my_decimal *val_decimal(my_decimal *) override;
  bool get_dyn_value(THD *thd, DYNAMIC_COLUMN_VALUE *val, String *tmp);
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_dyncol_get>(thd, this); }
};


class Item_func_dyncol_list: public Item_str_func
{
public:
  Item_func_dyncol_list(THD *thd, Item *str): Item_str_func(thd, str)
    {collation.set(DYNCOL_UTF);}
  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    max_length= MAX_BLOB_WIDTH;
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("column_list") };
    return name;
  }
  String *val_str(String *) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_dyncol_list>(thd, this); }
};

/*
  this is used by JOIN_TAB::keep_current_rowid
  and stores handler::position().
  It has nothing to do with _rowid pseudo-column, that the parser supports.
*/
class Item_temptable_rowid :public Item_str_func
{
public:
  TABLE *table;
  Item_temptable_rowid(TABLE *table_arg);
  const Type_handler *type_handler() const override
  { return &type_handler_string; }
  Field *create_tmp_field(MEM_ROOT *root, bool group, TABLE *table)
  { return create_table_field_from_handler(root, table); }
  String *val_str(String *str) override;
  enum Functype functype() const override { return  TEMPTABLE_ROWID; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("<rowid>") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_temptable_rowid>(thd, this); }
};
#ifdef WITH_WSREP

#include "wsrep_api.h"

class Item_func_wsrep_last_written_gtid: public Item_str_ascii_func
{
  String gtid_str;
public:
  Item_func_wsrep_last_written_gtid(THD *thd): Item_str_ascii_func(thd) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("wsrep_last_written_gtid") };
    return name;
  }
  String *val_str_ascii(String *) override;
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= WSREP_GTID_STR_LEN;
    set_maybe_null();
    return FALSE;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_wsrep_last_written_gtid>(thd, this); }
};

class Item_func_wsrep_last_seen_gtid: public Item_str_ascii_func
{
  String gtid_str;
public:
  Item_func_wsrep_last_seen_gtid(THD *thd): Item_str_ascii_func(thd) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("wsrep_last_seen_gtid") };
    return name;
  }
  String *val_str_ascii(String *) override;
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= WSREP_GTID_STR_LEN;
    set_maybe_null();
    return FALSE;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_wsrep_last_seen_gtid>(thd, this); }
};

class Item_func_wsrep_sync_wait_upto: public Item_int_func
{
  String value;
public:
 Item_func_wsrep_sync_wait_upto(THD *thd, Item *a): Item_int_func(thd, a) {}
 Item_func_wsrep_sync_wait_upto(THD *thd, Item *a, Item* b): Item_int_func(thd, a, b) {}
  const Type_handler *type_handler() const override
  { return &type_handler_string; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("wsrep_sync_wait_upto_gtid") };
    return name;
  }
  longlong val_int() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_wsrep_sync_wait_upto>(thd, this); }
};
#endif /* WITH_WSREP */

#endif /* ITEM_STRFUNC_INCLUDED */

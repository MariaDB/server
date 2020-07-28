/*
   Copyright (c) 2000, 2011, Oracle and/or its affiliates.
   Copyright (c) 2008, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file

  @brief
  Functions to create an item. Used by sql_yac.yy
*/

#include "mariadb.h"
#include "sql_priv.h"
/*
  It is necessary to include set_var.h instead of item.h because there
  are dependencies on include order for set_var.h and item.h. This
  will be resolved later.
*/
#include "sql_class.h"                          // set_var.h: THD
#include "set_var.h"
#include "sp_head.h"
#include "sp.h"
#include "sql_time.h"
#include "sql_type_geom.h"
#include <mysql/plugin_function.h>


extern "C" uchar*
get_native_fct_hash_key(const uchar *buff, size_t *length,
                        my_bool /* unused */)
{
  Native_func_registry *func= (Native_func_registry*) buff;
  *length= func->name.length;
  return (uchar*) func->name.str;
}


bool Native_func_registry_array::append_to_hash(HASH *hash) const
{
  DBUG_ENTER("Native_func_registry_array::append_to_hash");
  for (size_t i= 0; i < count(); i++)
  {
    const Native_func_registry &func= element(i);
    DBUG_ASSERT(func.builder != NULL);
    if (my_hash_insert(hash, (uchar*) &func))
      DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}


#ifdef HAVE_SPATIAL
extern Native_func_registry_array native_func_registry_array_geom;
#endif


/*
=============================================================================
  LOCAL DECLARATIONS
=============================================================================
*/

/**
  Function builder for Stored Functions.
*/

class Create_sp_func : public Create_qfunc
{
public:
  virtual Item *create_with_db(THD *thd, LEX_CSTRING *db, LEX_CSTRING *name,
                               bool use_explicit_name, List<Item> *item_list);

  static Create_sp_func s_singleton;

protected:
  /** Constructor. */
  Create_sp_func() {}
  /** Destructor. */
  virtual ~Create_sp_func() {}
};


/*
  Concrete functions builders (native functions).
  Please keep this list sorted in alphabetical order,
  it helps to compare code between versions, and helps with merges conflicts.
*/

class Create_func_abs : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_abs s_singleton;

protected:
  Create_func_abs() {}
  virtual ~Create_func_abs() {}
};


class Create_func_acos : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_acos s_singleton;

protected:
  Create_func_acos() {}
  virtual ~Create_func_acos() {}
};


class Create_func_addtime : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_addtime s_singleton;

protected:
  Create_func_addtime() {}
  virtual ~Create_func_addtime() {}
};


class Create_func_aes_encrypt : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_aes_encrypt s_singleton;

protected:
  Create_func_aes_encrypt() {}
  virtual ~Create_func_aes_encrypt() {}
};


class Create_func_aes_decrypt : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_aes_decrypt s_singleton;

protected:
  Create_func_aes_decrypt() {}
  virtual ~Create_func_aes_decrypt() {}
};


class Create_func_asin : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_asin s_singleton;

protected:
  Create_func_asin() {}
  virtual ~Create_func_asin() {}
};


class Create_func_atan : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_atan s_singleton;

protected:
  Create_func_atan() {}
  virtual ~Create_func_atan() {}
};


class Create_func_benchmark : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_benchmark s_singleton;

protected:
  Create_func_benchmark() {}
  virtual ~Create_func_benchmark() {}
};


class Create_func_bin : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_bin s_singleton;

protected:
  Create_func_bin() {}
  virtual ~Create_func_bin() {}
};


class Create_func_binlog_gtid_pos : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_binlog_gtid_pos s_singleton;

protected:
  Create_func_binlog_gtid_pos() {}
  virtual ~Create_func_binlog_gtid_pos() {}
};


class Create_func_bit_count : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_bit_count s_singleton;

protected:
  Create_func_bit_count() {}
  virtual ~Create_func_bit_count() {}
};


class Create_func_bit_length : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_bit_length s_singleton;

protected:
  Create_func_bit_length() {}
  virtual ~Create_func_bit_length() {}
};


class Create_func_ceiling : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_ceiling s_singleton;

protected:
  Create_func_ceiling() {}
  virtual ~Create_func_ceiling() {}
};


class Create_func_chr : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_chr s_singleton;

protected:
  Create_func_chr() {}
  virtual ~Create_func_chr() {}
};


class Create_func_char_length : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_char_length s_singleton;

protected:
  Create_func_char_length() {}
  virtual ~Create_func_char_length() {}
};


class Create_func_coercibility : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_coercibility s_singleton;

protected:
  Create_func_coercibility() {}
  virtual ~Create_func_coercibility() {}
};

class Create_func_dyncol_check : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_dyncol_check s_singleton;

protected:
  Create_func_dyncol_check() {}
  virtual ~Create_func_dyncol_check() {}
};

class Create_func_dyncol_exists : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_dyncol_exists s_singleton;

protected:
  Create_func_dyncol_exists() {}
  virtual ~Create_func_dyncol_exists() {}
};

class Create_func_dyncol_list : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_dyncol_list s_singleton;

protected:
  Create_func_dyncol_list() {}
  virtual ~Create_func_dyncol_list() {}
};

class Create_func_dyncol_json : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_dyncol_json s_singleton;

protected:
  Create_func_dyncol_json() {}
  virtual ~Create_func_dyncol_json() {}
};


class Create_func_compress : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_compress s_singleton;

protected:
  Create_func_compress() {}
  virtual ~Create_func_compress() {}
};


class Create_func_concat : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_concat s_singleton;

protected:
  Create_func_concat() {}
  virtual ~Create_func_concat() {}
};


class Create_func_concat_operator_oracle : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_concat_operator_oracle s_singleton;

protected:
  Create_func_concat_operator_oracle() {}
  virtual ~Create_func_concat_operator_oracle() {}
};


class Create_func_decode_histogram : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_decode_histogram s_singleton;

protected:
  Create_func_decode_histogram() {}
  virtual ~Create_func_decode_histogram() {}
};


class Create_func_decode_oracle : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_decode_oracle s_singleton;

protected:
  Create_func_decode_oracle() {}
  virtual ~Create_func_decode_oracle() {}
};


class Create_func_concat_ws : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_concat_ws s_singleton;

protected:
  Create_func_concat_ws() {}
  virtual ~Create_func_concat_ws() {}
};


class Create_func_connection_id : public Create_func_arg0
{
public:
  virtual Item *create_builder(THD *thd);

  static Create_func_connection_id s_singleton;

protected:
  Create_func_connection_id() {}
  virtual ~Create_func_connection_id() {}
};


class Create_func_nvl2 : public Create_func_arg3
{
public:
  virtual Item *create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3);

  static Create_func_nvl2 s_singleton;

protected:
  Create_func_nvl2() {}
  virtual ~Create_func_nvl2() {}
};


class Create_func_conv : public Create_func_arg3
{
public:
  virtual Item *create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3);

  static Create_func_conv s_singleton;

protected:
  Create_func_conv() {}
  virtual ~Create_func_conv() {}
};


class Create_func_convert_tz : public Create_func_arg3
{
public:
  virtual Item *create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3);

  static Create_func_convert_tz s_singleton;

protected:
  Create_func_convert_tz() {}
  virtual ~Create_func_convert_tz() {}
};


class Create_func_cos : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_cos s_singleton;

protected:
  Create_func_cos() {}
  virtual ~Create_func_cos() {}
};


class Create_func_cot : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_cot s_singleton;

protected:
  Create_func_cot() {}
  virtual ~Create_func_cot() {}
};


class Create_func_crc32 : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_crc32 s_singleton;

protected:
  Create_func_crc32() {}
  virtual ~Create_func_crc32() {}
};


class Create_func_datediff : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_datediff s_singleton;

protected:
  Create_func_datediff() {}
  virtual ~Create_func_datediff() {}
};


class Create_func_dayname : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_dayname s_singleton;

protected:
  Create_func_dayname() {}
  virtual ~Create_func_dayname() {}
};


class Create_func_dayofmonth : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_dayofmonth s_singleton;

protected:
  Create_func_dayofmonth() {}
  virtual ~Create_func_dayofmonth() {}
};


class Create_func_dayofweek : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_dayofweek s_singleton;

protected:
  Create_func_dayofweek() {}
  virtual ~Create_func_dayofweek() {}
};


class Create_func_dayofyear : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_dayofyear s_singleton;

protected:
  Create_func_dayofyear() {}
  virtual ~Create_func_dayofyear() {}
};


class Create_func_degrees : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_degrees s_singleton;

protected:
  Create_func_degrees() {}
  virtual ~Create_func_degrees() {}
};


class Create_func_des_decrypt : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_des_decrypt s_singleton;

protected:
  Create_func_des_decrypt() {}
  virtual ~Create_func_des_decrypt() {}
};


class Create_func_des_encrypt : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_des_encrypt s_singleton;

protected:
  Create_func_des_encrypt() {}
  virtual ~Create_func_des_encrypt() {}
};


class Create_func_elt : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_elt s_singleton;

protected:
  Create_func_elt() {}
  virtual ~Create_func_elt() {}
};


class Create_func_encode : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_encode s_singleton;

protected:
  Create_func_encode() {}
  virtual ~Create_func_encode() {}
};


class Create_func_encrypt : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_encrypt s_singleton;

protected:
  Create_func_encrypt() {}
  virtual ~Create_func_encrypt() {}
};


class Create_func_exp : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_exp s_singleton;

protected:
  Create_func_exp() {}
  virtual ~Create_func_exp() {}
};


class Create_func_export_set : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_export_set s_singleton;

protected:
  Create_func_export_set() {}
  virtual ~Create_func_export_set() {}
};


class Create_func_field : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_field s_singleton;

protected:
  Create_func_field() {}
  virtual ~Create_func_field() {}
};


class Create_func_find_in_set : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_find_in_set s_singleton;

protected:
  Create_func_find_in_set() {}
  virtual ~Create_func_find_in_set() {}
};


class Create_func_floor : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_floor s_singleton;

protected:
  Create_func_floor() {}
  virtual ~Create_func_floor() {}
};


class Create_func_format : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_format s_singleton;

protected:
  Create_func_format() {}
  virtual ~Create_func_format() {}
};


class Create_func_found_rows : public Create_func_arg0
{
public:
  virtual Item *create_builder(THD *thd);

  static Create_func_found_rows s_singleton;

protected:
  Create_func_found_rows() {}
  virtual ~Create_func_found_rows() {}
};


class Create_func_from_base64 : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_from_base64 s_singleton;

protected:
  Create_func_from_base64() {}
  virtual ~Create_func_from_base64() {}
};


class Create_func_from_days : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_from_days s_singleton;

protected:
  Create_func_from_days() {}
  virtual ~Create_func_from_days() {}
};


class Create_func_from_unixtime : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_from_unixtime s_singleton;

protected:
  Create_func_from_unixtime() {}
  virtual ~Create_func_from_unixtime() {}
};


class Create_func_get_lock : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_get_lock s_singleton;

protected:
  Create_func_get_lock() {}
  virtual ~Create_func_get_lock() {}
};


class Create_func_greatest : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_greatest s_singleton;

protected:
  Create_func_greatest() {}
  virtual ~Create_func_greatest() {}
};


class Create_func_hex : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_hex s_singleton;

protected:
  Create_func_hex() {}
  virtual ~Create_func_hex() {}
};


class Create_func_ifnull : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_ifnull s_singleton;

protected:
  Create_func_ifnull() {}
  virtual ~Create_func_ifnull() {}
};


class Create_func_instr : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_instr s_singleton;

protected:
  Create_func_instr() {}
  virtual ~Create_func_instr() {}
};


class Create_func_is_free_lock : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_is_free_lock s_singleton;

protected:
  Create_func_is_free_lock() {}
  virtual ~Create_func_is_free_lock() {}
};


class Create_func_is_used_lock : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_is_used_lock s_singleton;

protected:
  Create_func_is_used_lock() {}
  virtual ~Create_func_is_used_lock() {}
};


class Create_func_isnull : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_isnull s_singleton;

protected:
  Create_func_isnull() {}
  virtual ~Create_func_isnull() {}
};


class Create_func_json_exists : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_json_exists s_singleton;

protected:
  Create_func_json_exists() {}
  virtual ~Create_func_json_exists() {}
};


class Create_func_json_valid : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_json_valid s_singleton;

protected:
  Create_func_json_valid() {}
  virtual ~Create_func_json_valid() {}
};


class Create_func_json_compact : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_json_compact s_singleton;

protected:
  Create_func_json_compact() {}
  virtual ~Create_func_json_compact() {}
};


class Create_func_json_loose : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_json_loose s_singleton;

protected:
  Create_func_json_loose() {}
  virtual ~Create_func_json_loose() {}
};


class Create_func_json_detailed: public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_detailed s_singleton;

protected:
  Create_func_json_detailed() {}
  virtual ~Create_func_json_detailed() {}
};


class Create_func_json_type : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_json_type s_singleton;

protected:
  Create_func_json_type() {}
  virtual ~Create_func_json_type() {}
};


class Create_func_json_depth : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_json_depth s_singleton;

protected:
  Create_func_json_depth() {}
  virtual ~Create_func_json_depth() {}
};


class Create_func_json_value : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_json_value s_singleton;

protected:
  Create_func_json_value() {}
  virtual ~Create_func_json_value() {}
};


class Create_func_json_query : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_json_query s_singleton;

protected:
  Create_func_json_query() {}
  virtual ~Create_func_json_query() {}
};


class Create_func_json_keys: public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_keys s_singleton;

protected:
  Create_func_json_keys() {}
  virtual ~Create_func_json_keys() {}
};


class Create_func_json_contains: public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_contains s_singleton;

protected:
  Create_func_json_contains() {}
  virtual ~Create_func_json_contains() {}
};


class Create_func_json_contains_path : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_contains_path s_singleton;

protected:
  Create_func_json_contains_path() {}
  virtual ~Create_func_json_contains_path() {}
};


class Create_func_json_extract : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_extract s_singleton;

protected:
  Create_func_json_extract() {}
  virtual ~Create_func_json_extract() {}
};


class Create_func_json_search : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_search s_singleton;

protected:
  Create_func_json_search() {}
  virtual ~Create_func_json_search() {}
};


class Create_func_json_array : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_array s_singleton;

protected:
  Create_func_json_array() {}
  virtual ~Create_func_json_array() {}
};


class Create_func_json_array_append : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_array_append s_singleton;

protected:
  Create_func_json_array_append() {}
  virtual ~Create_func_json_array_append() {}
};


class Create_func_json_array_insert : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_array_insert s_singleton;

protected:
  Create_func_json_array_insert() {}
  virtual ~Create_func_json_array_insert() {}
};


class Create_func_json_insert : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_insert s_singleton;

protected:
  Create_func_json_insert() {}
  virtual ~Create_func_json_insert() {}
};


class Create_func_json_set : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_set s_singleton;

protected:
  Create_func_json_set() {}
  virtual ~Create_func_json_set() {}
};


class Create_func_json_replace : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_replace s_singleton;

protected:
  Create_func_json_replace() {}
  virtual ~Create_func_json_replace() {}
};


class Create_func_json_remove : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_remove s_singleton;

protected:
  Create_func_json_remove() {}
  virtual ~Create_func_json_remove() {}
};


class Create_func_json_object : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_object s_singleton;

protected:
  Create_func_json_object() {}
  virtual ~Create_func_json_object() {}
};


class Create_func_json_length : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_length s_singleton;

protected:
  Create_func_json_length() {}
  virtual ~Create_func_json_length() {}
};


class Create_func_json_merge : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_merge s_singleton;

protected:
  Create_func_json_merge() {}
  virtual ~Create_func_json_merge() {}
};


class Create_func_json_merge_patch : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_json_merge_patch s_singleton;

protected:
  Create_func_json_merge_patch() {}
  virtual ~Create_func_json_merge_patch() {}
};


class Create_func_json_quote : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_json_quote s_singleton;

protected:
  Create_func_json_quote() {}
  virtual ~Create_func_json_quote() {}
};


class Create_func_json_unquote : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_json_unquote s_singleton;

protected:
  Create_func_json_unquote() {}
  virtual ~Create_func_json_unquote() {}
};


class Create_func_last_day : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_last_day s_singleton;

protected:
  Create_func_last_day() {}
  virtual ~Create_func_last_day() {}
};


class Create_func_last_insert_id : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_last_insert_id s_singleton;

protected:
  Create_func_last_insert_id() {}
  virtual ~Create_func_last_insert_id() {}
};


class Create_func_lcase : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_lcase s_singleton;

protected:
  Create_func_lcase() {}
  virtual ~Create_func_lcase() {}
};


class Create_func_least : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_least s_singleton;

protected:
  Create_func_least() {}
  virtual ~Create_func_least() {}
};


class Create_func_length : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_length s_singleton;

protected:
  Create_func_length() {}
  virtual ~Create_func_length() {}
};

class Create_func_octet_length : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_octet_length s_singleton;

protected:
  Create_func_octet_length() {}
  virtual ~Create_func_octet_length() {}
};


#ifndef DBUG_OFF
class Create_func_like_range_min : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_like_range_min s_singleton;

protected:
  Create_func_like_range_min() {}
  virtual ~Create_func_like_range_min() {}
};


class Create_func_like_range_max : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_like_range_max s_singleton;

protected:
  Create_func_like_range_max() {}
  virtual ~Create_func_like_range_max() {}
};
#endif


class Create_func_ln : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_ln s_singleton;

protected:
  Create_func_ln() {}
  virtual ~Create_func_ln() {}
};


class Create_func_load_file : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_load_file s_singleton;

protected:
  Create_func_load_file() {}
  virtual ~Create_func_load_file() {}
};


class Create_func_locate : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_locate s_singleton;

protected:
  Create_func_locate() {}
  virtual ~Create_func_locate() {}
};


class Create_func_log : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_log s_singleton;

protected:
  Create_func_log() {}
  virtual ~Create_func_log() {}
};


class Create_func_log10 : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_log10 s_singleton;

protected:
  Create_func_log10() {}
  virtual ~Create_func_log10() {}
};


class Create_func_log2 : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_log2 s_singleton;

protected:
  Create_func_log2() {}
  virtual ~Create_func_log2() {}
};


class Create_func_lpad : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name,
                              List<Item> *item_list)
  {
    return thd->variables.sql_mode & MODE_ORACLE ?
           create_native_oracle(thd, name, item_list) :
           create_native_std(thd, name, item_list);
  }
  static Create_func_lpad s_singleton;

protected:
  Create_func_lpad() {}
  virtual ~Create_func_lpad() {}
  Item *create_native_std(THD *thd, LEX_CSTRING *name, List<Item> *items);
  Item *create_native_oracle(THD *thd, LEX_CSTRING *name, List<Item> *items);
};


class Create_func_lpad_oracle : public Create_func_lpad
{
public:
  Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list)
  {
    return create_native_oracle(thd, name, item_list);
  }
  static Create_func_lpad_oracle s_singleton;
};


class Create_func_ltrim : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_ltrim s_singleton;

protected:
  Create_func_ltrim() {}
  virtual ~Create_func_ltrim() {}
};


class Create_func_ltrim_oracle : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_ltrim_oracle s_singleton;

protected:
  Create_func_ltrim_oracle() {}
  virtual ~Create_func_ltrim_oracle() {}
};


class Create_func_makedate : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_makedate s_singleton;

protected:
  Create_func_makedate() {}
  virtual ~Create_func_makedate() {}
};


class Create_func_maketime : public Create_func_arg3
{
public:
  virtual Item *create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3);

  static Create_func_maketime s_singleton;

protected:
  Create_func_maketime() {}
  virtual ~Create_func_maketime() {}
};


class Create_func_make_set : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_make_set s_singleton;

protected:
  Create_func_make_set() {}
  virtual ~Create_func_make_set() {}
};


class Create_func_master_pos_wait : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_master_pos_wait s_singleton;

protected:
  Create_func_master_pos_wait() {}
  virtual ~Create_func_master_pos_wait() {}
};


class Create_func_master_gtid_wait : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_master_gtid_wait s_singleton;

protected:
  Create_func_master_gtid_wait() {}
  virtual ~Create_func_master_gtid_wait() {}
};


class Create_func_md5 : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_md5 s_singleton;

protected:
  Create_func_md5() {}
  virtual ~Create_func_md5() {}
};


class Create_func_monthname : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_monthname s_singleton;

protected:
  Create_func_monthname() {}
  virtual ~Create_func_monthname() {}
};


class Create_func_name_const : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_name_const s_singleton;

protected:
  Create_func_name_const() {}
  virtual ~Create_func_name_const() {}
};


class Create_func_nullif : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_nullif s_singleton;

protected:
  Create_func_nullif() {}
  virtual ~Create_func_nullif() {}
};


class Create_func_oct : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_oct s_singleton;

protected:
  Create_func_oct() {}
  virtual ~Create_func_oct() {}
};


class Create_func_ord : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_ord s_singleton;

protected:
  Create_func_ord() {}
  virtual ~Create_func_ord() {}
};


class Create_func_period_add : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_period_add s_singleton;

protected:
  Create_func_period_add() {}
  virtual ~Create_func_period_add() {}
};


class Create_func_period_diff : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_period_diff s_singleton;

protected:
  Create_func_period_diff() {}
  virtual ~Create_func_period_diff() {}
};


class Create_func_pi : public Create_func_arg0
{
public:
  virtual Item *create_builder(THD *thd);

  static Create_func_pi s_singleton;

protected:
  Create_func_pi() {}
  virtual ~Create_func_pi() {}
};


class Create_func_pow : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_pow s_singleton;

protected:
  Create_func_pow() {}
  virtual ~Create_func_pow() {}
};


class Create_func_quote : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_quote s_singleton;

protected:
  Create_func_quote() {}
  virtual ~Create_func_quote() {}
};


class Create_func_regexp_instr : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_regexp_instr s_singleton;

protected:
  Create_func_regexp_instr() {}
  virtual ~Create_func_regexp_instr() {}
};


class Create_func_regexp_replace : public Create_func_arg3
{
public:
  virtual Item *create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3);

  static Create_func_regexp_replace s_singleton;

protected:
  Create_func_regexp_replace() {}
  virtual ~Create_func_regexp_replace() {}
};


class Create_func_regexp_substr : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_regexp_substr s_singleton;

protected:
  Create_func_regexp_substr() {}
  virtual ~Create_func_regexp_substr() {}
};


class Create_func_radians : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_radians s_singleton;

protected:
  Create_func_radians() {}
  virtual ~Create_func_radians() {}
};


class Create_func_rand : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_rand s_singleton;

protected:
  Create_func_rand() {}
  virtual ~Create_func_rand() {}
};


class Create_func_release_all_locks : public Create_func_arg0
{
public:
  virtual Item *create_builder(THD *thd);

  static Create_func_release_all_locks s_singleton;
};


class Create_func_release_lock : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_release_lock s_singleton;

protected:
  Create_func_release_lock() {}
  virtual ~Create_func_release_lock() {}
};


class Create_func_replace_oracle : public Create_func_arg3
{
public:
  virtual Item *create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3);

  static Create_func_replace_oracle s_singleton;

protected:
  Create_func_replace_oracle() {}
  virtual ~Create_func_replace_oracle() {}
};


class Create_func_reverse : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_reverse s_singleton;

protected:
  Create_func_reverse() {}
  virtual ~Create_func_reverse() {}
};


class Create_func_round : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_round s_singleton;

protected:
  Create_func_round() {}
  virtual ~Create_func_round() {}
};


class Create_func_rpad : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name,
                              List<Item> *item_list)
  {
    return thd->variables.sql_mode & MODE_ORACLE ?
           create_native_oracle(thd, name, item_list) :
           create_native_std(thd, name, item_list);
  }
  static Create_func_rpad s_singleton;

protected:
  Create_func_rpad() {}
  virtual ~Create_func_rpad() {}
  Item *create_native_std(THD *thd, LEX_CSTRING *name, List<Item> *items);
  Item *create_native_oracle(THD *thd, LEX_CSTRING *name, List<Item> *items);
};


class Create_func_rpad_oracle : public Create_func_rpad
{
public:
  Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list)
  {
    return create_native_oracle(thd, name, item_list);
  }
  static Create_func_rpad_oracle s_singleton;
};


class Create_func_rtrim : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_rtrim s_singleton;

protected:
  Create_func_rtrim() {}
  virtual ~Create_func_rtrim() {}
};


class Create_func_rtrim_oracle : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_rtrim_oracle s_singleton;

protected:
  Create_func_rtrim_oracle() {}
  virtual ~Create_func_rtrim_oracle() {}
};


class Create_func_sec_to_time : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_sec_to_time s_singleton;

protected:
  Create_func_sec_to_time() {}
  virtual ~Create_func_sec_to_time() {}
};


class Create_func_sha : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_sha s_singleton;

protected:
  Create_func_sha() {}
  virtual ~Create_func_sha() {}
};


class Create_func_sha2 : public Create_func_arg2
{
public:
  virtual Item* create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_sha2 s_singleton;

protected:
  Create_func_sha2() {}
  virtual ~Create_func_sha2() {}
};


class Create_func_sign : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_sign s_singleton;

protected:
  Create_func_sign() {}
  virtual ~Create_func_sign() {}
};


class Create_func_sin : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_sin s_singleton;

protected:
  Create_func_sin() {}
  virtual ~Create_func_sin() {}
};


class Create_func_sleep : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_sleep s_singleton;

protected:
  Create_func_sleep() {}
  virtual ~Create_func_sleep() {}
};


class Create_func_soundex : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_soundex s_singleton;

protected:
  Create_func_soundex() {}
  virtual ~Create_func_soundex() {}
};


class Create_func_space : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_space s_singleton;

protected:
  Create_func_space() {}
  virtual ~Create_func_space() {}
};


class Create_func_sqrt : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_sqrt s_singleton;

protected:
  Create_func_sqrt() {}
  virtual ~Create_func_sqrt() {}
};


class Create_func_str_to_date : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_str_to_date s_singleton;

protected:
  Create_func_str_to_date() {}
  virtual ~Create_func_str_to_date() {}
};


class Create_func_strcmp : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_strcmp s_singleton;

protected:
  Create_func_strcmp() {}
  virtual ~Create_func_strcmp() {}
};


class Create_func_substr_index : public Create_func_arg3
{
public:
  virtual Item *create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3);

  static Create_func_substr_index s_singleton;

protected:
  Create_func_substr_index() {}
  virtual ~Create_func_substr_index() {}
};


class Create_func_substr_oracle : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name,
                              List<Item> *item_list);

  static Create_func_substr_oracle s_singleton;

protected:
  Create_func_substr_oracle() {}
  virtual ~Create_func_substr_oracle() {}
};


class Create_func_subtime : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_subtime s_singleton;

protected:
  Create_func_subtime() {}
  virtual ~Create_func_subtime() {}
};


class Create_func_tan : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_tan s_singleton;

protected:
  Create_func_tan() {}
  virtual ~Create_func_tan() {}
};


class Create_func_time_format : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_time_format s_singleton;

protected:
  Create_func_time_format() {}
  virtual ~Create_func_time_format() {}
};


class Create_func_time_to_sec : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_time_to_sec s_singleton;

protected:
  Create_func_time_to_sec() {}
  virtual ~Create_func_time_to_sec() {}
};


class Create_func_timediff : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_timediff s_singleton;

protected:
  Create_func_timediff() {}
  virtual ~Create_func_timediff() {}
};


class Create_func_to_base64 : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_to_base64 s_singleton;

protected:
  Create_func_to_base64() {}
  virtual ~Create_func_to_base64() {}
};


class Create_func_to_days : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_to_days s_singleton;

protected:
  Create_func_to_days() {}
  virtual ~Create_func_to_days() {}
};

class Create_func_to_seconds : public Create_func_arg1
{
public:
  virtual Item* create_1_arg(THD *thd, Item *arg1);

  static Create_func_to_seconds s_singleton;

protected:
  Create_func_to_seconds() {}
  virtual ~Create_func_to_seconds() {}
};


class Create_func_ucase : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_ucase s_singleton;

protected:
  Create_func_ucase() {}
  virtual ~Create_func_ucase() {}
};


class Create_func_uncompress : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_uncompress s_singleton;

protected:
  Create_func_uncompress() {}
  virtual ~Create_func_uncompress() {}
};


class Create_func_uncompressed_length : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_uncompressed_length s_singleton;

protected:
  Create_func_uncompressed_length() {}
  virtual ~Create_func_uncompressed_length() {}
};


class Create_func_unhex : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_unhex s_singleton;

protected:
  Create_func_unhex() {}
  virtual ~Create_func_unhex() {}
};


class Create_func_unix_timestamp : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_unix_timestamp s_singleton;

protected:
  Create_func_unix_timestamp() {}
  virtual ~Create_func_unix_timestamp() {}
};


class Create_func_uuid : public Create_func_arg0
{
public:
  virtual Item *create_builder(THD *thd);

  static Create_func_uuid s_singleton;

protected:
  Create_func_uuid() {}
  virtual ~Create_func_uuid() {}
};


class Create_func_uuid_short : public Create_func_arg0
{
public:
  virtual Item *create_builder(THD *thd);

  static Create_func_uuid_short s_singleton;

protected:
  Create_func_uuid_short() {}
  virtual ~Create_func_uuid_short() {}
};


class Create_func_version : public Create_func_arg0
{
public:
  virtual Item *create_builder(THD *thd);

  static Create_func_version s_singleton;

protected:
  Create_func_version() {}
  virtual ~Create_func_version() {}
};


class Create_func_weekday : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_weekday s_singleton;

protected:
  Create_func_weekday() {}
  virtual ~Create_func_weekday() {}
};


class Create_func_weekofyear : public Create_func_arg1
{
public:
  virtual Item *create_1_arg(THD *thd, Item *arg1);

  static Create_func_weekofyear s_singleton;

protected:
  Create_func_weekofyear() {}
  virtual ~Create_func_weekofyear() {}
};


#ifdef WITH_WSREP
class Create_func_wsrep_last_written_gtid : public Create_func_arg0
{
public:
  virtual Item *create_builder(THD *thd);

  static Create_func_wsrep_last_written_gtid s_singleton;

protected:
  Create_func_wsrep_last_written_gtid() {}
  virtual ~Create_func_wsrep_last_written_gtid() {}
};


class Create_func_wsrep_last_seen_gtid : public Create_func_arg0
{
public:
  virtual Item *create_builder(THD *thd);

  static Create_func_wsrep_last_seen_gtid s_singleton;

protected:
  Create_func_wsrep_last_seen_gtid() {}
  virtual ~Create_func_wsrep_last_seen_gtid() {}
};


class Create_func_wsrep_sync_wait_upto : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_wsrep_sync_wait_upto s_singleton;

protected:
  Create_func_wsrep_sync_wait_upto() {}
  virtual ~Create_func_wsrep_sync_wait_upto() {}
};
#endif /* WITH_WSREP */


class Create_func_xml_extractvalue : public Create_func_arg2
{
public:
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2);

  static Create_func_xml_extractvalue s_singleton;

protected:
  Create_func_xml_extractvalue() {}
  virtual ~Create_func_xml_extractvalue() {}
};


class Create_func_xml_update : public Create_func_arg3
{
public:
  virtual Item *create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3);

  static Create_func_xml_update s_singleton;

protected:
  Create_func_xml_update() {}
  virtual ~Create_func_xml_update() {}
};


class Create_func_year_week : public Create_native_func
{
public:
  virtual Item *create_native(THD *thd, LEX_CSTRING *name, List<Item> *item_list);

  static Create_func_year_week s_singleton;

protected:
  Create_func_year_week() {}
  virtual ~Create_func_year_week() {}
};


/*
=============================================================================
  IMPLEMENTATION
=============================================================================
*/

/**
  Checks if there are named parameters in a parameter list.
  The syntax to name parameters in a function call is as follow:
  <code>foo(expr AS named, expr named, expr AS "named", expr "named")</code>
  @param params The parameter list, can be null
  @return true if one or more parameter is named
*/
static bool has_named_parameters(List<Item> *params)
{
  if (params)
  {
    Item *param;
    List_iterator<Item> it(*params);
    while ((param= it++))
    {
      if (! param->is_autogenerated_name)
        return true;
    }
  }

  return false;
}


Item*
Create_qfunc::create_func(THD *thd, LEX_CSTRING *name, List<Item> *item_list)
{
  LEX_CSTRING db;

  if (unlikely(! thd->db.str && ! thd->lex->sphead))
  {
    /*
      The proper error message should be in the lines of:
        Can't resolve <name>() to a function call,
        because this function:
        - is not a native function,
        - is not a user defined function,
        - can not match a qualified (read: stored) function
          since no database is selected.
      Reusing ER_SP_DOES_NOT_EXIST have a message consistent with
      the case when a default database exist, see Create_sp_func::create().
    */
    my_error(ER_SP_DOES_NOT_EXIST, MYF(0),
             "FUNCTION", name->str);
    return NULL;
  }

  if (thd->lex->copy_db_to(&db))
    return NULL;

  return create_with_db(thd, &db, name, false, item_list);
}


#ifdef HAVE_DLOPEN
Create_udf_func Create_udf_func::s_singleton;

Item*
Create_udf_func::create_func(THD *thd, LEX_CSTRING *name, List<Item> *item_list)
{
  udf_func *udf= find_udf(name->str,  name->length);
  DBUG_ASSERT(udf);
  return create(thd, udf, item_list);
}


Item*
Create_udf_func::create(THD *thd, udf_func *udf, List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  DBUG_ENTER("Create_udf_func::create");
  if (item_list != NULL)
    arg_count= item_list->elements;

  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_UDF);

  DBUG_ASSERT(   (udf->type == UDFTYPE_FUNCTION)
              || (udf->type == UDFTYPE_AGGREGATE));

  switch(udf->returns) {
  case STRING_RESULT:
  {
    if (udf->type == UDFTYPE_FUNCTION)
    {
      if (arg_count)
        func= new (thd->mem_root) Item_func_udf_str(thd, udf, *item_list);
      else
        func= new (thd->mem_root) Item_func_udf_str(thd, udf);
    }
    else
    {
      if (arg_count)
        func= new (thd->mem_root) Item_sum_udf_str(thd, udf, *item_list);
      else
        func= new (thd->mem_root) Item_sum_udf_str(thd, udf);
    }
    break;
  }
  case REAL_RESULT:
  {
    if (udf->type == UDFTYPE_FUNCTION)
    {
      if (arg_count)
        func= new (thd->mem_root) Item_func_udf_float(thd, udf, *item_list);
      else
        func= new (thd->mem_root) Item_func_udf_float(thd, udf);
    }
    else
    {
      if (arg_count)
        func= new (thd->mem_root) Item_sum_udf_float(thd, udf, *item_list);
      else
        func= new (thd->mem_root) Item_sum_udf_float(thd, udf);
    }
    break;
  }
  case INT_RESULT:
  {
    if (udf->type == UDFTYPE_FUNCTION)
    {
      if (arg_count)
        func= new (thd->mem_root) Item_func_udf_int(thd, udf, *item_list);
      else
        func= new (thd->mem_root) Item_func_udf_int(thd, udf);
    }
    else
    {
      if (arg_count)
        func= new (thd->mem_root) Item_sum_udf_int(thd, udf, *item_list);
      else
        func= new (thd->mem_root) Item_sum_udf_int(thd, udf);
    }
    break;
  }
  case DECIMAL_RESULT:
  {
    if (udf->type == UDFTYPE_FUNCTION)
    {
      if (arg_count)
        func= new (thd->mem_root) Item_func_udf_decimal(thd, udf, *item_list);
      else
        func= new (thd->mem_root) Item_func_udf_decimal(thd, udf);
    }
    else
    {
      if (arg_count)
        func= new (thd->mem_root) Item_sum_udf_decimal(thd, udf, *item_list);
      else
        func= new (thd->mem_root) Item_sum_udf_decimal(thd, udf);
    }
    break;
  }
  default:
  {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0), "UDF return type");
  }
  }
  thd->lex->safe_to_cache_query= 0;
  DBUG_RETURN(func);
}
#endif


Create_sp_func Create_sp_func::s_singleton;

Item*
Create_sp_func::create_with_db(THD *thd, LEX_CSTRING *db, LEX_CSTRING *name,
                               bool use_explicit_name, List<Item> *item_list)
{
  int arg_count= 0;
  Item *func= NULL;
  LEX *lex= thd->lex;
  sp_name *qname;
  const Sp_handler *sph= &sp_handler_function;
  Database_qualified_name pkgname(&null_clex_str, &null_clex_str);

  if (unlikely(has_named_parameters(item_list)))
  {
    /*
      The syntax "db.foo(expr AS p1, expr AS p2, ...) is invalid,
      and has been rejected during syntactic parsing already,
      because a stored function call may not have named parameters.

      The syntax "foo(expr AS p1, expr AS p2, ...)" is correct,
      because it can refer to a User Defined Function call.
      For a Stored Function however, this has no semantic.
    */
    my_error(ER_WRONG_PARAMETERS_TO_STORED_FCT, MYF(0), name->str);
    return NULL;
  }

  if (item_list != NULL)
    arg_count= item_list->elements;

  qname= new (thd->mem_root) sp_name(db, name, use_explicit_name);
  if (unlikely(sph->sp_resolve_package_routine(thd, thd->lex->sphead,
                                               qname, &sph, &pkgname)))
    return NULL;
  sph->add_used_routine(lex, thd, qname);
  if (pkgname.m_name.length)
    sp_handler_package_body.add_used_routine(lex, thd, &pkgname);
  Name_resolution_context *ctx= lex->current_context();
  if (arg_count > 0)
    func= new (thd->mem_root) Item_func_sp(thd, ctx, qname, sph, *item_list);
  else
    func= new (thd->mem_root) Item_func_sp(thd, ctx, qname, sph);

  lex->safe_to_cache_query= 0;
  return func;
}


Item*
Create_native_func::create_func(THD *thd, LEX_CSTRING *name, List<Item> *item_list)
{
  if (unlikely(has_named_parameters(item_list)))
  {
    my_error(ER_WRONG_PARAMETERS_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  return create_native(thd, name, item_list);
}


Item*
Create_func_arg0::create_func(THD *thd, LEX_CSTRING *name, List<Item> *item_list)
{
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count != 0))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  return create_builder(thd);
}


Item*
Create_func_arg1::create_func(THD *thd, LEX_CSTRING *name, List<Item> *item_list)
{
  int arg_count= 0;

  if (item_list)
    arg_count= item_list->elements;

  if (unlikely(arg_count != 1))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  Item *param_1= item_list->pop();

  if (unlikely(! param_1->is_autogenerated_name))
  {
    my_error(ER_WRONG_PARAMETERS_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  return create_1_arg(thd, param_1);
}


Item*
Create_func_arg2::create_func(THD *thd, LEX_CSTRING *name, List<Item> *item_list)
{
  int arg_count= 0;

  if (item_list)
    arg_count= item_list->elements;

  if (unlikely(arg_count != 2))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  Item *param_1= item_list->pop();
  Item *param_2= item_list->pop();

  if (unlikely(!param_1->is_autogenerated_name ||
               !param_2->is_autogenerated_name))
  {
    my_error(ER_WRONG_PARAMETERS_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  return create_2_arg(thd, param_1, param_2);
}


Item*
Create_func_arg3::create_func(THD *thd, LEX_CSTRING *name, List<Item> *item_list)
{
  int arg_count= 0;

  if (item_list)
    arg_count= item_list->elements;

  if (unlikely(arg_count != 3))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  Item *param_1= item_list->pop();
  Item *param_2= item_list->pop();
  Item *param_3= item_list->pop();

  if (unlikely(!param_1->is_autogenerated_name ||
               !param_2->is_autogenerated_name ||
               !param_3->is_autogenerated_name))
  {
    my_error(ER_WRONG_PARAMETERS_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  return create_3_arg(thd, param_1, param_2, param_3);
}


Create_func_abs Create_func_abs::s_singleton;

Item*
Create_func_abs::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_abs(thd, arg1);
}


Create_func_acos Create_func_acos::s_singleton;

Item*
Create_func_acos::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_acos(thd, arg1);
}


Create_func_addtime Create_func_addtime::s_singleton;

Item*
Create_func_addtime::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_add_time(thd, arg1, arg2, false);
}


Create_func_aes_encrypt Create_func_aes_encrypt::s_singleton;

Item*
Create_func_aes_encrypt::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_aes_encrypt(thd, arg1, arg2);
}


Create_func_aes_decrypt Create_func_aes_decrypt::s_singleton;

Item*
Create_func_aes_decrypt::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_aes_decrypt(thd, arg1, arg2);
}


Create_func_asin Create_func_asin::s_singleton;

Item*
Create_func_asin::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_asin(thd, arg1);
}


Create_func_atan Create_func_atan::s_singleton;

Item*
Create_func_atan::create_native(THD *thd, LEX_CSTRING *name,
                                List<Item> *item_list)
{
  Item* func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 1:
  {
    Item *param_1= item_list->pop();
    func= new (thd->mem_root) Item_func_atan(thd, param_1);
    break;
  }
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_atan(thd, param_1, param_2);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


Create_func_benchmark Create_func_benchmark::s_singleton;

Item*
Create_func_benchmark::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
  return new (thd->mem_root) Item_func_benchmark(thd, arg1, arg2);
}


Create_func_bin Create_func_bin::s_singleton;

Item*
Create_func_bin::create_1_arg(THD *thd, Item *arg1)
{
  Item *i10= new (thd->mem_root) Item_int(thd, (int32) 10,2);
  Item *i2= new (thd->mem_root) Item_int(thd, (int32) 2,1);
  return new (thd->mem_root) Item_func_conv(thd, arg1, i10, i2);
}


Create_func_binlog_gtid_pos Create_func_binlog_gtid_pos::s_singleton;

Item*
Create_func_binlog_gtid_pos::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
#ifdef HAVE_REPLICATION
  if (unlikely(!mysql_bin_log.is_open()))
#endif
  {
    my_error(ER_NO_BINARY_LOGGING, MYF(0));
    return NULL;
  }
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  return new (thd->mem_root) Item_func_binlog_gtid_pos(thd, arg1, arg2);
}


Create_func_bit_count Create_func_bit_count::s_singleton;

Item*
Create_func_bit_count::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_bit_count(thd, arg1);
}


Create_func_bit_length Create_func_bit_length::s_singleton;

Item*
Create_func_bit_length::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_bit_length(thd, arg1);
}


Create_func_ceiling Create_func_ceiling::s_singleton;

Item*
Create_func_ceiling::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_ceiling(thd, arg1);
}


Create_func_chr Create_func_chr::s_singleton;

Item*
Create_func_chr::create_1_arg(THD *thd, Item *arg1)
{
  CHARSET_INFO *cs_db= thd->variables.collation_database;
  return new (thd->mem_root) Item_func_chr(thd, arg1, cs_db);
}


Create_func_char_length Create_func_char_length::s_singleton;

Item*
Create_func_char_length::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_char_length(thd, arg1);
}


Create_func_coercibility Create_func_coercibility::s_singleton;

Item*
Create_func_coercibility::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_coercibility(thd, arg1);
}


Create_func_dyncol_check Create_func_dyncol_check::s_singleton;

Item*
Create_func_dyncol_check::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_dyncol_check(thd, arg1);
}

Create_func_dyncol_exists Create_func_dyncol_exists::s_singleton;

Item*
Create_func_dyncol_exists::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_dyncol_exists(thd, arg1, arg2);
}

Create_func_dyncol_list Create_func_dyncol_list::s_singleton;

Item*
Create_func_dyncol_list::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_dyncol_list(thd, arg1);
}

Create_func_dyncol_json Create_func_dyncol_json::s_singleton;

Item*
Create_func_dyncol_json::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_dyncol_json(thd, arg1);
}

Create_func_concat Create_func_concat::s_singleton;

Item*
Create_func_concat::create_native(THD *thd, LEX_CSTRING *name,
                                  List<Item> *item_list)
{
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 1))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  return thd->variables.sql_mode & MODE_ORACLE ?
    new (thd->mem_root) Item_func_concat_operator_oracle(thd, *item_list) :
    new (thd->mem_root) Item_func_concat(thd, *item_list);
}

Create_func_concat_operator_oracle
  Create_func_concat_operator_oracle::s_singleton;

Item*
Create_func_concat_operator_oracle::create_native(THD *thd, LEX_CSTRING *name,
                                                  List<Item> *item_list)
{
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 1))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  return new (thd->mem_root) Item_func_concat_operator_oracle(thd, *item_list);
}

Create_func_decode_histogram Create_func_decode_histogram::s_singleton;

Item *
Create_func_decode_histogram::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_decode_histogram(thd, arg1, arg2);
}

Create_func_decode_oracle Create_func_decode_oracle::s_singleton;

Item*
Create_func_decode_oracle::create_native(THD *thd, LEX_CSTRING *name,
                                         List<Item> *item_list)
{
  uint arg_count= item_list ? item_list->elements : 0;
  if (unlikely(arg_count < 3))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }
  return new (thd->mem_root) Item_func_decode_oracle(thd, *item_list);
}

Create_func_concat_ws Create_func_concat_ws::s_singleton;

Item*
Create_func_concat_ws::create_native(THD *thd, LEX_CSTRING *name,
                                     List<Item> *item_list)
{
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  /* "WS" stands for "With Separator": this function takes 2+ arguments */
  if (unlikely(arg_count < 2))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  return new (thd->mem_root) Item_func_concat_ws(thd, *item_list);
}


Create_func_compress Create_func_compress::s_singleton;

Item*
Create_func_compress::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_compress(thd, arg1);
}


Create_func_connection_id Create_func_connection_id::s_singleton;

Item*
Create_func_connection_id::create_builder(THD *thd)
{
  thd->lex->safe_to_cache_query= 0;
  return new (thd->mem_root) Item_func_connection_id(thd);
}


Create_func_nvl2 Create_func_nvl2::s_singleton;

Item*
Create_func_nvl2::create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3)
{
  return new (thd->mem_root) Item_func_nvl2(thd, arg1, arg2, arg3);
}


Create_func_conv Create_func_conv::s_singleton;

Item*
Create_func_conv::create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3)
{
  return new (thd->mem_root) Item_func_conv(thd, arg1, arg2, arg3);
}


Create_func_convert_tz Create_func_convert_tz::s_singleton;

Item*
Create_func_convert_tz::create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3)
{
  return new (thd->mem_root) Item_func_convert_tz(thd, arg1, arg2, arg3);
}


Create_func_cos Create_func_cos::s_singleton;

Item*
Create_func_cos::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_cos(thd, arg1);
}


Create_func_cot Create_func_cot::s_singleton;

Item*
Create_func_cot::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_cot(thd, arg1);
}


Create_func_crc32 Create_func_crc32::s_singleton;

Item*
Create_func_crc32::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_crc32(thd, arg1);
}

Create_func_datediff Create_func_datediff::s_singleton;

Item*
Create_func_datediff::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  Item *i1= new (thd->mem_root) Item_func_to_days(thd, arg1);
  Item *i2= new (thd->mem_root) Item_func_to_days(thd, arg2);

  return new (thd->mem_root) Item_func_minus(thd, i1, i2);
}


Create_func_dayname Create_func_dayname::s_singleton;

Item*
Create_func_dayname::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_dayname(thd, arg1);
}


Create_func_dayofmonth Create_func_dayofmonth::s_singleton;

Item*
Create_func_dayofmonth::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_dayofmonth(thd, arg1);
}


Create_func_dayofweek Create_func_dayofweek::s_singleton;

Item*
Create_func_dayofweek::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_weekday(thd, arg1, 1);
}


Create_func_dayofyear Create_func_dayofyear::s_singleton;

Item*
Create_func_dayofyear::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_dayofyear(thd, arg1);
}


Create_func_degrees Create_func_degrees::s_singleton;

Item*
Create_func_degrees::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_units(thd, (char*) "degrees", arg1,
                                             180/M_PI, 0.0);
}


Create_func_des_decrypt Create_func_des_decrypt::s_singleton;

Item*
Create_func_des_decrypt::create_native(THD *thd, LEX_CSTRING *name,
                                       List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 1:
  {
    Item *param_1= item_list->pop();
    func= new (thd->mem_root) Item_func_des_decrypt(thd, param_1);
    break;
  }
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_des_decrypt(thd, param_1, param_2);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


Create_func_des_encrypt Create_func_des_encrypt::s_singleton;

Item*
Create_func_des_encrypt::create_native(THD *thd, LEX_CSTRING *name,
                                       List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 1:
  {
    Item *param_1= item_list->pop();
    func= new (thd->mem_root) Item_func_des_encrypt(thd, param_1);
    break;
  }
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_des_encrypt(thd, param_1, param_2);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


Create_func_elt Create_func_elt::s_singleton;

Item*
Create_func_elt::create_native(THD *thd, LEX_CSTRING *name,
                               List<Item> *item_list)
{
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 2))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  return new (thd->mem_root) Item_func_elt(thd, *item_list);
}


Create_func_encode Create_func_encode::s_singleton;

Item*
Create_func_encode::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_encode(thd, arg1, arg2);
}


Create_func_encrypt Create_func_encrypt::s_singleton;

Item*
Create_func_encrypt::create_native(THD *thd, LEX_CSTRING *name,
                                   List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 1:
  {
    Item *param_1= item_list->pop();
    func= new (thd->mem_root) Item_func_encrypt(thd, param_1);
    thd->lex->uncacheable(UNCACHEABLE_RAND);
    break;
  }
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_encrypt(thd, param_1, param_2);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


Create_func_exp Create_func_exp::s_singleton;

Item*
Create_func_exp::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_exp(thd, arg1);
}


Create_func_export_set Create_func_export_set::s_singleton;

Item*
Create_func_export_set::create_native(THD *thd, LEX_CSTRING *name,
                                      List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 3:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    Item *param_3= item_list->pop();
    func= new (thd->mem_root) Item_func_export_set(thd, param_1, param_2, param_3);
    break;
  }
  case 4:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    Item *param_3= item_list->pop();
    Item *param_4= item_list->pop();
    func= new (thd->mem_root) Item_func_export_set(thd, param_1, param_2, param_3,
                                                   param_4);
    break;
  }
  case 5:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    Item *param_3= item_list->pop();
    Item *param_4= item_list->pop();
    Item *param_5= item_list->pop();
    func= new (thd->mem_root) Item_func_export_set(thd, param_1, param_2, param_3,
                                                   param_4, param_5);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


Create_func_field Create_func_field::s_singleton;

Item*
Create_func_field::create_native(THD *thd, LEX_CSTRING *name,
                                 List<Item> *item_list)
{
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 2))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  return new (thd->mem_root) Item_func_field(thd, *item_list);
}


Create_func_find_in_set Create_func_find_in_set::s_singleton;

Item*
Create_func_find_in_set::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_find_in_set(thd, arg1, arg2);
}


Create_func_floor Create_func_floor::s_singleton;

Item*
Create_func_floor::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_floor(thd, arg1);
}


Create_func_format Create_func_format::s_singleton;

Item*
Create_func_format::create_native(THD *thd, LEX_CSTRING *name,
                                  List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= item_list ? item_list->elements : 0;

  switch (arg_count) {
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_format(thd, param_1, param_2);
    break;
  }
  case 3:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    Item *param_3= item_list->pop();
    func= new (thd->mem_root) Item_func_format(thd, param_1, param_2, param_3);
    break;
  }
  default:
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }

  return func;
}


Create_func_from_base64 Create_func_from_base64::s_singleton;


Item *
Create_func_from_base64::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_from_base64(thd, arg1);
}


Create_func_found_rows Create_func_found_rows::s_singleton;

Item*
Create_func_found_rows::create_builder(THD *thd)
{
  DBUG_ENTER("Create_func_found_rows::create");
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  thd->lex->safe_to_cache_query= 0;
  DBUG_RETURN(new (thd->mem_root) Item_func_found_rows(thd));
}


Create_func_from_days Create_func_from_days::s_singleton;

Item*
Create_func_from_days::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_from_days(thd, arg1);
}


Create_func_from_unixtime Create_func_from_unixtime::s_singleton;

Item*
Create_func_from_unixtime::create_native(THD *thd, LEX_CSTRING *name,
                                         List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 1:
  {
    Item *param_1= item_list->pop();
    func= new (thd->mem_root) Item_func_from_unixtime(thd, param_1);
    break;
  }
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    Item *ut= new (thd->mem_root) Item_func_from_unixtime(thd, param_1);
    func= new (thd->mem_root) Item_func_date_format(thd, ut, param_2);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}



Create_func_get_lock Create_func_get_lock::s_singleton;

Item*
Create_func_get_lock::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
  return new (thd->mem_root) Item_func_get_lock(thd, arg1, arg2);
}


Create_func_greatest Create_func_greatest::s_singleton;

Item*
Create_func_greatest::create_native(THD *thd, LEX_CSTRING *name,
                                    List<Item> *item_list)
{
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 2))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  return new (thd->mem_root) Item_func_max(thd, *item_list);
}


Create_func_hex Create_func_hex::s_singleton;

Item*
Create_func_hex::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_hex(thd, arg1);
}


Create_func_ifnull Create_func_ifnull::s_singleton;

Item*
Create_func_ifnull::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_ifnull(thd, arg1, arg2);
}


Create_func_instr Create_func_instr::s_singleton;

Item*
Create_func_instr::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_locate(thd, arg1, arg2);
}


Create_func_is_free_lock Create_func_is_free_lock::s_singleton;

Item*
Create_func_is_free_lock::create_1_arg(THD *thd, Item *arg1)
{
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
  return new (thd->mem_root) Item_func_is_free_lock(thd, arg1);
}


Create_func_is_used_lock Create_func_is_used_lock::s_singleton;

Item*
Create_func_is_used_lock::create_1_arg(THD *thd, Item *arg1)
{
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
  return new (thd->mem_root) Item_func_is_used_lock(thd, arg1);
}


Create_func_isnull Create_func_isnull::s_singleton;

Item*
Create_func_isnull::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_isnull(thd, arg1);
}


Create_func_json_exists Create_func_json_exists::s_singleton;

Item*
Create_func_json_exists::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  status_var_increment(current_thd->status_var.feature_json);
  return new (thd->mem_root) Item_func_json_exists(thd, arg1, arg2);
}


Create_func_json_detailed Create_func_json_detailed::s_singleton;

Item*
Create_func_json_detailed::create_native(THD *thd, LEX_CSTRING *name,
                                     List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 1 || arg_count > 2 /* json_doc, [path]...*/))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_format(thd, *item_list);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_loose Create_func_json_loose::s_singleton;

Item*
Create_func_json_loose::create_1_arg(THD *thd, Item *arg1)
{
  status_var_increment(current_thd->status_var.feature_json);
  return new (thd->mem_root) Item_func_json_format(thd, arg1,
               Item_func_json_format::LOOSE);
}


Create_func_json_compact Create_func_json_compact::s_singleton;

Item*
Create_func_json_compact::create_1_arg(THD *thd, Item *arg1)
{
  status_var_increment(current_thd->status_var.feature_json);
  return new (thd->mem_root) Item_func_json_format(thd, arg1,
               Item_func_json_format::COMPACT);
}


Create_func_json_valid Create_func_json_valid::s_singleton;

Item*
Create_func_json_valid::create_1_arg(THD *thd, Item *arg1)
{
  status_var_increment(current_thd->status_var.feature_json);
  return new (thd->mem_root) Item_func_json_valid(thd, arg1);
}


Create_func_json_type Create_func_json_type::s_singleton;

Item*
Create_func_json_type::create_1_arg(THD *thd, Item *arg1)
{
  status_var_increment(current_thd->status_var.feature_json);
  return new (thd->mem_root) Item_func_json_type(thd, arg1);
}


Create_func_json_depth Create_func_json_depth::s_singleton;

Item*
Create_func_json_depth::create_1_arg(THD *thd, Item *arg1)
{
  status_var_increment(current_thd->status_var.feature_json);
  return new (thd->mem_root) Item_func_json_depth(thd, arg1);
}


Create_func_json_value Create_func_json_value::s_singleton;

Item*
Create_func_json_value::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  status_var_increment(current_thd->status_var.feature_json);
  return new (thd->mem_root) Item_func_json_value(thd, arg1, arg2);
}


Create_func_json_query Create_func_json_query::s_singleton;

Item*
Create_func_json_query::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  status_var_increment(current_thd->status_var.feature_json);
  return new (thd->mem_root) Item_func_json_query(thd, arg1, arg2);
}


Create_func_json_quote Create_func_json_quote::s_singleton;

Item*
Create_func_json_quote::create_1_arg(THD *thd, Item *arg1)
{
  status_var_increment(current_thd->status_var.feature_json);
  return new (thd->mem_root) Item_func_json_quote(thd, arg1);
}


Create_func_json_unquote Create_func_json_unquote::s_singleton;

Item*
Create_func_json_unquote::create_1_arg(THD *thd, Item *arg1)
{
  status_var_increment(current_thd->status_var.feature_json);
  return new (thd->mem_root) Item_func_json_unquote(thd, arg1);
}


Create_func_last_day Create_func_last_day::s_singleton;

Item*
Create_func_last_day::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_last_day(thd, arg1);
}


Create_func_json_array Create_func_json_array::s_singleton;

Item*
Create_func_json_array::create_native(THD *thd, LEX_CSTRING *name,
                                      List<Item> *item_list)
{
  Item *func;

  if (item_list != NULL)
  {
    func= new (thd->mem_root) Item_func_json_array(thd, *item_list);
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_array(thd);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_array_append Create_func_json_array_append::s_singleton;

Item*
Create_func_json_array_append::create_native(THD *thd, LEX_CSTRING *name,
                                                 List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 3 || (arg_count & 1) == 0 /*is even*/))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_array_append(thd, *item_list);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_array_insert Create_func_json_array_insert::s_singleton;

Item*
Create_func_json_array_insert::create_native(THD *thd, LEX_CSTRING *name,
                                                 List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 3 || (arg_count & 1) == 0 /*is even*/))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_array_insert(thd, *item_list);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_insert Create_func_json_insert::s_singleton;

Item*
Create_func_json_insert::create_native(THD *thd, LEX_CSTRING *name,
                                                 List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 3 || (arg_count & 1) == 0 /*is even*/))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_insert(true, false,
                                                    thd, *item_list);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_set Create_func_json_set::s_singleton;

Item*
Create_func_json_set::create_native(THD *thd, LEX_CSTRING *name,
                                    List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 3 || (arg_count & 1) == 0 /*is even*/))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_insert(true, true,
                                                    thd, *item_list);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_replace Create_func_json_replace::s_singleton;

Item*
Create_func_json_replace::create_native(THD *thd, LEX_CSTRING *name,
                                        List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 3 || (arg_count & 1) == 0 /*is even*/))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_insert(false, true,
                                                    thd, *item_list);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_remove Create_func_json_remove::s_singleton;

Item*
Create_func_json_remove::create_native(THD *thd, LEX_CSTRING *name,
                                       List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 2 /*json_doc, path [,path]*/))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_remove(thd, *item_list);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_object Create_func_json_object::s_singleton;

Item*
Create_func_json_object::create_native(THD *thd, LEX_CSTRING *name,
                                       List<Item> *item_list)
{
  Item *func;
  int arg_count;

  if (item_list != NULL)
  {
    arg_count= item_list->elements;
    if (unlikely((arg_count & 1) != 0 /*is odd*/))
    {
      my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
      func= NULL;
    }
    else
    {
      func= new (thd->mem_root) Item_func_json_object(thd, *item_list);
    }
  }
  else
  {
    arg_count= 0;
    func= new (thd->mem_root) Item_func_json_object(thd);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_length Create_func_json_length::s_singleton;

Item*
Create_func_json_length::create_native(THD *thd, LEX_CSTRING *name,
                                       List<Item> *item_list)
{
  Item *func;
  int arg_count;

  if (unlikely(item_list == NULL ||
               (arg_count= item_list->elements) == 0))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    func= NULL;
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_length(thd, *item_list);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_merge Create_func_json_merge::s_singleton;

Item*
Create_func_json_merge::create_native(THD *thd, LEX_CSTRING *name,
                                      List<Item> *item_list)
{
  Item *func;
  int arg_count;

  if (unlikely(item_list == NULL ||
               (arg_count= item_list->elements) < 2)) // json, json
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    func= NULL;
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_merge(thd, *item_list);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_merge_patch Create_func_json_merge_patch::s_singleton;

Item*
Create_func_json_merge_patch::create_native(THD *thd, LEX_CSTRING *name,
                                           List<Item> *item_list)
{
  Item *func;
  int arg_count;

  if (item_list == NULL ||
      (arg_count= item_list->elements) < 2) // json, json
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    func= NULL;
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_merge_patch(thd, *item_list);
  }

  return func;
}


Create_func_json_contains Create_func_json_contains::s_singleton;

Item*
Create_func_json_contains::create_native(THD *thd, LEX_CSTRING *name,
                                         List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count == 2 || arg_count == 3/* json_doc, val, [path] */))
  {
    func= new (thd->mem_root) Item_func_json_contains(thd, *item_list);
  }
  else
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_keys Create_func_json_keys::s_singleton;

Item*
Create_func_json_keys::create_native(THD *thd, LEX_CSTRING *name,
                                     List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 1 || arg_count > 2 /* json_doc, [path]...*/))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_keys(thd, *item_list);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_contains_path Create_func_json_contains_path::s_singleton;

Item*
Create_func_json_contains_path::create_native(THD *thd, LEX_CSTRING *name,
                                                 List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 3 /* json_doc, one_or_all, path, [path]...*/))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_contains_path(thd, *item_list);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_extract Create_func_json_extract::s_singleton;

Item*
Create_func_json_extract::create_native(THD *thd, LEX_CSTRING *name,
                                                 List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 2 /* json_doc, path, [path]...*/))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_extract(thd, *item_list);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_json_search Create_func_json_search::s_singleton;

Item*
Create_func_json_search::create_native(THD *thd, LEX_CSTRING *name,
                                       List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 3 /* json_doc, one_or_all, search_str, [escape_char[, path]...*/))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
  }
  else
  {
    func= new (thd->mem_root) Item_func_json_search(thd, *item_list);
  }

  status_var_increment(current_thd->status_var.feature_json);
  return func;
}


Create_func_last_insert_id Create_func_last_insert_id::s_singleton;

Item*
Create_func_last_insert_id::create_native(THD *thd, LEX_CSTRING *name,
                                          List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 0:
  {
    func= new (thd->mem_root) Item_func_last_insert_id(thd);
    thd->lex->safe_to_cache_query= 0;
    break;
  }
  case 1:
  {
    Item *param_1= item_list->pop();
    func= new (thd->mem_root) Item_func_last_insert_id(thd, param_1);
    thd->lex->safe_to_cache_query= 0;
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


Create_func_lcase Create_func_lcase::s_singleton;

Item*
Create_func_lcase::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_lcase(thd, arg1);
}


Create_func_least Create_func_least::s_singleton;

Item*
Create_func_least::create_native(THD *thd, LEX_CSTRING *name,
                                 List<Item> *item_list)
{
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 2))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  return new (thd->mem_root) Item_func_min(thd, *item_list);
}


Create_func_length Create_func_length::s_singleton;

Item*
Create_func_length::create_1_arg(THD *thd, Item *arg1)
{
  if (thd->variables.sql_mode & MODE_ORACLE)
    return new (thd->mem_root) Item_func_char_length(thd, arg1);
  else
    return new (thd->mem_root) Item_func_octet_length(thd, arg1);
}

Create_func_octet_length Create_func_octet_length::s_singleton;

Item*
Create_func_octet_length::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_octet_length(thd, arg1);
}


#ifndef DBUG_OFF
Create_func_like_range_min Create_func_like_range_min::s_singleton;

Item*
Create_func_like_range_min::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_like_range_min(thd, arg1, arg2);
}


Create_func_like_range_max Create_func_like_range_max::s_singleton;

Item*
Create_func_like_range_max::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_like_range_max(thd, arg1, arg2);
}
#endif


Create_func_ln Create_func_ln::s_singleton;

Item*
Create_func_ln::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_ln(thd, arg1);
}


Create_func_load_file Create_func_load_file::s_singleton;

Item*
Create_func_load_file::create_1_arg(THD *thd, Item *arg1)
{
  DBUG_ENTER("Create_func_load_file::create");
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
  DBUG_RETURN(new (thd->mem_root) Item_load_file(thd, arg1));
}


Create_func_locate Create_func_locate::s_singleton;

Item*
Create_func_locate::create_native(THD *thd, LEX_CSTRING *name,
                                  List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    /* Yes, parameters in that order : 2, 1 */
    func= new (thd->mem_root) Item_func_locate(thd, param_2, param_1);
    break;
  }
  case 3:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    Item *param_3= item_list->pop();
    /* Yes, parameters in that order : 2, 1, 3 */
    func= new (thd->mem_root) Item_func_locate(thd, param_2, param_1, param_3);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


Create_func_log Create_func_log::s_singleton;

Item*
Create_func_log::create_native(THD *thd, LEX_CSTRING *name,
                               List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 1:
  {
    Item *param_1= item_list->pop();
    func= new (thd->mem_root) Item_func_log(thd, param_1);
    break;
  }
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_log(thd, param_1, param_2);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


Create_func_log10 Create_func_log10::s_singleton;

Item*
Create_func_log10::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_log10(thd, arg1);
}


Create_func_log2 Create_func_log2::s_singleton;

Item*
Create_func_log2::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_log2(thd, arg1);
}


Create_func_lpad Create_func_lpad::s_singleton;

Create_func_lpad_oracle Create_func_lpad_oracle::s_singleton;

Item*
Create_func_lpad::create_native_std(THD *thd, LEX_CSTRING *name,
                                    List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= item_list ? item_list->elements : 0;

  switch (arg_count) {
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_lpad(thd, param_1, param_2);
    break;
  }
  case 3:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    Item *param_3= item_list->pop();
    func= new (thd->mem_root) Item_func_lpad(thd, param_1, param_2, param_3);
    break;
  }
  default:
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }

  return func;
}


Item*
Create_func_lpad::create_native_oracle(THD *thd, LEX_CSTRING *name,
                                       List<Item> *item_list)
{
  int arg_count= item_list ? item_list->elements : 0;
  switch (arg_count) {
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    return new (thd->mem_root) Item_func_lpad_oracle(thd, param_1, param_2);
  }
  case 3:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    Item *param_3= item_list->pop();
    return new (thd->mem_root) Item_func_lpad_oracle(thd, param_1,
                                                     param_2, param_3);
  }
  default:
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  return NULL;
}


Create_func_ltrim Create_func_ltrim::s_singleton;

Item*
Create_func_ltrim::create_1_arg(THD *thd, Item *arg1)
{
  return Lex_trim(TRIM_LEADING, arg1).make_item_func_trim(thd);
}


Create_func_ltrim_oracle Create_func_ltrim_oracle::s_singleton;

Item*
Create_func_ltrim_oracle::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_ltrim_oracle(thd, arg1);
}


Create_func_makedate Create_func_makedate::s_singleton;

Item*
Create_func_makedate::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_makedate(thd, arg1, arg2);
}


Create_func_maketime Create_func_maketime::s_singleton;

Item*
Create_func_maketime::create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3)
{
  return new (thd->mem_root) Item_func_maketime(thd, arg1, arg2, arg3);
}


Create_func_make_set Create_func_make_set::s_singleton;

Item*
Create_func_make_set::create_native(THD *thd, LEX_CSTRING *name,
                                    List<Item> *item_list)
{
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 2))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }

  return new (thd->mem_root) Item_func_make_set(thd, *item_list);
}


Create_func_master_pos_wait Create_func_master_pos_wait::s_singleton;

Item*
Create_func_master_pos_wait::create_native(THD *thd, LEX_CSTRING *name,
                                           List<Item> *item_list)

{
  Item *func= NULL;
  int arg_count= 0;

  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 2 || arg_count > 4))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return func;
  }

  thd->lex->safe_to_cache_query= 0;

  Item *param_1= item_list->pop();
  Item *param_2= item_list->pop();
  switch (arg_count) {
  case 2:
  {
    func= new (thd->mem_root) Item_master_pos_wait(thd, param_1, param_2);
    break;
  }
  case 3:
  {
    Item *param_3= item_list->pop();
    func= new (thd->mem_root) Item_master_pos_wait(thd, param_1, param_2, param_3);
    break;
  }
  case 4:
  {
    Item *param_3= item_list->pop();
    Item *param_4= item_list->pop();
    func= new (thd->mem_root) Item_master_pos_wait(thd, param_1, param_2, param_3,
                                                   param_4);
    break;
  }
  }

  return func;
}


Create_func_master_gtid_wait Create_func_master_gtid_wait::s_singleton;

Item*
Create_func_master_gtid_wait::create_native(THD *thd, LEX_CSTRING *name,
                                            List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (unlikely(arg_count < 1 || arg_count > 2))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return func;
  }

  thd->lex->safe_to_cache_query= 0;

  Item *param_1= item_list->pop();
  switch (arg_count) {
  case 1:
  {
    func= new (thd->mem_root) Item_master_gtid_wait(thd, param_1);
    break;
  }
  case 2:
  {
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_master_gtid_wait(thd, param_1, param_2);
    break;
  }
  }

  return func;
}


Create_func_md5 Create_func_md5::s_singleton;

Item*
Create_func_md5::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_md5(thd, arg1);
}


Create_func_monthname Create_func_monthname::s_singleton;

Item*
Create_func_monthname::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_monthname(thd, arg1);
}


Create_func_name_const Create_func_name_const::s_singleton;

Item*
Create_func_name_const::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  if (!arg1->basic_const_item())
    goto err;

  if (arg2->basic_const_item())
    return new (thd->mem_root) Item_name_const(thd, arg1, arg2);

  if (arg2->type() == Item::FUNC_ITEM)
  {
    Item_func *value_func= (Item_func *) arg2;
    if (value_func->functype() != Item_func::COLLATE_FUNC &&
        value_func->functype() != Item_func::NEG_FUNC)
      goto err;

    if (!value_func->key_item()->basic_const_item())
      goto err;
    return new (thd->mem_root) Item_name_const(thd, arg1, arg2);
  }
err:
  my_error(ER_WRONG_ARGUMENTS, MYF(0), "NAME_CONST");
  return NULL;
}


Create_func_nullif Create_func_nullif::s_singleton;

Item*
Create_func_nullif::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_nullif(thd, arg1, arg2);
}


Create_func_oct Create_func_oct::s_singleton;

Item*
Create_func_oct::create_1_arg(THD *thd, Item *arg1)
{
  Item *i10= new (thd->mem_root) Item_int(thd, (int32) 10,2);
  Item *i8= new (thd->mem_root) Item_int(thd, (int32) 8,1);
  return new (thd->mem_root) Item_func_conv(thd, arg1, i10, i8);
}


Create_func_ord Create_func_ord::s_singleton;

Item*
Create_func_ord::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_ord(thd, arg1);
}


Create_func_period_add Create_func_period_add::s_singleton;

Item*
Create_func_period_add::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_period_add(thd, arg1, arg2);
}


Create_func_period_diff Create_func_period_diff::s_singleton;

Item*
Create_func_period_diff::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_period_diff(thd, arg1, arg2);
}


Create_func_pi Create_func_pi::s_singleton;

Item*
Create_func_pi::create_builder(THD *thd)
{
  return new (thd->mem_root) Item_static_float_func(thd, "pi()", M_PI, 6, 8);
}


Create_func_pow Create_func_pow::s_singleton;

Item*
Create_func_pow::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_pow(thd, arg1, arg2);
}


Create_func_quote Create_func_quote::s_singleton;

Item*
Create_func_quote::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_quote(thd, arg1);
}


Create_func_regexp_instr Create_func_regexp_instr::s_singleton;

Item*
Create_func_regexp_instr::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_regexp_instr(thd, arg1, arg2);
}


Create_func_regexp_replace Create_func_regexp_replace::s_singleton;

Item*
Create_func_regexp_replace::create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3)
{
  return new (thd->mem_root) Item_func_regexp_replace(thd, arg1, arg2, arg3);
}


Create_func_regexp_substr Create_func_regexp_substr::s_singleton;

Item*
Create_func_regexp_substr::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_regexp_substr(thd, arg1, arg2);
}


Create_func_radians Create_func_radians::s_singleton;

Item*
Create_func_radians::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_units(thd, (char*) "radians", arg1,
                                             M_PI/180, 0.0);
}


Create_func_rand Create_func_rand::s_singleton;

Item*
Create_func_rand::create_native(THD *thd, LEX_CSTRING *name,
                                List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  /*
    When RAND() is binlogged, the seed is binlogged too.  So the
    sequence of random numbers is the same on a replication slave as
    on the master.  However, if several RAND() values are inserted
    into a table, the order in which the rows are modified may differ
    between master and slave, because the order is undefined.  Hence,
    the statement is unsafe to log in statement format.

    For normal INSERT's this is howevever safe
  */
  if (thd->lex->sql_command != SQLCOM_INSERT)
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);

  switch (arg_count) {
  case 0:
  {
    func= new (thd->mem_root) Item_func_rand(thd);
    thd->lex->uncacheable(UNCACHEABLE_RAND);
    break;
  }
  case 1:
  {
    Item *param_1= item_list->pop();
    func= new (thd->mem_root) Item_func_rand(thd, param_1);
    thd->lex->uncacheable(UNCACHEABLE_RAND);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


Create_func_release_all_locks Create_func_release_all_locks::s_singleton;

Item*
Create_func_release_all_locks::create_builder(THD *thd)
{
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
  return new (thd->mem_root) Item_func_release_all_locks(thd);
}


Create_func_release_lock Create_func_release_lock::s_singleton;

Item*
Create_func_release_lock::create_1_arg(THD *thd, Item *arg1)
{
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
  return new (thd->mem_root) Item_func_release_lock(thd, arg1);
}


Create_func_replace_oracle Create_func_replace_oracle::s_singleton;

Item*
Create_func_replace_oracle::create_3_arg(THD *thd, Item *arg1, Item *arg2,
                                        Item *arg3)
{
  return new (thd->mem_root) Item_func_replace_oracle(thd, arg1, arg2, arg3);
}


Create_func_reverse Create_func_reverse::s_singleton;

Item*
Create_func_reverse::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_reverse(thd, arg1);
}


Create_func_round Create_func_round::s_singleton;

Item*
Create_func_round::create_native(THD *thd, LEX_CSTRING *name,
                                 List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 1:
  {
    Item *param_1= item_list->pop();
    Item *i0= new (thd->mem_root) Item_int(thd, (char*)"0", 0, 1);
    func= new (thd->mem_root) Item_func_round(thd, param_1, i0, 0);
    break;
  }
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_round(thd, param_1, param_2, 0);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


Create_func_rpad Create_func_rpad::s_singleton;

Create_func_rpad_oracle Create_func_rpad_oracle::s_singleton;

Item*
Create_func_rpad::create_native_std(THD *thd, LEX_CSTRING *name,
                                    List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= item_list ? item_list->elements : 0;

  switch (arg_count) {
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_rpad(thd, param_1, param_2);
    break;
  }
  case 3:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    Item *param_3= item_list->pop();
    func= new (thd->mem_root) Item_func_rpad(thd, param_1, param_2, param_3);
    break;
  }
  default:
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }

  return func;
}


Item*
Create_func_rpad::create_native_oracle(THD *thd, LEX_CSTRING *name,
                                       List<Item> *item_list)
{
  int arg_count= item_list ? item_list->elements : 0;
  switch (arg_count) {
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    return new (thd->mem_root) Item_func_rpad_oracle(thd, param_1, param_2);
  }
  case 3:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    Item *param_3= item_list->pop();
    return new (thd->mem_root) Item_func_rpad_oracle(thd, param_1,
                                                     param_2, param_3);
  }
  default:
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  return NULL;
}


Create_func_rtrim Create_func_rtrim::s_singleton;

Item*
Create_func_rtrim::create_1_arg(THD *thd, Item *arg1)
{
  return Lex_trim(TRIM_TRAILING, arg1).make_item_func_trim(thd);
}


Create_func_rtrim_oracle Create_func_rtrim_oracle::s_singleton;

Item*
Create_func_rtrim_oracle::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_rtrim_oracle(thd, arg1);
}


Create_func_sec_to_time Create_func_sec_to_time::s_singleton;

Item*
Create_func_sec_to_time::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_sec_to_time(thd, arg1);
}


Create_func_sha Create_func_sha::s_singleton;

Item*
Create_func_sha::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_sha(thd, arg1);
}


Create_func_sha2 Create_func_sha2::s_singleton;

Item*
Create_func_sha2::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_sha2(thd, arg1, arg2);
}


Create_func_sign Create_func_sign::s_singleton;

Item*
Create_func_sign::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_sign(thd, arg1);
}


Create_func_sin Create_func_sin::s_singleton;

Item*
Create_func_sin::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_sin(thd, arg1);
}


Create_func_sleep Create_func_sleep::s_singleton;

Item*
Create_func_sleep::create_1_arg(THD *thd, Item *arg1)
{
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
  return new (thd->mem_root) Item_func_sleep(thd, arg1);
}


Create_func_soundex Create_func_soundex::s_singleton;

Item*
Create_func_soundex::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_soundex(thd, arg1);
}


Create_func_space Create_func_space::s_singleton;

Item*
Create_func_space::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_space(thd, arg1);
}


Create_func_sqrt Create_func_sqrt::s_singleton;

Item*
Create_func_sqrt::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_sqrt(thd, arg1);
}


Create_func_str_to_date Create_func_str_to_date::s_singleton;

Item*
Create_func_str_to_date::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_str_to_date(thd, arg1, arg2);
}


Create_func_strcmp Create_func_strcmp::s_singleton;

Item*
Create_func_strcmp::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_strcmp(thd, arg1, arg2);
}


Create_func_substr_index Create_func_substr_index::s_singleton;

Item*
Create_func_substr_index::create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3)
{
  return new (thd->mem_root) Item_func_substr_index(thd, arg1, arg2, arg3);
}


Create_func_substr_oracle Create_func_substr_oracle::s_singleton;

Item*
Create_func_substr_oracle::create_native(THD *thd, LEX_CSTRING *name,
                                List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= item_list ? item_list->elements : 0;

  switch (arg_count) {
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_substr_oracle(thd, param_1, param_2);
    break;
  }
  case 3:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    Item *param_3= item_list->pop();
    func= new (thd->mem_root) Item_func_substr_oracle(thd, param_1, param_2, param_3);
    break;
  }
  default:
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }

  return func;
}


Create_func_subtime Create_func_subtime::s_singleton;

Item*
Create_func_subtime::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_add_time(thd, arg1, arg2, true);
}


Create_func_tan Create_func_tan::s_singleton;

Item*
Create_func_tan::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_tan(thd, arg1);
}


Create_func_time_format Create_func_time_format::s_singleton;

Item*
Create_func_time_format::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_time_format(thd, arg1, arg2);
}


Create_func_time_to_sec Create_func_time_to_sec::s_singleton;

Item*
Create_func_time_to_sec::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_time_to_sec(thd, arg1);
}


Create_func_timediff Create_func_timediff::s_singleton;

Item*
Create_func_timediff::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_timediff(thd, arg1, arg2);
}


Create_func_to_base64 Create_func_to_base64::s_singleton;

Item*
Create_func_to_base64::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_to_base64(thd, arg1);
}


Create_func_to_days Create_func_to_days::s_singleton;

Item*
Create_func_to_days::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_to_days(thd, arg1);
}


Create_func_to_seconds Create_func_to_seconds::s_singleton;

Item*
Create_func_to_seconds::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_to_seconds(thd, arg1);
}


Create_func_ucase Create_func_ucase::s_singleton;

Item*
Create_func_ucase::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_ucase(thd, arg1);
}


Create_func_uncompress Create_func_uncompress::s_singleton;

Item*
Create_func_uncompress::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_uncompress(thd, arg1);
}


Create_func_uncompressed_length Create_func_uncompressed_length::s_singleton;

Item*
Create_func_uncompressed_length::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_uncompressed_length(thd, arg1);
}


Create_func_unhex Create_func_unhex::s_singleton;

Item*
Create_func_unhex::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_unhex(thd, arg1);
}


Create_func_unix_timestamp Create_func_unix_timestamp::s_singleton;

Item*
Create_func_unix_timestamp::create_native(THD *thd, LEX_CSTRING *name,
                                          List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 0:
  {
    func= new (thd->mem_root) Item_func_unix_timestamp(thd);
    thd->lex->safe_to_cache_query= 0;
    break;
  }
  case 1:
  {
    Item *param_1= item_list->pop();
    func= new (thd->mem_root) Item_func_unix_timestamp(thd, param_1);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


Create_func_uuid Create_func_uuid::s_singleton;

Item*
Create_func_uuid::create_builder(THD *thd)
{
  DBUG_ENTER("Create_func_uuid::create");
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  thd->lex->safe_to_cache_query= 0;
  DBUG_RETURN(new (thd->mem_root) Item_func_uuid(thd));
}


Create_func_uuid_short Create_func_uuid_short::s_singleton;

Item*
Create_func_uuid_short::create_builder(THD *thd)
{
  DBUG_ENTER("Create_func_uuid_short::create");
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  thd->lex->safe_to_cache_query= 0;
  DBUG_RETURN(new (thd->mem_root) Item_func_uuid_short(thd));
}


Create_func_version Create_func_version::s_singleton;

Item*
Create_func_version::create_builder(THD *thd)
{
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  static Lex_cstring name(STRING_WITH_LEN("version()"));
  return new (thd->mem_root) Item_static_string_func(thd, name,
                                                     Lex_cstring_strlen(server_version),
                                                     system_charset_info,
                                                     DERIVATION_SYSCONST);
}


Create_func_weekday Create_func_weekday::s_singleton;

Item*
Create_func_weekday::create_1_arg(THD *thd, Item *arg1)
{
  return new (thd->mem_root) Item_func_weekday(thd, arg1, 0);
}


Create_func_weekofyear Create_func_weekofyear::s_singleton;

Item*
Create_func_weekofyear::create_1_arg(THD *thd, Item *arg1)
{
  Item *i1= new (thd->mem_root) Item_int(thd, (char*) "3", 3, 1);
  return new (thd->mem_root) Item_func_week(thd, arg1, i1);
}


#ifdef WITH_WSREP
Create_func_wsrep_last_written_gtid
Create_func_wsrep_last_written_gtid::s_singleton;

Item*
Create_func_wsrep_last_written_gtid::create_builder(THD *thd)
{
  thd->lex->safe_to_cache_query= 0;
  return new (thd->mem_root) Item_func_wsrep_last_written_gtid(thd);
}


Create_func_wsrep_last_seen_gtid
Create_func_wsrep_last_seen_gtid::s_singleton;

Item*
Create_func_wsrep_last_seen_gtid::create_builder(THD *thd)
{
  thd->lex->safe_to_cache_query= 0;
  return new (thd->mem_root) Item_func_wsrep_last_seen_gtid(thd);
}


Create_func_wsrep_sync_wait_upto
Create_func_wsrep_sync_wait_upto::s_singleton;

Item*
Create_func_wsrep_sync_wait_upto::create_native(THD *thd,
                                         LEX_CSTRING *name,
                                         List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;
  Item *param_1, *param_2;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count)
  {
  case 1:
    param_1= item_list->pop();
    func= new (thd->mem_root) Item_func_wsrep_sync_wait_upto(thd, param_1);
    break;
  case 2:
    param_1= item_list->pop();
    param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_wsrep_sync_wait_upto(thd, param_1, param_2);
    break;
  default:
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  thd->lex->safe_to_cache_query= 0;
  return func;
}
#endif /* WITH_WSREP */

Create_func_xml_extractvalue Create_func_xml_extractvalue::s_singleton;

Item*
Create_func_xml_extractvalue::create_2_arg(THD *thd, Item *arg1, Item *arg2)
{
  return new (thd->mem_root) Item_func_xml_extractvalue(thd, arg1, arg2);
}


Create_func_xml_update Create_func_xml_update::s_singleton;

Item*
Create_func_xml_update::create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3)
{
  return new (thd->mem_root) Item_func_xml_update(thd, arg1, arg2, arg3);
}


Create_func_year_week Create_func_year_week::s_singleton;

Item*
Create_func_year_week::create_native(THD *thd, LEX_CSTRING *name,
                                     List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 1:
  {
    Item *param_1= item_list->pop();
    Item *i0= new (thd->mem_root) Item_int(thd, (char*) "0", 0, 1);
    func= new (thd->mem_root) Item_func_yearweek(thd, param_1, i0);
    break;
  }
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_yearweek(thd, param_1, param_2);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


#define BUILDER(F) & F::s_singleton

/*
  MySQL native functions.
  MAINTAINER:
  - Keep sorted for human lookup. At runtime, a hash table is used.
  - do **NOT** conditionally (#ifdef, #ifndef) define a function *NAME*:
    doing so will cause user code that works against a --without-XYZ binary
    to fail with name collisions against a --with-XYZ binary.
    Use something similar to GEOM_BUILDER instead.
  - keep 1 line per entry, it makes grep | sort easier
*/

static Native_func_registry func_array[] =
{
  { { STRING_WITH_LEN("ABS") }, BUILDER(Create_func_abs)},
  { { STRING_WITH_LEN("ACOS") }, BUILDER(Create_func_acos)},
  { { STRING_WITH_LEN("ADDTIME") }, BUILDER(Create_func_addtime)},
  { { STRING_WITH_LEN("AES_DECRYPT") }, BUILDER(Create_func_aes_decrypt)},
  { { STRING_WITH_LEN("AES_ENCRYPT") }, BUILDER(Create_func_aes_encrypt)},
  { { STRING_WITH_LEN("ASIN") }, BUILDER(Create_func_asin)},
  { { STRING_WITH_LEN("ATAN") }, BUILDER(Create_func_atan)},
  { { STRING_WITH_LEN("ATAN2") }, BUILDER(Create_func_atan)},
  { { STRING_WITH_LEN("BENCHMARK") }, BUILDER(Create_func_benchmark)},
  { { STRING_WITH_LEN("BIN") }, BUILDER(Create_func_bin)},
  { { STRING_WITH_LEN("BINLOG_GTID_POS") }, BUILDER(Create_func_binlog_gtid_pos)},
  { { STRING_WITH_LEN("BIT_COUNT") }, BUILDER(Create_func_bit_count)},
  { { STRING_WITH_LEN("BIT_LENGTH") }, BUILDER(Create_func_bit_length)},
  { { STRING_WITH_LEN("CEIL") }, BUILDER(Create_func_ceiling)},
  { { STRING_WITH_LEN("CEILING") }, BUILDER(Create_func_ceiling)},
  { { STRING_WITH_LEN("CHARACTER_LENGTH") }, BUILDER(Create_func_char_length)},
  { { STRING_WITH_LEN("CHAR_LENGTH") }, BUILDER(Create_func_char_length)},
  { { STRING_WITH_LEN("CHR") }, BUILDER(Create_func_chr)},
  { { STRING_WITH_LEN("COERCIBILITY") }, BUILDER(Create_func_coercibility)},
  { { STRING_WITH_LEN("COLUMN_CHECK") }, BUILDER(Create_func_dyncol_check)},
  { { STRING_WITH_LEN("COLUMN_EXISTS") }, BUILDER(Create_func_dyncol_exists)},
  { { STRING_WITH_LEN("COLUMN_LIST") }, BUILDER(Create_func_dyncol_list)},
  { { STRING_WITH_LEN("COLUMN_JSON") }, BUILDER(Create_func_dyncol_json)},
  { { STRING_WITH_LEN("COMPRESS") }, BUILDER(Create_func_compress)},
  { { STRING_WITH_LEN("CONCAT") }, BUILDER(Create_func_concat)},
  { { STRING_WITH_LEN("CONCAT_OPERATOR_ORACLE") }, BUILDER(Create_func_concat_operator_oracle)},
  { { STRING_WITH_LEN("CONCAT_WS") }, BUILDER(Create_func_concat_ws)},
  { { STRING_WITH_LEN("CONNECTION_ID") }, BUILDER(Create_func_connection_id)},
  { { STRING_WITH_LEN("CONV") }, BUILDER(Create_func_conv)},
  { { STRING_WITH_LEN("CONVERT_TZ") }, BUILDER(Create_func_convert_tz)},
  { { STRING_WITH_LEN("COS") }, BUILDER(Create_func_cos)},
  { { STRING_WITH_LEN("COT") }, BUILDER(Create_func_cot)},
  { { STRING_WITH_LEN("CRC32") }, BUILDER(Create_func_crc32)},
  { { STRING_WITH_LEN("DATEDIFF") }, BUILDER(Create_func_datediff)},
  { { STRING_WITH_LEN("DAYNAME") }, BUILDER(Create_func_dayname)},
  { { STRING_WITH_LEN("DAYOFMONTH") }, BUILDER(Create_func_dayofmonth)},
  { { STRING_WITH_LEN("DAYOFWEEK") }, BUILDER(Create_func_dayofweek)},
  { { STRING_WITH_LEN("DAYOFYEAR") }, BUILDER(Create_func_dayofyear)},
  { { STRING_WITH_LEN("DEGREES") }, BUILDER(Create_func_degrees)},
  { { STRING_WITH_LEN("DECODE_HISTOGRAM") }, BUILDER(Create_func_decode_histogram)},
  { { STRING_WITH_LEN("DECODE_ORACLE") }, BUILDER(Create_func_decode_oracle)},
  { { STRING_WITH_LEN("DES_DECRYPT") }, BUILDER(Create_func_des_decrypt)},
  { { STRING_WITH_LEN("DES_ENCRYPT") }, BUILDER(Create_func_des_encrypt)},
  { { STRING_WITH_LEN("ELT") }, BUILDER(Create_func_elt)},
  { { STRING_WITH_LEN("ENCODE") }, BUILDER(Create_func_encode)},
  { { STRING_WITH_LEN("ENCRYPT") }, BUILDER(Create_func_encrypt)},
  { { STRING_WITH_LEN("EXP") }, BUILDER(Create_func_exp)},
  { { STRING_WITH_LEN("EXPORT_SET") }, BUILDER(Create_func_export_set)},
  { { STRING_WITH_LEN("EXTRACTVALUE") }, BUILDER(Create_func_xml_extractvalue)},
  { { STRING_WITH_LEN("FIELD") }, BUILDER(Create_func_field)},
  { { STRING_WITH_LEN("FIND_IN_SET") }, BUILDER(Create_func_find_in_set)},
  { { STRING_WITH_LEN("FLOOR") }, BUILDER(Create_func_floor)},
  { { STRING_WITH_LEN("FORMAT") }, BUILDER(Create_func_format)},
  { { STRING_WITH_LEN("FOUND_ROWS") }, BUILDER(Create_func_found_rows)},
  { { STRING_WITH_LEN("FROM_BASE64") }, BUILDER(Create_func_from_base64)},
  { { STRING_WITH_LEN("FROM_DAYS") }, BUILDER(Create_func_from_days)},
  { { STRING_WITH_LEN("FROM_UNIXTIME") }, BUILDER(Create_func_from_unixtime)},
  { { STRING_WITH_LEN("GET_LOCK") }, BUILDER(Create_func_get_lock)},
  { { STRING_WITH_LEN("GREATEST") }, BUILDER(Create_func_greatest)},
  { { STRING_WITH_LEN("HEX") }, BUILDER(Create_func_hex)},
  { { STRING_WITH_LEN("IFNULL") }, BUILDER(Create_func_ifnull)},
  { { STRING_WITH_LEN("INSTR") }, BUILDER(Create_func_instr)},
  { { STRING_WITH_LEN("ISNULL") }, BUILDER(Create_func_isnull)},
  { { STRING_WITH_LEN("IS_FREE_LOCK") }, BUILDER(Create_func_is_free_lock)},
  { { STRING_WITH_LEN("IS_USED_LOCK") }, BUILDER(Create_func_is_used_lock)},
  { { STRING_WITH_LEN("JSON_ARRAY") }, BUILDER(Create_func_json_array)},
  { { STRING_WITH_LEN("JSON_ARRAY_APPEND") }, BUILDER(Create_func_json_array_append)},
  { { STRING_WITH_LEN("JSON_ARRAY_INSERT") }, BUILDER(Create_func_json_array_insert)},
  { { STRING_WITH_LEN("JSON_COMPACT") }, BUILDER(Create_func_json_compact)},
  { { STRING_WITH_LEN("JSON_CONTAINS") }, BUILDER(Create_func_json_contains)},
  { { STRING_WITH_LEN("JSON_CONTAINS_PATH") }, BUILDER(Create_func_json_contains_path)},
  { { STRING_WITH_LEN("JSON_DEPTH") }, BUILDER(Create_func_json_depth)},
  { { STRING_WITH_LEN("JSON_DETAILED") }, BUILDER(Create_func_json_detailed)},
  { { STRING_WITH_LEN("JSON_EXISTS") }, BUILDER(Create_func_json_exists)},
  { { STRING_WITH_LEN("JSON_EXTRACT") }, BUILDER(Create_func_json_extract)},
  { { STRING_WITH_LEN("JSON_INSERT") }, BUILDER(Create_func_json_insert)},
  { { STRING_WITH_LEN("JSON_KEYS") }, BUILDER(Create_func_json_keys)},
  { { STRING_WITH_LEN("JSON_LENGTH") }, BUILDER(Create_func_json_length)},
  { { STRING_WITH_LEN("JSON_LOOSE") }, BUILDER(Create_func_json_loose)},
  { { STRING_WITH_LEN("JSON_MERGE") }, BUILDER(Create_func_json_merge)},
  { { STRING_WITH_LEN("JSON_MERGE_PATCH") }, BUILDER(Create_func_json_merge_patch)},
  { { STRING_WITH_LEN("JSON_MERGE_PRESERVE") }, BUILDER(Create_func_json_merge)},
  { { STRING_WITH_LEN("JSON_QUERY") }, BUILDER(Create_func_json_query)},
  { { STRING_WITH_LEN("JSON_QUOTE") }, BUILDER(Create_func_json_quote)},
  { { STRING_WITH_LEN("JSON_OBJECT") }, BUILDER(Create_func_json_object)},
  { { STRING_WITH_LEN("JSON_REMOVE") }, BUILDER(Create_func_json_remove)},
  { { STRING_WITH_LEN("JSON_REPLACE") }, BUILDER(Create_func_json_replace)},
  { { STRING_WITH_LEN("JSON_SET") }, BUILDER(Create_func_json_set)},
  { { STRING_WITH_LEN("JSON_SEARCH") }, BUILDER(Create_func_json_search)},
  { { STRING_WITH_LEN("JSON_TYPE") }, BUILDER(Create_func_json_type)},
  { { STRING_WITH_LEN("JSON_UNQUOTE") }, BUILDER(Create_func_json_unquote)},
  { { STRING_WITH_LEN("JSON_VALID") }, BUILDER(Create_func_json_valid)},
  { { STRING_WITH_LEN("JSON_VALUE") }, BUILDER(Create_func_json_value)},
  { { STRING_WITH_LEN("LAST_DAY") }, BUILDER(Create_func_last_day)},
  { { STRING_WITH_LEN("LAST_INSERT_ID") }, BUILDER(Create_func_last_insert_id)},
  { { STRING_WITH_LEN("LCASE") }, BUILDER(Create_func_lcase)},
  { { STRING_WITH_LEN("LEAST") }, BUILDER(Create_func_least)},
  { { STRING_WITH_LEN("LENGTH") }, BUILDER(Create_func_length)},
  { { STRING_WITH_LEN("LENGTHB") }, BUILDER(Create_func_octet_length)},
#ifndef DBUG_OFF
  { { STRING_WITH_LEN("LIKE_RANGE_MIN") }, BUILDER(Create_func_like_range_min)},
  { { STRING_WITH_LEN("LIKE_RANGE_MAX") }, BUILDER(Create_func_like_range_max)},
#endif
  { { STRING_WITH_LEN("LN") }, BUILDER(Create_func_ln)},
  { { STRING_WITH_LEN("LOAD_FILE") }, BUILDER(Create_func_load_file)},
  { { STRING_WITH_LEN("LOCATE") }, BUILDER(Create_func_locate)},
  { { STRING_WITH_LEN("LOG") }, BUILDER(Create_func_log)},
  { { STRING_WITH_LEN("LOG10") }, BUILDER(Create_func_log10)},
  { { STRING_WITH_LEN("LOG2") }, BUILDER(Create_func_log2)},
  { { STRING_WITH_LEN("LOWER") }, BUILDER(Create_func_lcase)},
  { { STRING_WITH_LEN("LPAD") }, BUILDER(Create_func_lpad)},
  { { STRING_WITH_LEN("LPAD_ORACLE") }, BUILDER(Create_func_lpad_oracle)},
  { { STRING_WITH_LEN("LTRIM") }, BUILDER(Create_func_ltrim)},
  { { STRING_WITH_LEN("LTRIM_ORACLE") }, BUILDER(Create_func_ltrim_oracle)},
  { { STRING_WITH_LEN("MAKEDATE") }, BUILDER(Create_func_makedate)},
  { { STRING_WITH_LEN("MAKETIME") }, BUILDER(Create_func_maketime)},
  { { STRING_WITH_LEN("MAKE_SET") }, BUILDER(Create_func_make_set)},
  { { STRING_WITH_LEN("MASTER_GTID_WAIT") }, BUILDER(Create_func_master_gtid_wait)},
  { { STRING_WITH_LEN("MASTER_POS_WAIT") }, BUILDER(Create_func_master_pos_wait)},
  { { STRING_WITH_LEN("MD5") }, BUILDER(Create_func_md5)},
  { { STRING_WITH_LEN("MONTHNAME") }, BUILDER(Create_func_monthname)},
  { { STRING_WITH_LEN("NAME_CONST") }, BUILDER(Create_func_name_const)},
  { { STRING_WITH_LEN("NVL") }, BUILDER(Create_func_ifnull)},
  { { STRING_WITH_LEN("NVL2") }, BUILDER(Create_func_nvl2)},
  { { STRING_WITH_LEN("NULLIF") }, BUILDER(Create_func_nullif)},
  { { STRING_WITH_LEN("OCT") }, BUILDER(Create_func_oct)},
  { { STRING_WITH_LEN("OCTET_LENGTH") }, BUILDER(Create_func_octet_length)},
  { { STRING_WITH_LEN("ORD") }, BUILDER(Create_func_ord)},
  { { STRING_WITH_LEN("PERIOD_ADD") }, BUILDER(Create_func_period_add)},
  { { STRING_WITH_LEN("PERIOD_DIFF") }, BUILDER(Create_func_period_diff)},
  { { STRING_WITH_LEN("PI") }, BUILDER(Create_func_pi)},
  { { STRING_WITH_LEN("POW") }, BUILDER(Create_func_pow)},
  { { STRING_WITH_LEN("POWER") }, BUILDER(Create_func_pow)},
  { { STRING_WITH_LEN("QUOTE") }, BUILDER(Create_func_quote)},
  { { STRING_WITH_LEN("REGEXP_INSTR") }, BUILDER(Create_func_regexp_instr)},
  { { STRING_WITH_LEN("REGEXP_REPLACE") }, BUILDER(Create_func_regexp_replace)},
  { { STRING_WITH_LEN("REGEXP_SUBSTR") }, BUILDER(Create_func_regexp_substr)},
  { { STRING_WITH_LEN("RADIANS") }, BUILDER(Create_func_radians)},
  { { STRING_WITH_LEN("RAND") }, BUILDER(Create_func_rand)},
  { { STRING_WITH_LEN("RELEASE_ALL_LOCKS") },
      BUILDER(Create_func_release_all_locks)},
  { { STRING_WITH_LEN("RELEASE_LOCK") }, BUILDER(Create_func_release_lock)},
  { { STRING_WITH_LEN("REPLACE_ORACLE") },
      BUILDER(Create_func_replace_oracle)},
  { { STRING_WITH_LEN("REVERSE") }, BUILDER(Create_func_reverse)},
  { { STRING_WITH_LEN("ROUND") }, BUILDER(Create_func_round)},
  { { STRING_WITH_LEN("RPAD") }, BUILDER(Create_func_rpad)},
  { { STRING_WITH_LEN("RPAD_ORACLE") }, BUILDER(Create_func_rpad_oracle)},
  { { STRING_WITH_LEN("RTRIM") }, BUILDER(Create_func_rtrim)},
  { { STRING_WITH_LEN("RTRIM_ORACLE") }, BUILDER(Create_func_rtrim_oracle)},
  { { STRING_WITH_LEN("SEC_TO_TIME") }, BUILDER(Create_func_sec_to_time)},
  { { STRING_WITH_LEN("SHA") }, BUILDER(Create_func_sha)},
  { { STRING_WITH_LEN("SHA1") }, BUILDER(Create_func_sha)},
  { { STRING_WITH_LEN("SHA2") }, BUILDER(Create_func_sha2)},
  { { STRING_WITH_LEN("SIGN") }, BUILDER(Create_func_sign)},
  { { STRING_WITH_LEN("SIN") }, BUILDER(Create_func_sin)},
  { { STRING_WITH_LEN("SLEEP") }, BUILDER(Create_func_sleep)},
  { { STRING_WITH_LEN("SOUNDEX") }, BUILDER(Create_func_soundex)},
  { { STRING_WITH_LEN("SPACE") }, BUILDER(Create_func_space)},
  { { STRING_WITH_LEN("SQRT") }, BUILDER(Create_func_sqrt)},
  { { STRING_WITH_LEN("STRCMP") }, BUILDER(Create_func_strcmp)},
  { { STRING_WITH_LEN("STR_TO_DATE") }, BUILDER(Create_func_str_to_date)},
  { { STRING_WITH_LEN("SUBSTR_ORACLE") },
      BUILDER(Create_func_substr_oracle)},
  { { STRING_WITH_LEN("SUBSTRING_INDEX") }, BUILDER(Create_func_substr_index)},
  { { STRING_WITH_LEN("SUBTIME") }, BUILDER(Create_func_subtime)},
  { { STRING_WITH_LEN("TAN") }, BUILDER(Create_func_tan)},
  { { STRING_WITH_LEN("TIMEDIFF") }, BUILDER(Create_func_timediff)},
  { { STRING_WITH_LEN("TIME_FORMAT") }, BUILDER(Create_func_time_format)},
  { { STRING_WITH_LEN("TIME_TO_SEC") }, BUILDER(Create_func_time_to_sec)},
  { { STRING_WITH_LEN("TO_BASE64") }, BUILDER(Create_func_to_base64)},
  { { STRING_WITH_LEN("TO_DAYS") }, BUILDER(Create_func_to_days)},
  { { STRING_WITH_LEN("TO_SECONDS") }, BUILDER(Create_func_to_seconds)},
  { { STRING_WITH_LEN("UCASE") }, BUILDER(Create_func_ucase)},
  { { STRING_WITH_LEN("UNCOMPRESS") }, BUILDER(Create_func_uncompress)},
  { { STRING_WITH_LEN("UNCOMPRESSED_LENGTH") }, BUILDER(Create_func_uncompressed_length)},
  { { STRING_WITH_LEN("UNHEX") }, BUILDER(Create_func_unhex)},
  { { STRING_WITH_LEN("UNIX_TIMESTAMP") }, BUILDER(Create_func_unix_timestamp)},
  { { STRING_WITH_LEN("UPDATEXML") }, BUILDER(Create_func_xml_update)},
  { { STRING_WITH_LEN("UPPER") }, BUILDER(Create_func_ucase)},
  { { STRING_WITH_LEN("UUID") }, BUILDER(Create_func_uuid)},
  { { STRING_WITH_LEN("UUID_SHORT") }, BUILDER(Create_func_uuid_short)},
  { { STRING_WITH_LEN("VERSION") }, BUILDER(Create_func_version)},
  { { STRING_WITH_LEN("WEEKDAY") }, BUILDER(Create_func_weekday)},
  { { STRING_WITH_LEN("WEEKOFYEAR") }, BUILDER(Create_func_weekofyear)},
#ifdef WITH_WSREP
  { { STRING_WITH_LEN("WSREP_LAST_WRITTEN_GTID") }, BUILDER(Create_func_wsrep_last_written_gtid)},
  { { STRING_WITH_LEN("WSREP_LAST_SEEN_GTID") }, BUILDER(Create_func_wsrep_last_seen_gtid)},
  { { STRING_WITH_LEN("WSREP_SYNC_WAIT_UPTO_GTID") }, BUILDER(Create_func_wsrep_sync_wait_upto)},
#endif /* WITH_WSREP */
  { { STRING_WITH_LEN("YEARWEEK") }, BUILDER(Create_func_year_week)}
};

Native_func_registry_array
  native_func_registry_array(func_array, array_elements(func_array));

static HASH native_functions_hash;

/*
  Load the hash table for native functions.
  Note: this code is not thread safe, and is intended to be used at server
  startup only (before going multi-threaded)
*/

int item_create_init()
{
  DBUG_ENTER("item_create_init");
  size_t count= native_func_registry_array.count();
#ifdef HAVE_SPATIAL
  count+= native_func_registry_array_geom.count();
#endif
  if (my_hash_init(key_memory_native_functions, & native_functions_hash,
                   system_charset_info, (ulong) count, 0, 0, (my_hash_get_key)
                   get_native_fct_hash_key, NULL, MYF(0)))
    DBUG_RETURN(1);

  if (native_func_registry_array.append_to_hash(&native_functions_hash))
    DBUG_RETURN(1);

#ifdef HAVE_SPATIAL
  if (native_func_registry_array_geom.append_to_hash(&native_functions_hash))
    DBUG_RETURN(1);
#endif

#ifndef DBUG_OFF
  for (uint i=0 ; i < native_functions_hash.records ; i++)
  {
    Native_func_registry *func;
    func= (Native_func_registry*) my_hash_element(& native_functions_hash, i);
    DBUG_PRINT("info", ("native function: %s  length: %u",
                        func->name.str, (uint) func->name.length));
  }
#endif

  DBUG_RETURN(0);
}


/*
  This function is used (dangerously) by plugin/versioning/versioning.cc
  TODO: MDEV-20842 Wrap SQL functions defined in
        plugin/versioning/versioning.cc into MariaDB_FUNCTION_PLUGIN
*/
int item_create_append(Native_func_registry array[])
{
  Native_func_registry *func;

  DBUG_ENTER("item_create_append");

  for (func= array; func->builder != NULL; func++)
  {
    if (my_hash_insert(& native_functions_hash, (uchar*) func))
      DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}

/*
  Empty the hash table for native functions.
  Note: this code is not thread safe, and is intended to be used at server
  shutdown only (after thread requests have been executed).
*/

void item_create_cleanup()
{
  DBUG_ENTER("item_create_cleanup");
  my_hash_free(& native_functions_hash);
  DBUG_VOID_RETURN;
}


static Create_func *
function_plugin_find_native_function_builder(THD *thd, const LEX_CSTRING &name)
{
  plugin_ref plugin;
  if ((plugin= my_plugin_lock_by_name(thd, &name, MariaDB_FUNCTION_PLUGIN)))
  {
    Create_func *builder=
      reinterpret_cast<Plugin_function*>(plugin_decl(plugin)->info)->
        create_func();
    // TODO: MDEV-20846 Add proper unlocking for MariaDB_FUNCTION_PLUGIN
    plugin_unlock(thd, plugin);
    return builder;
  }
  return NULL;
}


Create_func *
find_native_function_builder(THD *thd, const LEX_CSTRING *name)
{
  Native_func_registry *func;
  Create_func *builder= NULL;

  /* Thread safe */
  func= (Native_func_registry*) my_hash_search(&native_functions_hash,
                                               (uchar*) name->str,
                                               name->length);

  if (func && (builder= func->builder))
    return builder;

  if ((builder= function_plugin_find_native_function_builder(thd, *name)))
    return builder;

  return NULL;
}

Create_qfunc *
find_qualified_function_builder(THD *thd)
{
  return & Create_sp_func::s_singleton;
}


static List<Item> *create_func_dyncol_prepare(THD *thd,
                                              DYNCALL_CREATE_DEF **dfs,
                                              List<DYNCALL_CREATE_DEF> &list)
{
  DYNCALL_CREATE_DEF *def;
  List_iterator_fast<DYNCALL_CREATE_DEF> li(list);
  List<Item> *args= new (thd->mem_root) List<Item>;

  *dfs= (DYNCALL_CREATE_DEF *)alloc_root(thd->mem_root,
                                         sizeof(DYNCALL_CREATE_DEF) *
                                         list.elements);

  if (!args || !*dfs)
    return NULL;

  for (uint i= 0; (def= li++) ;)
  {
    dfs[0][i++]= *def;
    args->push_back(def->key, thd->mem_root);
    args->push_back(def->value, thd->mem_root);
  }
  return args;
}

Item *create_func_dyncol_create(THD *thd, List<DYNCALL_CREATE_DEF> &list)
{
  List<Item> *args;
  DYNCALL_CREATE_DEF *dfs;
  if (!(args= create_func_dyncol_prepare(thd, &dfs, list)))
    return NULL;

  return new (thd->mem_root) Item_func_dyncol_create(thd, *args, dfs);
}

Item *create_func_dyncol_add(THD *thd, Item *str,
                             List<DYNCALL_CREATE_DEF> &list)
{
  List<Item> *args;
  DYNCALL_CREATE_DEF *dfs;

  if (!(args= create_func_dyncol_prepare(thd, &dfs, list)))
    return NULL;

  args->push_back(str, thd->mem_root);

  return new (thd->mem_root) Item_func_dyncol_add(thd, *args, dfs);
}



Item *create_func_dyncol_delete(THD *thd, Item *str, List<Item> &nums)
{
  DYNCALL_CREATE_DEF *dfs;
  Item *key;
  List_iterator_fast<Item> it(nums);
  List<Item> *args= new (thd->mem_root) List<Item>;

  dfs= (DYNCALL_CREATE_DEF *)alloc_root(thd->mem_root,
                                        sizeof(DYNCALL_CREATE_DEF) *
                                        nums.elements);
  if (!args || !dfs)
    return NULL;

  for (uint i= 0; (key= it++); i++)
  {
    dfs[i].key= key;
    dfs[i].value= new (thd->mem_root) Item_null(thd);
    dfs[i].type= DYN_COL_INT;
    args->push_back(dfs[i].key, thd->mem_root);
    args->push_back(dfs[i].value, thd->mem_root);
  }

  args->push_back(str, thd->mem_root);

  return new (thd->mem_root) Item_func_dyncol_add(thd, *args, dfs);
}


Item *create_func_dyncol_get(THD *thd,  Item *str, Item *num,
                             const Type_handler *handler,
                             const char *c_len, const char *c_dec,
                             CHARSET_INFO *cs)
{
  Item *res;

  if (likely(!(res= new (thd->mem_root) Item_dyncol_get(thd, str, num))))
    return res;                                 // Return NULL
  return handler->create_typecast_item(thd, res,
                                       Type_cast_attributes(c_len, c_dec, cs));
}

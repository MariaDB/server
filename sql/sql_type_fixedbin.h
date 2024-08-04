#ifndef SQL_TYPE_FIXEDBIN_H
#define SQL_TYPE_FIXEDBIN_H
/* Copyright (c) 2019,2021 MariaDB Corporation

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

/*
  This is a common code for plugin (?) types that are generally
  handled like strings, but have their own fixed size on-disk binary storage
  format and their own (variable size) canonical string representation.

  Examples are INET6 and UUID types.
*/

#define MYSQL_SERVER
#include "sql_class.h" // THD, SORT_FIELD_ATTR
#include "opt_range.h" // SEL_ARG, null_element
#include "sql_type_fixedbin_storage.h"

/***********************************************************************/


template<class FbtImpl> class Type_collection_fbt;

template<class FbtImpl, class TypeCollectionImpl = Type_collection_fbt<FbtImpl> >
class Type_handler_fbt: public Type_handler
{
  /* =[ internal helper classes ]=============================== */

public:
  class Fbt: public FbtImpl
  {
  protected:
    using FbtImpl::m_buffer;
    bool make_from_item(Item *item, bool warn)
    {
      if (item->type_handler() == singleton())
      {
        Native tmp(m_buffer, sizeof(m_buffer));
        bool rc= item->val_native(current_thd, &tmp);
        if (rc)
          return true;
        DBUG_ASSERT(tmp.length() == sizeof(m_buffer));
        if (tmp.ptr() != m_buffer)
          memcpy(m_buffer, tmp.ptr(), sizeof(m_buffer));
        return false;
      }
      StringBuffer<FbtImpl::max_char_length()+1> tmp;
      String *str= item->val_str(&tmp);
      return str ? make_from_character_or_binary_string(str, warn) : true;
    }

    bool character_string_to_fbt(const char *str, size_t str_length,
                                  CHARSET_INFO *cs)
    {
      if (cs->state & MY_CS_NONASCII)
      {
        char tmp[FbtImpl::max_char_length()+1];
        String_copier copier;
        uint length= copier.well_formed_copy(&my_charset_latin1, tmp, sizeof(tmp),
                                             cs, str, str_length);
        return FbtImpl::ascii_to_fbt(tmp, length);
      }
      return FbtImpl::ascii_to_fbt(str, str_length);
    }
    bool make_from_character_or_binary_string(const String *str, bool warn)
    {
      if (str->charset() != &my_charset_bin)
      {
        bool rc= character_string_to_fbt(str->ptr(), str->length(),
                                          str->charset());
        if (rc && warn)
          current_thd->push_warning_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                       singleton()->name().ptr(), ErrConvString(str).ptr());
        return rc;
      }
      if (str->length() != sizeof(m_buffer))
      {
        if (warn)
          current_thd->push_warning_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                       singleton()->name().ptr(), ErrConvString(str).ptr());
        return true;
      }
      DBUG_ASSERT(str->ptr() != m_buffer);
      memcpy(m_buffer, str->ptr(), sizeof(m_buffer));
      return false;
    }
    bool binary_to_fbt(const char *str, size_t length)
    {
      if (length != sizeof(m_buffer))
        return true;
      memcpy(m_buffer, str, length);
      return false;
    }

    Fbt() { }

  public:

     static Fbt zero()
     {
       Fbt fbt;
       fbt.set_zero();
       return fbt;
     }

     static Fbt record_to_memory(const char *ptr)
     {
       Fbt fbt;
       FbtImpl::record_to_memory(fbt.m_buffer, ptr);
       return fbt;
     }
    /*
      Check at Item's fix_fields() time if "item" can return a nullable value
      on conversion to Fbt, or conversion produces a NOT NULL Fbt value.
    */
    static bool fix_fields_maybe_null_on_conversion_to_fbt(Item *item)
    {
      if (item->maybe_null())
        return true;
      if (item->type_handler() == singleton())
        return false;
      if (!item->const_item() || item->is_expensive())
        return true;
      return Fbt_null(item, false).is_null();
    }

    /*
      Check at fix_fields() time if any of the items can return a nullable
      value on conversion to Fbt.
    */
    static bool fix_fields_maybe_null_on_conversion_to_fbt(Item **items,
                                                           uint count)
    {
      for (uint i= 0; i < count; i++)
      {
        if (Fbt::fix_fields_maybe_null_on_conversion_to_fbt(items[i]))
          return true;
      }
      return false;
    }

  public:

    Fbt(Item *item, bool *error, bool warn= true)
    {
      *error= make_from_item(item, warn);
    }
    void to_record(char *str, size_t str_size) const
    {
      DBUG_ASSERT(str_size >= sizeof(m_buffer));
      FbtImpl::memory_to_record(str, m_buffer);
    }
    bool to_binary(String *to) const
    {
      return to->copy(m_buffer, sizeof(m_buffer), &my_charset_bin);
    }
    bool to_native(Native *to) const
    {
      return to->copy(m_buffer, sizeof(m_buffer));
    }
    bool to_string(String *to) const
    {
      to->set_charset(&my_charset_latin1);
      if (to->alloc(FbtImpl::max_char_length()+1))
        return true;
      to->length((uint32) FbtImpl::to_string(const_cast<char*>(to->ptr()),
                                             FbtImpl::max_char_length()+1));
      return false;
    }
    int cmp(const Binary_string &other) const
    {
      return FbtImpl::cmp(FbtImpl::to_lex_cstring(), other.to_lex_cstring());
    }
    int cmp(const Fbt &other) const
    {
      return FbtImpl::cmp(FbtImpl::to_lex_cstring(), other.to_lex_cstring());
    }
  };

  class Fbt_null: public Fbt, public Null_flag
  {
  public:
    // Initialize from a text representation
    Fbt_null(const char *str, size_t length, CHARSET_INFO *cs)
     :Null_flag(Fbt::character_string_to_fbt(str, length, cs)) { }
    Fbt_null(const String &str)
     :Fbt_null(str.ptr(), str.length(), str.charset()) { }
    // Initialize from a binary representation
    Fbt_null(const char *str, size_t length)
     :Null_flag(Fbt::binary_to_fbt(str, length)) { }
    Fbt_null(const Binary_string &str)
     :Fbt_null(str.ptr(), str.length()) { }
    // Initialize from an Item
    Fbt_null(Item *item, bool warn= true)
     :Null_flag(Fbt::make_from_item(item, warn)) { }
  public:
    const Fbt& to_fbt() const
    {
      DBUG_ASSERT(!is_null());
      return *this;
    }
    void to_record(char *str, size_t str_size) const
    {
      to_fbt().to_record(str, str_size);
    }
    bool to_binary(String *to) const
    {
      return to_fbt().to_binary(to);
    }
    bool to_string(String *to) const
    {
      return to_fbt().to_string(to);
    }
  };

  /* =[ API classes ]=========================================== */

  class Type_std_attributes_fbt: public Type_std_attributes
  {
  public:
    Type_std_attributes_fbt()
     :Type_std_attributes(
        Type_numeric_attributes(FbtImpl::max_char_length(), 0, true),
        DTCollation_numeric())
    { }
  };

  class Item_literal_fbt: public Item_literal
  {
    Fbt m_value;
  public:
    Item_literal_fbt(THD *thd)
     :Item_literal(thd),
      m_value(Fbt::zero())
    { }
    Item_literal_fbt(THD *thd, const Fbt &value)
     :Item_literal(thd),
      m_value(value)
    { }
    const Type_handler *type_handler() const override
    {
      return singleton();
    }
    longlong val_int() override
    {
      return 0;
    }
    double val_real() override
    {
      return 0;
    }
    String *val_str(String *to) override
    {
      return m_value.to_string(to) ? NULL : to;
    }
    my_decimal *val_decimal(my_decimal *to) override
    {
      my_decimal_set_zero(to);
      return to;
    }
    bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
    {
      set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);
      return false;
    }
    bool val_native(THD *thd, Native *to) override
    {
      return m_value.to_native(to);
    }
    void print(String *str, enum_query_type query_type) override
    {
      StringBuffer<FbtImpl::max_char_length()+64> tmp;
      tmp.append(singleton()->name().lex_cstring());
      my_caseup_str(&my_charset_latin1, tmp.c_ptr());
      str->append(tmp);
      str->append('\'');
      m_value.to_string(&tmp);
      str->append(tmp);
      str->append('\'');
    }
    Item *do_get_copy(THD *thd) const override
    { return get_item_copy<Item_literal_fbt>(thd, this); }
    Item *do_build_clone(THD *thd) const override { return get_copy(thd); }

    // Non-overriding methods
    void set_value(const Fbt &value)
    {
      m_value= value;
    }
  };

  class Field_fbt: public Field
  {
    static void set_min_value(char *ptr)
    {
      memset(ptr, 0, FbtImpl::binary_length());
    }
    static void set_max_value(char *ptr)
    {
      memset(ptr, 0xFF, FbtImpl::binary_length());
    }
    void store_warning(const ErrConv &str,
                       Sql_condition::enum_warning_level level)
    {
      if (get_thd()->count_cuted_fields <= CHECK_FIELD_EXPRESSION)
        return;
      const TABLE_SHARE *s= table->s;
      static const Name type_name= singleton()->name();
      get_thd()->push_warning_truncated_value_for_field(level, type_name.ptr(),
        str.ptr(), s ? s->db.str : nullptr, s ? s->table_name.str : nullptr,
        field_name.str);
    }
    int set_null_with_warn(const ErrConv &str)
    {
      store_warning(str, Sql_condition::WARN_LEVEL_WARN);
      set_null();
      return 1;
    }
    int set_min_value_with_warn(const ErrConv &str)
    {
      store_warning(str, Sql_condition::WARN_LEVEL_WARN);
      set_min_value((char*) ptr);
      return 1;
    }
    int set_max_value_with_warn(const ErrConv &str)
    {
      store_warning(str, Sql_condition::WARN_LEVEL_WARN);
      set_max_value((char*) ptr);
      return 1;
    }
    int store_fbt_null_with_warn(const Fbt_null &fbt,
                                   const ErrConvString &err)
    {
      DBUG_ASSERT(marked_for_write_or_computed());
      if (fbt.is_null())
        return maybe_null() ? set_null_with_warn(err)
                            : set_min_value_with_warn(err);
      fbt.to_record((char *) ptr, FbtImpl::binary_length());
      return 0;
    }

  public:
    Field_fbt(const LEX_CSTRING *field_name_arg, const Record_addr &rec)
      :Field(rec.ptr(), FbtImpl::max_char_length(),
             rec.null_ptr(), rec.null_bit(), Field::NONE, field_name_arg)
    {
      flags|= BINARY_FLAG | UNSIGNED_FLAG;
    }
    const Type_handler *type_handler() const override
    {
      return singleton();
    }
    uint32 max_display_length() const override { return field_length; }
    bool str_needs_quotes() const override { return true; }
    const DTCollation &dtcollation() const override
    {
      static DTCollation_numeric c;
      return c;
    }
    CHARSET_INFO *charset(void) const override { return &my_charset_numeric; }
    const CHARSET_INFO *sort_charset(void) const override { return &my_charset_bin; }
    /**
      This makes client-server protocol convert the value according
      to @@character_set_client.
    */
    bool binary() const override { return false; }
    enum ha_base_keytype key_type() const override { return HA_KEYTYPE_BINARY; }

    bool is_equal(const Column_definition &new_field) const override
    {
      return new_field.type_handler() == type_handler();
    }
    bool eq_def(const Field *field) const override
    {
      return Field::eq_def(field);
    }
    double pos_in_interval(Field *min, Field *max) override
    {
      return pos_in_interval_val_str(min, max, 0);
    }
    int cmp(const uchar *a, const uchar *b) const override
    { return memcmp(a, b, pack_length()); }

    void sort_string(uchar *to, uint length) override
    {
      DBUG_ASSERT(length == pack_length());
      memcpy(to, ptr, length);
    }
    uint32 pack_length() const override
    {
      return FbtImpl::binary_length();
    }
    uint pack_length_from_metadata(uint field_metadata) const override
    {
      return FbtImpl::binary_length();
    }

    void sql_type(String &str) const override
    {
      static Name name= singleton()->name();
      str.set_ascii(name.ptr(), name.length());
    }

    void make_send_field(Send_field *to) override
    {
      Field::make_send_field(to);
      to->set_data_type_name(singleton()->name().lex_cstring());
    }

    bool validate_value_in_record(THD *thd, const uchar *record) const override
    {
      return false;
    }

    bool val_native(Native *to) override
    {
      DBUG_ASSERT(marked_for_read());
      if (to->alloc(FbtImpl::binary_length()))
        return true;
      to->length(FbtImpl::binary_length());
      FbtImpl::record_to_memory((char*) to->ptr(), (const char*) ptr);
      return false;
    }

    Fbt to_fbt() const
    {
      DBUG_ASSERT(marked_for_read());
      return Fbt::record_to_memory((const char*) ptr);
    }

    String *val_str(String *val_buffer, String *) override
    {
      return to_fbt().to_string(val_buffer) ? NULL : val_buffer;
    }

    my_decimal *val_decimal(my_decimal *to) override
    {
      DBUG_ASSERT(marked_for_read());
      my_decimal_set_zero(to);
      return to;
    }

    longlong val_int() override
    {
      DBUG_ASSERT(marked_for_read());
      return 0;
    }

    double val_real() override
    {
      DBUG_ASSERT(marked_for_read());
      return 0;
    }

    bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override
    {
      DBUG_ASSERT(marked_for_read());
      set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);
      return false;
    }

    bool val_bool(void) override
    {
      DBUG_ASSERT(marked_for_read());
      return !Fbt::only_zero_bytes((const char *) ptr, FbtImpl::binary_length());
    }

    int store_native(const Native &value) override
    {
      DBUG_ASSERT(marked_for_write_or_computed());
      DBUG_ASSERT(value.length() == FbtImpl::binary_length());
      FbtImpl::memory_to_record((char*) ptr, value.ptr());
      return 0;
    }

    int store(const char *str, size_t length, CHARSET_INFO *cs) override
    {
      return cs == &my_charset_bin ? store_binary(str, length)
                                   : store_text(str, length, cs);
    }

    int store_text(const char *str, size_t length, CHARSET_INFO *cs) override
    {
      return store_fbt_null_with_warn(Fbt_null(str, length, cs),
                                        ErrConvString(str, length, cs));
    }

    int store_binary(const char *str, size_t length) override
    {
      return store_fbt_null_with_warn(Fbt_null(str, length),
                                        ErrConvString(str, length,
                                                      &my_charset_bin));
    }

    int store_hex_hybrid(const char *str, size_t length) override
    {
      return Field_fbt::store_binary(str, length);
    }

    int store_decimal(const my_decimal *num) override
    {
      DBUG_ASSERT(marked_for_write_or_computed());
      return set_min_value_with_warn(ErrConvDecimal(num));
    }

    int store(longlong nr, bool unsigned_flag) override
    {
      DBUG_ASSERT(marked_for_write_or_computed());
      return set_min_value_with_warn(
              ErrConvInteger(Longlong_hybrid(nr, unsigned_flag)));
    }

    int store(double nr) override
    {
      DBUG_ASSERT(marked_for_write_or_computed());
      return set_min_value_with_warn(ErrConvDouble(nr));
    }

    int store_time_dec(const MYSQL_TIME *ltime, uint dec) override
    {
      DBUG_ASSERT(marked_for_write_or_computed());
      return set_min_value_with_warn(ErrConvTime(ltime));
    }

    /*** Field conversion routines ***/
    int store_field(Field *from) override
    {
      // INSERT INTO t1 (fbt_field) SELECT different_field_type FROM t2;
      return from->save_in_field(this);
    }
    int save_in_field(Field *to) override
    {
      // INSERT INTO t2 (different_field_type) SELECT fbt_field FROM t1;
      if (to->charset() == &my_charset_bin &&
          dynamic_cast<const Type_handler_general_purpose_string*>
            (to->type_handler()))
      {
        NativeBuffer<FbtImpl::binary_length()+1> res;
        val_native(&res);
        return to->store(res.ptr(), res.length(), &my_charset_bin);
      }
      return save_in_field_str(to);
    }
    Copy_func *get_copy_func(const Field *from) const override
    {
      // ALTER to FBT from another field
      return do_field_string;
    }

    Copy_func *get_copy_func_to(const Field *to) const override
    {
      if (type_handler() == to->type_handler())
      {
        // ALTER from FBT to FBT
        DBUG_ASSERT(pack_length() == to->pack_length());
        DBUG_ASSERT(charset() == to->charset());
        DBUG_ASSERT(sort_charset() == to->sort_charset());
        return Field::do_field_eq;
      }
      // ALTER from FBT to another fbt type
      if (to->charset() == &my_charset_bin &&
          dynamic_cast<const Type_handler_general_purpose_string*>
            (to->type_handler()))
      {
        /*
          ALTER from FBT to a binary string type, e.g.:
            BINARY, TINYBLOB, BLOB, MEDIUMBLOB, LONGBLOB
        */
        return do_field_fbt_native_to_binary;
      }
      return do_field_string;
    }

    static void do_field_fbt_native_to_binary(const Copy_field *copy)
    {
      NativeBuffer<FbtImpl::binary_length()+1> res;
      copy->from_field->val_native(&res);
      copy->to_field->store(res.ptr(), res.length(), &my_charset_bin);
    }

    bool memcpy_field_possible(const Field *from) const override
    {
      // INSERT INTO t1 (fbt_field) SELECT field2 FROM t2;
      return type_handler() == from->type_handler();
    }
    enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                      const Relay_log_info *rli,
                                      const Conv_param &param) const override
    {
      if (type_handler() == source.type_handler() ||
          (source.type_handler() == &type_handler_string &&
           source.type_handler()->max_display_length_for_field(source) ==
           FbtImpl::binary_length()))
        return rpl_conv_type_from_same_data_type(source.metadata(), rli, param);
      return CONV_TYPE_IMPOSSIBLE;
    }

    /*** Optimizer routines ***/
    bool test_if_equality_guarantees_uniqueness(const Item *const_item) const override
    {
      /*
        This condition:
          WHERE fbt_field=const
        should return a single distinct value only,
        as comparison is done according to FBT.
      */
      return true;
    }
    bool can_be_substituted_to_equal_item(const Context &ctx,
                                          const Item_equal *item_equal)
                                          override
    {
      switch (ctx.subst_constraint()) {
      case ANY_SUBST:
        return ctx.compare_type_handler() == item_equal->compare_type_handler();
      case IDENTITY_SUBST:
        return true;
      }
      return false;
    }
    Item *get_equal_const_item(THD *thd, const Context &ctx,
                               Item *const_item) override
    {
      Fbt_null tmp(const_item);
      if (tmp.is_null())
        return NULL;
      return new (thd->mem_root) Item_literal_fbt(thd, tmp);
    }
    Data_type_compatibility can_optimize_keypart_ref(const Item_bool_func *cond,
                                                     const Item *item)
                                                     const override
    {
      /*
        Mixing of two different non-traditional types is currently prevented.
        This may change in the future.
      */
      DBUG_ASSERT(item->type_handler()->type_handler_base_or_self()->
                  is_traditional_scalar_type() ||
                  item->type_handler()->type_collection() ==
                  type_handler()->type_collection());
      return Data_type_compatibility::OK;
    }
    /**
      Test if Field can use range optimizer for a standard comparison operation:
        <=, <, =, <=>, >, >=
      Note, this method does not cover spatial operations.
    */
    Data_type_compatibility can_optimize_range(const Item_bool_func *cond,
                                               const Item *item,
                                               bool is_eq_func) const override
    {
      // See the DBUG_ASSERT comment in can_optimize_keypart_ref()
      DBUG_ASSERT(item->type_handler()->type_handler_base_or_self()->
                  is_traditional_scalar_type() ||
                  item->type_handler()->type_collection() ==
                  type_handler()->type_collection());
      return Data_type_compatibility::OK;
    }
    void hash_not_null(Hasher *hasher) override
    {
      FbtImpl::hash_record(ptr, hasher);
    }
    SEL_ARG *get_mm_leaf(RANGE_OPT_PARAM *prm, KEY_PART *key_part,
                         const Item_bool_func *cond,
                         scalar_comparison_op op, Item *value) override
    {
      DBUG_ENTER("Field_fbt::get_mm_leaf");
      if (can_optimize_scalar_range(prm, key_part, cond, op, value) !=
          Data_type_compatibility::OK)
        DBUG_RETURN(0);
      int err= value->save_in_field_no_warnings(this, 1);
      if ((op != SCALAR_CMP_EQUAL && is_real_null()) || err < 0)
        DBUG_RETURN(&null_element);
      if (err > 0)
      {
        if (op == SCALAR_CMP_EQ || op == SCALAR_CMP_EQUAL)
          DBUG_RETURN(new (prm->mem_root) SEL_ARG_IMPOSSIBLE(this));
        DBUG_RETURN(NULL); /*  Cannot infer anything */
      }
      DBUG_RETURN(stored_field_make_mm_leaf(prm, key_part, op, value));
    }
    Data_type_compatibility can_optimize_hash_join(const Item_bool_func *cond,
                                                   const Item *item)
                                                   const override
    {
      return can_optimize_keypart_ref(cond, item);
    }
    Data_type_compatibility can_optimize_group_min_max(
                                    const Item_bool_func *cond,
                                    const Item *const_item) const override
    {
      return Data_type_compatibility::OK;
    }

    uint row_pack_length() const override { return pack_length(); }

    Binlog_type_info binlog_type_info() const override
    {
      DBUG_ASSERT(type() == binlog_type());
      return Binlog_type_info_fixed_string(Field_fbt::binlog_type(),
                                           FbtImpl::binary_length(), &my_charset_bin);
    }

    uchar *pack(uchar *to, const uchar *from, uint max_length) override
    {
      DBUG_PRINT("debug", ("Packing field '%s'", field_name.str));
      return FbtImpl::pack(to, from, max_length);
    }

    const uchar *unpack(uchar *to, const uchar *from, const uchar *from_end,
                        uint param_data) override
    {
      return FbtImpl::unpack(to, from, from_end, param_data);
    }

    uint max_packed_col_length(uint max_length) override
    {
      return StringPack::max_packed_col_length(max_length);
    }

    uint packed_col_length(const uchar *fbt_ptr, uint length) override
    {
      return StringPack::packed_col_length(fbt_ptr, length);
    }

    uint size_of() const override { return sizeof(*this); }
  };


  class cmp_item_fbt: public cmp_item_scalar
  {
    Fbt m_native;
  public:
    cmp_item_fbt()
     :cmp_item_scalar(),
      m_native(Fbt::zero())
    { }
    void store_value(Item *item) override
    {
      m_native= Fbt(item, &m_null_value);
    }
    int cmp_not_null(const Value *val) override
    {
      DBUG_ASSERT(!val->is_null());
      DBUG_ASSERT(val->is_string());
      Fbt_null tmp(val->m_string);
      DBUG_ASSERT(!tmp.is_null());
      return m_native.cmp(tmp);
    }
    int cmp(Item *arg) override
    {
      Fbt_null tmp(arg);
      return m_null_value || tmp.is_null() ? UNKNOWN : m_native.cmp(tmp) != 0;
    }
    int compare(cmp_item *ci) override
    {
      cmp_item_fbt *tmp= static_cast<cmp_item_fbt*>(ci);
      DBUG_ASSERT(!m_null_value);
      DBUG_ASSERT(!tmp->m_null_value);
      return m_native.cmp(tmp->m_native);
    }
    cmp_item *make_same(THD *thd) override
    {
      return new (thd->mem_root) cmp_item_fbt();
    }
  };

  class in_fbt :public in_vector
  {
    Fbt m_value;
    static int cmp_fbt(void *cmp_arg, Fbt *a, Fbt *b)
    {
      return a->cmp(*b);
    }
  public:
    in_fbt(THD *thd, uint elements)
     :in_vector(thd, elements, sizeof(Fbt), (qsort2_cmp) cmp_fbt, 0),
      m_value(Fbt::zero())
    { }
    const Type_handler *type_handler() const override
    {
      return singleton();
    }
    bool set(uint pos, Item *item) override
    {
      Fbt *buff= &((Fbt *) base)[pos];
      Fbt_null value(item);
      if (value.is_null())
      {
        *buff= Fbt::zero();
        return true;
      }
      *buff= value;
      return false;
    }
    uchar *get_value(Item *item) override
    {
      Fbt_null value(item);
      if (value.is_null())
        return 0;
      m_value= value;
      return (uchar *) &m_value;
    }
    Item* create_item(THD *thd) override
    {
      return new (thd->mem_root) Item_literal_fbt(thd);
    }
    void value_to_item(uint pos, Item *item) override
    {
      const Fbt &buff= (((Fbt*) base)[pos]);
      static_cast<Item_literal_fbt*>(item)->set_value(buff);
    }
  };

  class Item_copy_fbt: public Item_copy
  {
    NativeBuffer<Fbt::binary_length()+1> m_value;
  public:
    Item_copy_fbt(THD *thd, Item *item_arg): Item_copy(thd, item_arg) {}

    bool val_native(THD *thd, Native *to) override
    {
      if (null_value)
        return true;
      return to->copy(m_value.ptr(), m_value.length());
    }
    String *val_str(String *to) override
    {
      if (null_value)
        return NULL;
      Fbt_null tmp(m_value.ptr(), m_value.length());
      return tmp.is_null() || tmp.to_string(to) ? NULL : to;
    }
    my_decimal *val_decimal(my_decimal *to) override
    {
      my_decimal_set_zero(to);
      return to;
    }
    double val_real() override
    {
      return 0;
    }
    longlong val_int() override
    {
      return 0;
    }
    bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
    {
      set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);
      return null_value;
    }
    void copy() override
    {
      null_value= item->val_native(current_thd, &m_value);
      DBUG_ASSERT(null_value == item->null_value);
    }
    int save_in_field(Field *field, bool no_conversions) override
    {
      return Item::save_in_field(field, no_conversions);
    }
    Item *do_get_copy(THD *thd) const override
    { return get_item_copy<Item_copy_fbt>(thd, this); }
    Item *do_build_clone(THD *thd) const override { return get_copy(thd); }
  };

  class Item_char_typecast_func_handler_fbt_to_binary:
                                         public Item_handled_func::Handler_str
  {
  public:
    const Type_handler *return_type_handler(const Item_handled_func *item)
                                            const override
    {
      if (item->max_length > MAX_FIELD_VARCHARLENGTH)
        return Type_handler::blob_type_handler(item->max_length);
      if (item->max_length > 255)
        return &type_handler_varchar;
      return &type_handler_string;
    }
    bool fix_length_and_dec(Item_handled_func *xitem) const override
    {
      return false;
    }
    String *val_str(Item_handled_func *item, String *to) const override
    {
      DBUG_ASSERT(dynamic_cast<const Item_char_typecast*>(item));
      return static_cast<Item_char_typecast*>(item)->
               val_str_binary_from_native(to);
    }
  };

  class Item_typecast_fbt: public Item_func
  {
  public:
    Item_typecast_fbt(THD *thd, Item *a) :Item_func(thd, a) {}

    const Type_handler *type_handler() const override
    { return singleton(); }

    enum Functype functype() const override { return CHAR_TYPECAST_FUNC; }
    bool eq(const Item *item, bool binary_cmp) const override
    {
      if (this == item)
        return true;
      if (item->type() != FUNC_ITEM ||
          functype() != ((Item_func*)item)->functype())
        return false;
      if (type_handler() != item->type_handler())
        return false;
      Item_typecast_fbt *cast= (Item_typecast_fbt*) item;
      return args[0]->eq(cast->args[0], binary_cmp);
    }
    LEX_CSTRING func_name_cstring() const override
    {
      static Name name= singleton()->name();
      size_t len= 9+name.length()+1;
      char *buf= (char*)current_thd->alloc(len);
      strmov(strmov(buf, "cast_as_"), name.ptr());
      return { buf, len };
    }
    void print(String *str, enum_query_type query_type) override
    {
      str->append(STRING_WITH_LEN("cast("));
      args[0]->print(str, query_type);
      str->append(STRING_WITH_LEN(" as "));
      str->append(singleton()->name().lex_cstring());
      str->append(')');
    }
    bool fix_length_and_dec(THD *thd) override
    {
      Type_std_attributes::operator=(Type_std_attributes_fbt());
      if (Fbt::fix_fields_maybe_null_on_conversion_to_fbt(args[0]))
        set_maybe_null();
      return false;
    }
    String *val_str(String *to) override
    {
      Fbt_null tmp(args[0]);
      return (null_value= tmp.is_null() || tmp.to_string(to)) ? NULL : to;
    }
    longlong val_int() override
    {
      return 0;
    }
    double val_real() override
    {
      return 0;
    }
    my_decimal *val_decimal(my_decimal *to) override
    {
      my_decimal_set_zero(to);
      return to;
    }
    bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
    {
      set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);
      return false;
    }
    bool val_native(THD *thd, Native *to) override
    {
      Fbt_null tmp(args[0]);
      return null_value= tmp.is_null() || tmp.to_native(to);
    }
    Item *do_get_copy(THD *thd) const override
    { return get_item_copy<Item_typecast_fbt>(thd, this); }
  };

  class Item_cache_fbt: public Item_cache
  {
    NativeBuffer<FbtImpl::binary_length()+1> m_value;
  public:
    Item_cache_fbt(THD *thd)
     :Item_cache(thd, singleton()) { }
    Item *do_get_copy(THD *thd) const override
    { return get_item_copy<Item_cache_fbt>(thd, this); }
    Item *do_build_clone(THD *thd) const override { return get_copy(thd); }
    bool cache_value() override
    {
      if (!example)
        return false;
      value_cached= true;
      null_value_inside= null_value=
        example->val_native_with_conversion_result(current_thd,
                                                   &m_value, type_handler());
      return true;
    }
    String* val_str(String *to) override
    {
      if (!has_value())
        return NULL;
      Fbt_null tmp(m_value.ptr(), m_value.length());
      return tmp.is_null() || tmp.to_string(to) ? NULL : to;
    }
    my_decimal *val_decimal(my_decimal *to) override
    {
      if (!has_value())
        return NULL;
      my_decimal_set_zero(to);
      return to;
    }
    longlong val_int() override
    {
      if (!has_value())
        return 0;
      return 0;
    }
    double val_real() override
    {
      if (!has_value())
        return 0;
      return 0;
    }
    longlong val_datetime_packed(THD *) override
    {
      DBUG_ASSERT(0);
      if (!has_value())
        return 0;
      return 0;
    }
    longlong val_time_packed(THD *) override
    {
      DBUG_ASSERT(0);
      if (!has_value())
        return 0;
      return 0;
    }
    bool get_date(THD *, MYSQL_TIME *ltime, date_mode_t) override
    {
      if (!has_value())
        return true;
      set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);
      return false;
    }
    bool val_native(THD *, Native *to) override
    {
      if (!has_value())
        return true;
      return to->copy(m_value.ptr(), m_value.length());
    }
  };

  /* =[ methods ]=============================================== */
private:

  bool character_or_binary_string_to_native(THD *thd, const String *str,
                                            Native *to) const
  {
    if (str->charset() == &my_charset_bin)
    {
      // Convert from a binary string
      if (str->length() != FbtImpl::binary_length() ||
          to->copy(str->ptr(), str->length()))
      {
        thd->push_warning_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                                      name().ptr(), ErrConvString(str).ptr());
        return true;
      }
      return false;
    }
    // Convert from a character string
    Fbt_null tmp(*str);
    if (tmp.is_null())
      thd->push_warning_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                                    name().ptr(), ErrConvString(str).ptr());
    return tmp.is_null() || tmp.to_native(to);
  }

public:
  ~Type_handler_fbt() override {}

  const Type_collection *type_collection() const override
  {
    return TypeCollectionImpl::singleton();
  }

  const Name &default_value() const override
  {
    return FbtImpl::default_value();
  }
  ulong KEY_pack_flags(uint column_nr) const override
  {
    return FbtImpl::KEY_pack_flags(column_nr);
  }
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_STRING;
  }
  bool Item_append_extended_type_info(Send_field_extended_metadata *to,
                                      const Item *item) const override
  {
    return to->set_data_type_name(name().lex_cstring());
  }

  enum_field_types field_type() const override
  {
    return MYSQL_TYPE_STRING;
  }

  Item_result result_type() const override
  {
    return STRING_RESULT;
  }

  Item_result cmp_type() const override
  {
    return STRING_RESULT;
  }

  enum_dynamic_column_type dyncol_type(const Type_all_attributes *attr)
                                       const override
  {
    return DYN_COL_STRING;
  }

  uint32 max_display_length_for_field(const Conv_source &src) const override
  {
    return FbtImpl::max_char_length();
  }

  const Type_handler *type_handler_for_implicit_upgrade() const override
  {
    return TypeCollectionImpl::singleton()->
             type_handler_for_implicit_upgrade(this);
  }
  const Type_handler *type_handler_for_comparison() const override
  {
    return this;
  }

  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item) const override
  {
    DBUG_ASSERT(field->type_handler() == this);
    Fbt_null ni(item); // Convert Item to Fbt
    if (ni.is_null())
      return 0;
    NativeBuffer<FbtImpl::binary_length()+1> tmp;
    if (field->val_native(&tmp))
    {
      DBUG_ASSERT(0);
      return 0;
    }
    return -ni.cmp(tmp);
  }
  CHARSET_INFO *charset_for_protocol(const Item *item) const override
  {
    return item->collation.collation;
  }

  bool is_scalar_type() const override { return true; }
  bool is_val_native_ready() const override { return true; }
  bool can_return_int() const override { return false; }
  bool can_return_decimal() const override { return false; }
  bool can_return_real() const override { return false; }
  bool can_return_str() const override { return true; }
  bool can_return_text() const override { return true; }
  bool can_return_date() const override { return false; }
  bool can_return_time() const override { return false; }
  bool convert_to_binary_using_val_native() const override { return true; }

  decimal_digits_t  Item_time_precision(THD *thd, Item *item) const override
  {
    return 0;
  }
  decimal_digits_t Item_datetime_precision(THD *thd, Item *item) const override
  {
    return 0;
  }
  decimal_digits_t Item_decimal_scale(const Item *item) const override
  {
    return 0;
  }
  decimal_digits_t  Item_decimal_precision(const Item *item) const override
  {
    /* This will be needed if we ever allow cast from Fbt to DECIMAL. */
    return (FbtImpl::binary_length()*8+7)/10*3; // = bytes to decimal digits
  }

  /*
    Returns how many digits a divisor adds into a division result.
    See Item::divisor_precision_increment() in item.h for more comments.
  */
  decimal_digits_t Item_divisor_precision_increment(const Item *) const override
  {
    return 0;
  }
  /**
    Makes a temporary table Field to handle numeric aggregate functions,
    e.g. SUM(DISTINCT expr), AVG(DISTINCT expr), etc.
  */
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }
  Field *make_conversion_table_field(MEM_ROOT *root, TABLE *table, uint metadata,
                                     const Field *target) const override
  {
    const Record_addr tmp(NULL, Bit_addr(true));
    return new (table->in_use->mem_root) Field_fbt(&empty_clex_str, tmp);
  }
  // Fix attributes after the parser
  bool Column_definition_fix_attributes(Column_definition *c) const override
  {
    c->length= FbtImpl::max_char_length();
    return false;
  }

  bool Column_definition_prepare_stage1(THD *, MEM_ROOT *,
                                        Column_definition *def,
                                        column_definition_type_t,
                                        const Column_derived_attributes *)
                                        const override
  {
    def->prepare_stage1_simple(&my_charset_numeric);
    return false;
  }

  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file) const override
  {
    def->redefine_stage1_common(dup, file);
    def->set_compression_method(dup->compression_method());
    def->create_length_to_internal_length_string();
    return false;
  }

  bool Column_definition_prepare_stage2(Column_definition *def, handler *file,
                                        ulonglong table_flags) const override
  {
    def->pack_flag= FIELDFLAG_BINARY;
    return false;
  }

  bool partition_field_check(const LEX_CSTRING &field_name,
                             Item *item_expr) const override
  {
    if (item_expr->cmp_type() != STRING_RESULT)
    {
      my_error(ER_WRONG_TYPE_COLUMN_VALUE_ERROR, MYF(0));
      return true;
    }
    return false;
  }

  bool partition_field_append_value(String *to, Item *item_expr,
                                    CHARSET_INFO *field_cs,
                                    partition_value_print_mode_t mode)
                                    const override
  {
    StringBuffer<FbtImpl::max_char_length()+64> fbtstr;
    Fbt_null fbt(item_expr);
    if (fbt.is_null())
    {
      my_error(ER_PARTITION_FUNCTION_IS_NOT_ALLOWED, MYF(0));
      return true;
    }
    return fbt.to_string(&fbtstr) ||
           to->append('\'') ||
           to->append(fbtstr) ||
           to->append('\'');
  }

  Field *make_table_field(MEM_ROOT *root, const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *table) const override
  {
    return new (root) Field_fbt(name, addr);
  }

  Field * make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name, const Record_addr &addr,
                            const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const override
  {
    return new (mem_root) Field_fbt(name, addr);
  }
  void Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const override
  {
    def->frm_pack_basic(buff);
    def->frm_pack_charset(buff);
  }
  bool Column_definition_attributes_frm_unpack(Column_definition_attributes *def,
                                          TABLE_SHARE *share, const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const override
  {
    def->frm_unpack_basic(buffer);
    return def->frm_unpack_charset(share, buffer);
  }
  void make_sort_key_part(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                          String *) const override
  {
    DBUG_ASSERT(item->type_handler() == this);
    NativeBuffer<FbtImpl::binary_length()+1> tmp;
    item->val_native_result(current_thd, &tmp);
    if (item->maybe_null())
    {
      if (item->null_value)
      {
        memset(to, 0, FbtImpl::binary_length() + 1);
        return;
      }
      *to++= 1;
    }
    DBUG_ASSERT(!item->null_value);
    DBUG_ASSERT(FbtImpl::binary_length() == tmp.length());
    DBUG_ASSERT(FbtImpl::binary_length() == sort_field->length);
    FbtImpl::memory_to_record((char*) to, tmp.ptr());
  }
  uint make_packed_sort_key_part(uchar *to, Item *item,
                                 const SORT_FIELD_ATTR *sort_field,
                                 String *) const override
  {
    DBUG_ASSERT(item->type_handler() == this);
    NativeBuffer<FbtImpl::binary_length()+1> tmp;
    item->val_native_result(current_thd, &tmp);
    if (item->maybe_null())
    {
      if (item->null_value)
      {
        *to++=0;
        return 0;
      }
      *to++= 1;
    }
    DBUG_ASSERT(!item->null_value);
    DBUG_ASSERT(FbtImpl::binary_length() == tmp.length());
    DBUG_ASSERT(FbtImpl::binary_length() == sort_field->length);
    FbtImpl::memory_to_record((char*) to, tmp.ptr());
    return tmp.length();
  }
  void sort_length(THD *thd, const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const override
  {
    attr->original_length= attr->length= FbtImpl::binary_length();
    attr->suffix_length= 0;
  }
  uint32 max_display_length(const Item *item) const override
  {
    return FbtImpl::max_char_length();
  }
  uint32 calc_pack_length(uint32 length) const override
  {
    return FbtImpl::binary_length();
  }
  void Item_update_null_value(Item *item) const override
  {
    NativeBuffer<FbtImpl::binary_length()+1> tmp;
    item->val_native(current_thd, &tmp);
  }
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const override
  {
    value->m_type= DYN_COL_STRING;
    String *str= item->val_str(&value->m_string);
    if (str != &value->m_string && !item->null_value)
    {
      // "item" returned a non-NULL value
      if (Fbt_null(*str).is_null())
      {
        /*
          The value was not-null, but conversion to FBT failed:
            SELECT a, DECODE_ORACLE(fbtcol, 'garbage', '<NULL>', '::01', '01')
            FROM t1;
        */
        thd->push_warning_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                                      name().ptr(), ErrConvString(str).ptr());
        value->m_type= DYN_COL_NULL;
        return true;
      }
      // "item" returned a non-NULL value, and it was a valid FBT
      value->m_string.set(str->ptr(), str->length(), str->charset());
    }
    return check_null(item, value);
  }
  void Item_param_setup_conversion(THD *thd, Item_param *param) const override
  {
    param->setup_conversion_string(thd, thd->variables.character_set_client);
  }
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override
  {
    param->set_param_str(pos, len);
  }
  bool Item_param_set_from_value(THD *thd, Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *val) const override
  {
    param->unsigned_flag= false;
    param->setup_conversion_string(thd, attr->collation.collation);
    /*
      Exact value of max_length is not known unless fbt is converted to
      charset of connection, so we have to set it later.
    */
    return param->set_str(val->m_string.ptr(), val->m_string.length(),
                          attr->collation.collation,
                          attr->collation.collation);
  }
  bool Item_param_val_native(THD *thd, Item_param *item, Native *to)
                             const override
  {
    StringBuffer<FbtImpl::max_char_length()+1> buffer;
    String *str= item->val_str(&buffer);
    if (!str)
      return true;
    Fbt_null tmp(*str);
    return tmp.is_null() || tmp.to_native(to);
  }
  bool Item_send(Item *item, Protocol *p, st_value *buf) const override
  {
    return Item_send_str(item, p, buf);
  }
  int Item_save_in_field(Item *item, Field *field, bool no_conversions)
                         const override
  {
    if (field->type_handler() == this)
    {
      NativeBuffer<MAX_FIELD_WIDTH> tmp;
      bool rc= item->val_native(current_thd, &tmp);
      if (rc || item->null_value)
        return set_field_to_null_with_conversions(field, no_conversions);
      field->set_notnull();
      return field->store_native(tmp);
    }
    return item->save_str_in_field(field, no_conversions);
  }

  String *print_item_value(THD *thd, Item *item, String *str) const override
  {
    StringBuffer<FbtImpl::max_char_length()+64> buf;
    String *result= item->val_str(&buf);
    /*
      TODO: This should eventually use one of these notations:
      1. CAST('xxx' AS Fbt)
         Problem: CAST is not supported as a NAME_CONST() argument.
      2. Fbt'xxx'
         Problem: This syntax is not supported by the parser yet.
    */
    return !result || str->realloc(result->length() + 2) ||
           str->append(STRING_WITH_LEN("'")) ||
           str->append(result->ptr(), result->length()) ||
           str->append(STRING_WITH_LEN("'")) ? nullptr : str;
  }

  /**
    Check if
      WHERE expr=value AND expr=const
    can be rewritten as:
      WHERE const=value AND expr=const

    "this" is the comparison handler that is used by "target".

    @param target       - the predicate expr=value,
                          whose "expr" argument will be replaced to "const".
    @param target_expr  - the target's "expr" which will be replaced to "const".
    @param target_value - the target's second argument, it will remain unchanged.
    @param source       - the equality predicate expr=const (or expr<=>const)
                          that can be used to rewrite the "target" part
                          (under certain conditions, see the code).
    @param source_expr  - the source's "expr". It should be exactly equal to
                          the target's "expr" to make condition rewrite possible.
    @param source_const - the source's "const" argument, it will be inserted
                          into "target" instead of "expr".
  */
  bool can_change_cond_ref_to_const(Item_bool_func2 *target, Item *target_expr,
                               Item *target_value, Item_bool_func2 *source,
                               Item *source_expr, Item *source_const)
                               const override
  {
    /*
      WHERE COALESCE(col)='xxx' AND COALESCE(col)=CONCAT(a);  -->
      WHERE COALESCE(col)='xxx' AND         'xxx'=CONCAT(a);
    */
    return target->compare_type_handler() == source->compare_type_handler();
  }
  bool subquery_type_allows_materialization(const Item *inner,
                                       const Item *outer, bool) const override
  {
    /*
      Example:
        SELECT * FROM t1 WHERE a IN (SELECT col FROM t1 GROUP BY col);
      Allow materialization only if the outer column is also FBT.
      This can be changed for more relaxed rules in the future.
    */
    DBUG_ASSERT(inner->type_handler() == this);
    return outer->type_handler() == this;
  }
  /**
    Make a simple constant replacement item for a constant "src",
    so the new item can futher be used for comparison with "cmp", e.g.:
      src = cmp   ->  replacement = cmp

    "this" is the type handler that is used to compare "src" and "cmp".

    @param thd - current thread, for mem_root
    @param src - The item that we want to replace. It's a const item,
                 but it can be complex enough to calculate on every row.
    @param cmp - The src's comparand.
    @retval    - a pointer to the created replacement Item
    @retval    - NULL, if could not create a replacement (e.g. on EOM).
                 NULL is also returned for ROWs, because instead of replacing
                 a Item_row to a new Item_row, Type_handler_row just replaces
                 its elements.
  */
  Item *make_const_item_for_comparison(THD *thd, Item *src,
                                       const Item *cmp) const override
  {
    Fbt_null tmp(src);
    if (tmp.is_null())
      return new (thd->mem_root) Item_null(thd, src->name.str);
    return new (thd->mem_root) Item_literal_fbt(thd, tmp);
  }
  Item_cache *Item_get_cache(THD *thd, const Item *item) const override
  {
    return new (thd->mem_root) Item_cache_fbt(thd);
  }

  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const override
  {
    return new (thd->mem_root) Item_typecast_fbt(thd, item);
  }
  Item_copy *create_item_copy(THD *thd, Item *item) const override
  {
    return new (thd->mem_root) Item_copy_fbt(thd, item);
  }
  int cmp_native(const Native &a, const Native &b) const override
  {
    return FbtImpl::cmp(a.to_lex_cstring(), b.to_lex_cstring());
  }
  bool set_comparator_func(THD *thd, Arg_comparator *cmp) const override
  {
    return cmp->set_cmp_func_native(thd);
  }
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                             bool binary_cmp) const override
  {
    return false;
  }
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const override
  {
    Fbt_null na(a), nb(b);
    return !na.is_null() && !nb.is_null() && !na.cmp(nb);
  }
  bool Item_bool_rowready_func2_fix_length_and_dec(THD *thd,
                                 Item_bool_rowready_func2 *func) const override
  {
    if (Type_handler::Item_bool_rowready_func2_fix_length_and_dec(thd, func))
      return true;
    if (!func->maybe_null() &&
        Fbt::fix_fields_maybe_null_on_conversion_to_fbt(func->arguments(), 2))
      func->set_maybe_null();
    return false;
  }
  bool Item_hybrid_func_fix_attributes(THD *thd, const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *h,
                                       Type_all_attributes *attr,
                                       Item **items, uint nitems) const override
  {
    attr->Type_std_attributes::operator=(Type_std_attributes_fbt());
    h->set_handler(this);
    /*
      If some of the arguments cannot be safely converted to "FBT NOT NULL",
      then mark the entire function nullability as NULL-able.
      Otherwise, keep the generic nullability calculated by earlier stages:
      - either by the most generic way in Item_func::fix_fields()
      - or by Item_func_xxx::fix_length_and_dec() before the call of
        Item_hybrid_func_fix_attributes()
      IFNULL() is special. It does not need to test args[0].
    */
    uint first= dynamic_cast<Item_func_ifnull*>(attr) ? 1 : 0;
    for (uint i= first; i < nitems; i++)
    {
      if (Fbt::fix_fields_maybe_null_on_conversion_to_fbt(items[i]))
      {
        attr->set_type_maybe_null(true);
        break;
      }
    }
    return false;
  }
  bool Item_func_min_max_fix_attributes(THD *thd, Item_func_min_max *func,
                                        Item **items, uint nitems) const override
  {
    return Item_hybrid_func_fix_attributes(thd, func->func_name_cstring(),
                                           func, func, items, nitems);

  }
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const override
  {
    func->Type_std_attributes::operator=(Type_std_attributes_fbt());
    func->set_handler(this);
    return false;
  }
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }

  bool Item_val_native_with_conversion(THD *thd, Item *item,
                                       Native *to) const override
  {
    if (item->type_handler() == this)
      return item->val_native(thd, to); // No conversion needed
    StringBuffer<FbtImpl::max_char_length()+1> buffer;
    String *str= item->val_str(&buffer);
    return str ? character_or_binary_string_to_native(thd, str, to) : true;
  }
  bool Item_val_native_with_conversion_result(THD *thd, Item *item,
                                              Native *to) const override
  {
    if (item->type_handler() == this)
      return item->val_native_result(thd, to); // No conversion needed
    StringBuffer<FbtImpl::max_char_length()+1> buffer;
    String *str= item->str_result(&buffer);
    return str ? character_or_binary_string_to_native(thd, str, to) : true;
  }

  bool Item_val_bool(Item *item) const override
  {
    NativeBuffer<FbtImpl::binary_length()+1> tmp;
    if (item->val_native(current_thd, &tmp))
      return false;
    return !Fbt::only_zero_bytes(tmp.ptr(), tmp.length());
  }
  void Item_get_date(THD *thd, Item *item, Temporal::Warn *buff,
                     MYSQL_TIME *ltime, date_mode_t fuzzydate) const override
  {
    set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);
  }

  longlong Item_val_int_signed_typecast(Item *item) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }

  longlong Item_val_int_unsigned_typecast(Item *item) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }

  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str)
                                      const override
  {
    NativeBuffer<FbtImpl::binary_length()+1> tmp;
    if ((item->null_value= item->arguments()[0]->val_native(current_thd, &tmp)))
      return nullptr;
    DBUG_ASSERT(tmp.length() == FbtImpl::binary_length());
    if (str->set_hex(tmp.ptr(), tmp.length()))
    {
      str->length(0);
      str->set_charset(item->collation.collation);
    }
    return str;
  }

  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *item,
                                              String *str) const override
  {
    NativeBuffer<FbtImpl::binary_length()+1> native;
    if (item->val_native(current_thd, &native))
    {
      DBUG_ASSERT(item->null_value);
      return nullptr;
    }
    DBUG_ASSERT(native.length() == FbtImpl::binary_length());
    Fbt_null tmp(native.ptr(), native.length());
    return tmp.is_null() || tmp.to_string(str) ? nullptr : str;
  }
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const override
  {
    return 0;
  }
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const override
  {
    return 0;
  }
  my_decimal *
  Item_func_hybrid_field_type_val_decimal(Item_func_hybrid_field_type *,
                                          my_decimal *to) const override
  {
    my_decimal_set_zero(to);
    return to;
  }
  void Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            Temporal::Warn *,
                                            MYSQL_TIME *to,
                                            date_mode_t fuzzydate)
                                            const override
  {
    set_zero_time(to, MYSQL_TIMESTAMP_TIME);
  }
  // WHERE is Item_func_min_max_val_native???
  String *Item_func_min_max_val_str(Item_func_min_max *func, String *str)
                                    const override
  {
    Fbt_null tmp(func);
    return tmp.is_null() || tmp.to_string(str) ? nullptr : str;
  }
  double Item_func_min_max_val_real(Item_func_min_max *) const override
  {
    return 0;
  }
  longlong Item_func_min_max_val_int(Item_func_min_max *) const override
  {
    return 0;
  }
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *to) const override
  {
    my_decimal_set_zero(to);
    return to;
  }
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*, MYSQL_TIME *to,
                                  date_mode_t fuzzydate) const override
  {
    set_zero_time(to, MYSQL_TIMESTAMP_TIME);
    return false;
  }

  bool Item_func_between_fix_length_and_dec(Item_func_between *func) const override
  {
    if (!func->maybe_null() &&
        Fbt::fix_fields_maybe_null_on_conversion_to_fbt(func->arguments(), 3))
      func->set_maybe_null();
    return false;
  }
  longlong Item_func_between_val_int(Item_func_between *func) const override
  {
    return func->val_int_cmp_native();
  }

  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const override
  {
    return new (thd->mem_root) cmp_item_fbt;
  }

  in_vector *make_in_vector(THD *thd, const Item_func_in *func,
                            uint nargs) const override
  {
    return new (thd->mem_root) in_fbt(thd, nargs);
  }

  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *func)
                                                    const override
  {
    if (!func->maybe_null() &&
        Fbt::fix_fields_maybe_null_on_conversion_to_fbt(func->arguments(),
                                                        func->argument_count()))
      func->set_maybe_null();
    if (func->compatible_types_scalar_bisection_possible())
    {
      return func->value_list_convert_const_to_int(thd) ||
             func->fix_for_scalar_comparison_using_bisection(thd);
    }
    return
      func->fix_for_scalar_comparison_using_cmp_items(thd,
                                                      1U << (uint) STRING_RESULT);
  }
  bool Item_func_round_fix_length_and_dec(Item_func_round *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }

  bool Item_func_abs_fix_length_and_dec(Item_func_abs *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }

  bool Item_func_neg_fix_length_and_dec(Item_func_neg *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }

  bool Item_func_signed_fix_length_and_dec(Item_func_signed *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool Item_double_typecast_fix_length_and_dec(Item_double_typecast *item)
                                          const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool Item_float_typecast_fix_length_and_dec(Item_float_typecast *item)
                                         const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *item)
                                           const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool Item_char_typecast_fix_length_and_dec(Item_char_typecast *item)
                                             const override
  {
    if (item->cast_charset() == &my_charset_bin)
    {
      static Item_char_typecast_func_handler_fbt_to_binary
               item_char_typecast_func_handler_fbt_to_binary;
      item->fix_length_and_dec_native_to_binary(FbtImpl::binary_length());
      item->set_func_handler(&item_char_typecast_func_handler_fbt_to_binary);
      return false;
    }
    item->fix_length_and_dec_str();
    return false;
  }

  bool Item_time_typecast_fix_length_and_dec(Item_time_typecast *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool Item_date_typecast_fix_length_and_dec(Item_date_typecast *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *item)
                                            const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool Item_func_plus_fix_length_and_dec(Item_func_plus *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool Item_func_div_fix_length_and_dec(Item_func_div *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }

  static Type_handler_fbt *singleton()
  {
    static Type_handler_fbt th;
    return &th;
  }
};

template<class FbtImpl>
class Type_collection_fbt: public Type_collection
{
  const Type_handler *aggregate_common(const Type_handler *a,
                                       const Type_handler *b) const
  {
    if (a == b)
      return a;
    return NULL;
  }
  const Type_handler *aggregate_if_string(const Type_handler *a,
                                          const Type_handler *b) const
  {
    static const Type_aggregator::Pair agg[]=
    {
      {Type_handler_fbt<FbtImpl>::singleton(), &type_handler_null,        Type_handler_fbt<FbtImpl>::singleton()},
      {Type_handler_fbt<FbtImpl>::singleton(), &type_handler_varchar,     Type_handler_fbt<FbtImpl>::singleton()},
      {Type_handler_fbt<FbtImpl>::singleton(), &type_handler_string,      Type_handler_fbt<FbtImpl>::singleton()},
      {Type_handler_fbt<FbtImpl>::singleton(), &type_handler_tiny_blob,   Type_handler_fbt<FbtImpl>::singleton()},
      {Type_handler_fbt<FbtImpl>::singleton(), &type_handler_blob,        Type_handler_fbt<FbtImpl>::singleton()},
      {Type_handler_fbt<FbtImpl>::singleton(), &type_handler_medium_blob, Type_handler_fbt<FbtImpl>::singleton()},
      {Type_handler_fbt<FbtImpl>::singleton(), &type_handler_long_blob,   Type_handler_fbt<FbtImpl>::singleton()},
      {Type_handler_fbt<FbtImpl>::singleton(), &type_handler_hex_hybrid,  Type_handler_fbt<FbtImpl>::singleton()},
      {NULL,NULL,NULL}
    };
    return Type_aggregator::find_handler_in_array(agg, a, b, true);
  }
public:
  const Type_handler *aggregate_for_result(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    const Type_handler *h;
    if ((h= aggregate_common(a, b)) || (h= aggregate_if_string(a, b)))
      return h;
    return NULL;
  }

  const Type_handler *aggregate_for_min_max(const Type_handler *a,
                                            const Type_handler *b)
                                            const override
  {
    return aggregate_for_result(a, b);
  }

  const Type_handler *aggregate_for_comparison(const Type_handler *a,
                                               const Type_handler *b)
                                               const override
  {
    if (const Type_handler *h= aggregate_common(a, b))
      return h;
    static const Type_aggregator::Pair agg[]=
    {
      {Type_handler_fbt<FbtImpl>::singleton(), &type_handler_null,      Type_handler_fbt<FbtImpl>::singleton()},
      {Type_handler_fbt<FbtImpl>::singleton(), &type_handler_long_blob, Type_handler_fbt<FbtImpl>::singleton()},
      {NULL,NULL,NULL}
    };
    return Type_aggregator::find_handler_in_array(agg, a, b, true);
  }

  const Type_handler *aggregate_for_num_op(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    return NULL;
  }

  const Type_handler *type_handler_for_implicit_upgrade(
                                               const Type_handler *from) const
  {
    return from;
  }

  static Type_collection_fbt *singleton()
  {
    static Type_collection_fbt tc;
    return &tc;
  }
};

#endif /* SQL_TYPE_FIXEDBIN_H */

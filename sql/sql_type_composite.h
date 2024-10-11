#ifndef SQL_TYPE_COMPOSITE_INCLUDED
#define SQL_TYPE_COMPOSITE_INCLUDED

#include "sql_type.h"

class Item_splocal;
class Field_composite;
class Item_composite;
class Item_field;

class Type_handler_composite: public Type_handler
{
public:
  virtual ~Type_handler_composite() = default;
  const Name &default_value() const override;
  bool validate_implicit_default_value(THD *, const Column_definition &)
    const override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool is_scalar_type() const override { return false; }
  bool can_return_int() const override { return false; }
  bool can_return_decimal() const override { return false; }
  bool can_return_real() const override { return false; }
  bool can_return_str() const override { return false; }
  bool can_return_text() const override { return false; }
  bool can_return_date() const override { return false; }
  bool can_return_time() const override { return false; }
  enum_field_types field_type() const override
  {
    MY_ASSERT_UNREACHABLE();
    return MYSQL_TYPE_NULL;
  };
  protocol_send_type_t protocol_send_type() const override
  {
    MY_ASSERT_UNREACHABLE();
    return PROTOCOL_SEND_STRING;
  }
  Item_result result_type() const override
  {
    return ROW_RESULT;
  }
  Item_result cmp_type() const override
  {
    return ROW_RESULT;
  }
  enum_dynamic_column_type dyncol_type(const Type_all_attributes *)
                                       const override
  {
    MY_ASSERT_UNREACHABLE();
    return DYN_COL_NULL;
  }
  int stored_field_cmp_to_item(THD *, Field *, Item *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return 0;
  }
  bool subquery_type_allows_materialization(const Item *, const Item *, bool)
    const override
  {
    MY_ASSERT_UNREACHABLE();
    return false;
  }
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const
    override
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }
  Field *make_conversion_table_field(MEM_ROOT *, TABLE *, uint, const Field *)
    const override
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }
  bool Column_definition_fix_attributes(Column_definition *) const override
  {
    return false;
  }
  void Column_definition_reuse_fix_attributes(THD *, Column_definition *,
                                              const Field *) const override
  {
    MY_ASSERT_UNREACHABLE();
  }
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        column_definition_type_t type,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const override;
  bool Column_definition_redefine_stage1(Column_definition *,
                                         const Column_definition *,
                                         const handler *)
                                         const override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Column_definition_prepare_stage2(Column_definition *, handler *,
                                        ulonglong) const override
  {
    return false;
  }
  Field *make_table_field(MEM_ROOT *, const LEX_CSTRING *, const Record_addr &,
                          const Type_all_attributes &, TABLE_SHARE *)
    const override
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }
  void make_sort_key_part(uchar *to, Item *item,
                          const SORT_FIELD_ATTR *sort_field,
                          String *tmp) const override
  {
    MY_ASSERT_UNREACHABLE();
  }
  uint make_packed_sort_key_part(uchar *, Item *, const SORT_FIELD_ATTR *,
                                 String *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return 0;
  }
  void sort_length(THD *, const Type_std_attributes *, SORT_FIELD_ATTR *)
    const override
  {
    MY_ASSERT_UNREACHABLE();
  }
  uint32 max_display_length(const Item *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return 0;
  }
  uint32 max_display_length_for_field(const Conv_source &) const override
  {
    MY_ASSERT_UNREACHABLE();
    return 0;
  }
  uint32 calc_pack_length(uint32) const override
  {
    MY_ASSERT_UNREACHABLE();
    return 0;
  }
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const override;
  decimal_digits_t Item_decimal_precision(const Item *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return DECIMAL_MAX_PRECISION;
  }
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const
    override;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const override;
  bool Item_send(Item *, Protocol *, st_value *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  void Item_update_null_value(Item *item) const override;
  int Item_save_in_field(Item *, Field *, bool) const override
  {
    MY_ASSERT_UNREACHABLE();
    return 1;
  }
  bool can_change_cond_ref_to_const(Item_bool_func2 *, Item *, Item *,
                                   Item_bool_func2 *, Item *, Item *)
    const override
  {
    MY_ASSERT_UNREACHABLE();
    return false;
  }
  Item_copy *create_item_copy(THD *, Item *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems)
                                       const override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_val_bool(Item *item) const override
  {
    MY_ASSERT_UNREACHABLE();
    return false;
  }
  void Item_get_date(THD *, Item *, Temporal::Warn *, MYSQL_TIME *ltime,
                     date_mode_t) const override
  {
    MY_ASSERT_UNREACHABLE();
    set_zero_time(ltime, MYSQL_TIMESTAMP_NONE);
  }
  longlong Item_val_int_signed_typecast(Item *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return 0;
  }
  longlong Item_val_int_unsigned_typecast(Item *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return 0;
  }
  String *Item_func_hex_val_str_ascii(Item_func_hex *, String *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const override
  {
    MY_ASSERT_UNREACHABLE();
    return 0.0;
  }
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const override
  {
    MY_ASSERT_UNREACHABLE();
    return 0;
  }
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }
  void Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            Temporal::Warn *,
                                            MYSQL_TIME *ltime,
                                            date_mode_t) const override
  {
    MY_ASSERT_UNREACHABLE();
    set_zero_time(ltime, MYSQL_TIMESTAMP_NONE);
  }

  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const
    override
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }
  double Item_func_min_max_val_real(Item_func_min_max *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return 0;
  }
  longlong Item_func_min_max_val_int(Item_func_min_max *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return 0;
  }
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }
  bool Item_func_min_max_get_date(THD *, Item_func_min_max*, MYSQL_TIME *,
                                  date_mode_t) const override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_func_between_fix_length_and_dec(Item_func_between *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  longlong Item_func_between_val_int(Item_func_between *func) const override;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const
    override;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const override;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const override;

  bool Item_func_signed_fix_length_and_dec(Item_func_signed *) const override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *) const
    override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_double_typecast_fix_length_and_dec(Item_double_typecast *) const
    override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_float_typecast_fix_length_and_dec(Item_float_typecast *) const
    override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *) const
    override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_char_typecast_fix_length_and_dec(Item_char_typecast *) const
    override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_time_typecast_fix_length_and_dec(Item_time_typecast *) const
    override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_date_typecast_fix_length_and_dec(Item_date_typecast *) const
    override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  bool Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *)
    const override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }

  bool Item_func_plus_fix_length_and_dec(Item_func_plus *) const override;
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *) const override;
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *) const override;
  bool Item_func_div_fix_length_and_dec(Item_func_div *) const override;
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *) const override;

  virtual
  Item *create_item_method(THD *thd,
                        const Lex_ident_cli_st *ca,
                        const Lex_ident_cli_st *cb,
                        List<Item> *args,
                        const char *method_end) const
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }

  static const Type_handler *get_handler(Item *item);

  virtual bool key_to_lex_cstring(THD *thd, Item **key, const LEX_CSTRING& name, LEX_CSTRING& out_key) const
  {
    return false;
  }

  /*
    Get the index of the item with the given name in the composite item.
    
    This is only implemented for composite items that have a fixed number of
    fields, such as ROWs.
  */
  virtual bool get_item_index(THD *thd, const Item_field *item, const LEX_CSTRING& name, uint& idx) const = 0;
  virtual Item_field *get_item(THD *thd, const Item_field *item, const LEX_CSTRING& name) const = 0;
  virtual Item_field *get_or_create_item(THD *thd, Item_field *item, const LEX_CSTRING& name) const = 0;

  virtual void prepare_for_set(Item_field *item) const
  {
    return;
  }
  virtual bool finalize_for_set(Item_field *item) const
  {
    return false;
  }
};

#endif /* SQL_TYPE_COMPOSITE_INCLUDED */

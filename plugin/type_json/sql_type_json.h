#ifndef SQL_TYPE_JSON_PLUGIN_INCLUDED
#define SQL_TYPE_JSON_PLUGIN_INCLUDED

#include "mariadb.h"
#include "sql_type.h"

class Type_handler_json : public Type_handler_long_blob
{
public:
  static constexpr LEX_CSTRING name_on_client{STRING_WITH_LEN("json")};
  virtual ~Type_handler_json() {}
  bool Item_append_extended_type_info(Send_field_extended_metadata *to,
                                      const Item *item) const override
  {
    return to->set_format_name(name_on_client);
  }
  const Type_handler *type_handler_base() const override
  {
    return &type_handler_long_blob;
  }

  const Type_collection *type_collection() const override;
  uint get_column_attributes() const override { return ATTR_NONE; }
  bool Column_definition_prepare_stage1(
      THD *thd, MEM_ROOT *mem_root, Column_definition *def,
      column_definition_type_t type,
      const Column_derived_attributes *derived_attr) const override;
  Field *make_table_field(MEM_ROOT *root, const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;

  Field *make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  Field *make_conversion_table_field(MEM_ROOT *root, TABLE *table,
                                     uint metadata,
                                     const Field *target) const override;
  const Type_handler *type_handler_for_comparison() const override;
  const Type_handler *type_handler_for_tmp_table(const Item *item) const override;
  bool Item_hybrid_func_fix_attributes(THD *thd, const LEX_CSTRING &func_name,
        Type_handler_hybrid_field_type *handler, Type_all_attributes *func,
        Item **items, uint nitems) const override;

  virtual Item *create_typecast_item(THD *thd, Item *item,
                  const Type_cast_attributes &attr) const override;

  Item *make_constructor_item(THD *thd, List<Item> *args) const override;

  bool can_return_int() const override { return false; }
  bool can_return_decimal() const override { return false; }
  bool can_return_real() const override { return false; }
  bool can_return_date() const override { return false; }
  bool can_return_time() const override { return false; }
};

extern Type_handler_json type_handler_json;

class Type_collection_json : public Type_collection
{
public:
  const Type_handler *
  aggregate_for_result(const Type_handler *a,
                       const Type_handler *b) const override;

  const Type_handler *
  aggregate_for_min_max(const Type_handler *a,
                        const Type_handler *b) const override;

  const Type_handler *
  aggregate_for_comparison(const Type_handler *a,
                           const Type_handler *b) const override;

  const Type_handler *
  aggregate_for_num_op(const Type_handler *a,
                       const Type_handler *b) const override;
};

#include "field.h"

class Field_json : public Field_blob
{
  int report_wrong_value(const ErrConv &val);

public:
  Field_json(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
             enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
             TABLE_SHARE *share)
      : Field_blob(ptr_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
                   field_name_arg, share, 4 /* blob_pack_length */,
                   &my_charset_utf8mb4_bin)
  {
  }
  bool has_charset() const override { return false; }
  const Type_handler *type_handler() const override
  {
    return &type_handler_json;
  }
  using Field_blob::store;
  int store(const char *to, size_t length, CHARSET_INFO *charset) override;
  void sql_type(String &str) const override;
  void make_send_field(Send_field *to) override
  {
    Field_longstr::make_send_field(to);
    to->set_data_type_name(Type_handler_json::name_on_client);
  }
  uint size_of() const override { return sizeof(*this); }
  
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;

};

class Item_json_typecast : public Item_char_typecast
{
public: 
  Item_json_typecast(THD *thd, Item *a, CHARSET_INFO *cs_arg):
    Item_char_typecast(thd, a, -1, cs_arg) {}

  const Type_handler *type_handler() const override
  { return &type_handler_json; }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("cast_as_json")};
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *to) override;
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_json_typecast>(thd, this); }

  void print(String *str, enum_query_type query_type) override;
};

#endif // SQL_TYPE_JSON_PLUGIN_INCLUDED

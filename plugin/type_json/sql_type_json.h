#ifndef SQL_TYPE_JSON_PLUGIN_INCLUDED
#define SQL_TYPE_JSON_PLUGIN_INCLUDED

#include "mariadb.h"
#include "sql_type.h"

class Type_handler_json : public Type_handler_long_blob
{
public:
  virtual ~Type_handler_json() { }
  bool Item_append_extended_type_info(Send_field_extended_metadata *to,
                                      const Item *item) const override
  {
    static const LEX_CSTRING fmt= {STRING_WITH_LEN("json")};
    return to->set_format_name(fmt);
  }
  const Type_collection *type_collection() const override;
  uint get_column_attributes() const override { return ATTR_CHARSET; }
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        column_definition_type_t type,
                                        const Column_derived_attributes
                                        *derived_attr) const override;
  Field *make_table_field(MEM_ROOT *root, const LEX_CSTRING *name,
           const Record_addr &addr, const Type_all_attributes &attr,
           TABLE_SHARE *share) const override;

  Field *make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
           const LEX_CSTRING *name, const Record_addr &addr,
           const Bit_addr &bit, const Column_definition_attributes *attr,
           uint32 flags) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  // const Type_handler *type_handler_for_comparison() const override;
  // const Type_handler *type_handler_for_tmp_table(const Item *item) const
  //   override;
  // bool Item_hybrid_func_fix_attributes(THD *thd, const LEX_CSTRING &func_name,
  //       Type_handler_hybrid_field_type *handler, Type_all_attributes *func,
  //       Item **items, uint nitems) const override;

  // virtual Item *create_typecast_item(THD *thd, Item *item,
  //                 const Type_cast_attributes &attr) const override;

  // Item *make_constructor_item(THD *thd, List<Item> *args) const override;


  bool can_return_int() const override { return false; }
  bool can_return_decimal() const override { return false; }
  bool can_return_real() const override { return false; }
  bool can_return_date() const override { return false; }
  bool can_return_time() const override { return false; }

};

extern Type_handler_json type_handler_json;

class Type_collection_json: public Type_collection
{
public:
  const Type_handler *aggregate_for_result(const Type_handler *a,
                                           const Type_handler *b)
                                           const override;

  const Type_handler *aggregate_for_min_max(const Type_handler *a,
                                            const Type_handler *b)
                                            const override;

  const Type_handler *aggregate_for_comparison(const Type_handler *a,
                                               const Type_handler *b)
                                               const override;

  const Type_handler *aggregate_for_num_op(const Type_handler *a,
                                           const Type_handler *b)
                                           const override;
};


#include "field.h"

class Field_json : public Field_blob
{
  int report_wrong_value(const ErrConv &val);

public:
  Field_json(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
             enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
             TABLE_SHARE *share, const DTCollation &collation)
      : Field_blob(ptr_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
                   field_name_arg, share, 4 /* blob_pack_length */, collation)
  {
  }
  const Type_handler *type_handler() const override
  {
    return &type_handler_json;
  }

  void sql_type(String &str) const override;

  uint size_of() const override { return sizeof(*this); }
};

#endif // SQL_TYPE_JSON_INCLUDED

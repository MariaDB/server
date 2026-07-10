#define MYSQL_SERVER
#include "mariadb.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sql_type_json.h"

Type_handler_json type_handler_json;
Type_collection_json type_collection_json;

const Type_collection *Type_handler_json::type_collection() const
{
  return &type_collection_json;
}

bool Type_handler_json::Column_definition_prepare_stage1(
    THD *thd, MEM_ROOT *root, Column_definition *def,
    column_definition_type_t type,
    const Column_derived_attributes *derived_attr) const
{
  if (Type_handler_long_blob::Column_definition_prepare_stage1(
          thd, root, def, type, derived_attr))
    return true;
  return false;
}

Field *Type_handler_json::make_table_field(MEM_ROOT *root,
                                           const LEX_CSTRING *name,
                                           const Record_addr &addr,
                                           const Type_all_attributes &attr,
                                           TABLE_SHARE *share) const
{
  return new (root) Field_json(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                               Field::NONE, name, share);
}

Field *Type_handler_json::make_table_field_from_def(
    TABLE_SHARE *share, MEM_ROOT *root, const LEX_CSTRING *name,
    const Record_addr &rec, const Bit_addr &bit,
    const Column_definition_attributes *attr, uint32 flags) const
{
  return new (root) Field_json(rec.ptr(), rec.null_ptr(), rec.null_bit(),
                               attr->unireg_check, name, share);
}

Field *Type_handler_json::make_conversion_table_field(
    MEM_ROOT *root, TABLE *table, uint metadata, const Field *target) const
{
  /* A JSON field uses 4 bytes for length as json file is a long blob*/
  uint pack_length= metadata & 0x00ff;
  if (pack_length != 4)
    return NULL;

  return new (root) Field_json(NULL, (uchar *) "", 1, Field::NONE,
                               &empty_clex_str, table->s);
}

const Type_handler *Type_handler_json::type_handler_for_comparison() const {
  return &type_handler_json;
}

const Type_handler *Type_handler_json::type_handler_for_tmp_table(const Item *item) const
{
  return &type_handler_json;
}

bool Type_handler_json::Item_hybrid_func_fix_attributes(THD *thd,
                                            const LEX_CSTRING &func_name,
                                            Type_handler_hybrid_field_type *handler,
                                            Type_all_attributes *func,
                                            Item **items, uint nitems) const
{
  if (func->aggregate_attributes_string(func_name, items, nitems))
    return true;

  handler->set_handler(&type_handler_json);
  return false;
}

Item *Type_handler_json::create_typecast_item(THD *thd, Item *item,
        const Type_cast_attributes &attr) const
{
  CHARSET_INFO *real_cs= attr.charset() ?
                  attr.charset() : thd->variables.collation_connection;
  return new (thd->mem_root) Item_json_typecast(thd, item, real_cs);
}

Item *Type_handler_json::make_constructor_item(THD *thd, List<Item> *args) const
{
  if (!args || args->elements != 1)
    return nullptr;
  Item_args tmp(thd, *args);
  return new (thd->mem_root)
    Item_json_typecast(thd, tmp.arguments()[0],
                       thd->variables.collation_connection);
}
/********************Type collection json *******************/

const Type_handler *
Type_collection_json::aggregate_for_comparison(const Type_handler *a,
                                               const Type_handler *b) const
{
  if (a->type_collection() == this)
    swap_variables(const Type_handler *, a, b);
  if (a == &type_handler_json        || a == &type_handler_hex_hybrid ||
      a == &type_handler_tiny_blob   || a == &type_handler_blob       ||
      a == &type_handler_medium_blob || a == &type_handler_long_blob  ||
      a == &type_handler_varchar     || a == &type_handler_string     ||
      a == &type_handler_null)
    return b;
  return NULL;
}

const Type_handler *
Type_collection_json::aggregate_for_result(const Type_handler *a,
                                           const Type_handler *b) const
{
  return aggregate_for_comparison(a, b);
}

const Type_handler *
Type_collection_json::aggregate_for_min_max(const Type_handler *a,
                                            const Type_handler *b) const
{
  return aggregate_for_comparison(a, b);
}

const Type_handler *
Type_collection_json::aggregate_for_num_op(const Type_handler *a,
                                           const Type_handler *b) const
{
  return NULL;
}

/********************** Field_json *************************/

void Field_json::sql_type(String &res) const
{
  res.set_ascii(STRING_WITH_LEN("json"));
}

int Field_json::report_wrong_value(const ErrConv &val)
{
  get_thd()->push_warning_truncated_value_for_field(
      Sql_condition::WARN_LEVEL_WARN, "json", val.ptr(),
      table ? table->s->db.str : nullptr,
      table ? table->s->table_name.str : nullptr,
      field_name.str);
  reset();
  return 1;
}

int Field_json::store(const char *from, size_t length, CHARSET_INFO *cs)
{
  if (get_thd()->count_cuted_fields != CHECK_FIELD_IGNORE)
  {
    json_engine_t je;
    int stack_buf[JSON_DEPTH_LIMIT];
    initJsonArray(NULL, &je.stack, sizeof(int), stack_buf, 0);

    if (!json_valid(from, length, cs, &je))
    {
      return report_wrong_value(ErrConvString(from, length, cs));
    }
  }

  return Field_blob::store(from, length, cs);
}

enum_conv_type
Field_json::rpl_conv_type_from(const Conv_source &source,
                               const Relay_log_info *rli,
                               const Conv_param &param) const
{
  const Type_handler *th= source.type_handler();
  if (th == &type_handler_tiny_blob ||
      th == &type_handler_medium_blob ||
      th == &type_handler_long_blob ||
      th == &type_handler_blob ||
      th == &type_handler_blob_compressed ||
      th == &type_handler_string ||
      th == &type_handler_var_string ||
      th == &type_handler_varchar ||
      th == &type_handler_varchar_compressed)
  {
    return CONV_TYPE_PRECISE;
  }

  return CONV_TYPE_IMPOSSIBLE;
}

/***************************************************/

class Item_json_typecast_func_handler: public Item_handled_func::Handler_str
{
public:
  const Type_handler *
    return_type_handler(const Item_handled_func *item) const override
  { return &type_handler_json; }

  const Type_handler *
    type_handler_for_create_select(const Item_handled_func *item) const override
  { return &type_handler_json; }

  bool fix_length_and_dec(Item_handled_func *item) const override
  {
    return false;
  }
  String *val_str(Item_handled_func *item, String *to) const override
  {
    DBUG_ASSERT(dynamic_cast<const Item_json_typecast*>(item));
    return static_cast<Item_json_typecast*>(item)->val_str_generic(to);
  }
};


static Item_json_typecast_func_handler item_json_typecast_func_handler;

bool Item_json_typecast::fix_length_and_dec(THD *thd)
{
  if (cast_charset()->mbminlen > 1)
  {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
             "CAST(AS json CHARACTER SET ucs2/utf16/utf32)");
    return true;
  }

  Item_char_typecast::fix_length_and_dec_str();
  set_func_handler(&item_json_typecast_func_handler);
  return false;
}

String *Item_json_typecast::val_str(String *to)
{
  String *res = Item_char_typecast::val_str(to);
  if (!res)
    return nullptr;

  json_engine_t je;
  int stack_buf[JSON_DEPTH_LIMIT];
  initJsonArray(NULL, &je.stack, sizeof(int), stack_buf, 0);
  if (!json_valid(res->ptr(), res->length(), res->charset(), &je))
  {
    THD *thd= current_thd;
    ErrConvString err(res->ptr(), res->length(), res->charset());
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_TRUNCATED_WRONG_VALUE,
                        ER_THD(thd, ER_TRUNCATED_WRONG_VALUE), "JSON",
                        err.ptr());
    null_value= true;
    return nullptr;
  }
  return res;
}

void Item_json_typecast::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as json"));
  print_charset(str);
  str->append(')');
}




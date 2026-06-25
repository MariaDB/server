#define MYSQL_SERVER
#include "mariadb.h"
#include "sql_type_json.h"
#include "sql_class.h"
#include "sql_lex.h"

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
  def->charset= &my_charset_utf8mb4_bin;
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
                               Field::NONE, name, share, attr.collation);
}

Field *Type_handler_json::make_table_field_from_def(
    TABLE_SHARE *share, MEM_ROOT *root, const LEX_CSTRING *name,
    const Record_addr &rec, const Bit_addr &bit,
    const Column_definition_attributes *attr, uint32 flags) const
{
  return new (root) Field_json(rec.ptr(), rec.null_ptr(), rec.null_bit(),
                               attr->unireg_check, name, share, attr->charset);
}

Field *Type_handler_json::make_conversion_table_field(
    MEM_ROOT *root, TABLE *table, uint metadata, const Field *target) const
{
  /* A JSON field uses 4 bytes for length as json file is a long blob*/
  uint pack_length= metadata & 0x00ff;
  if (pack_length != 4)
    return NULL;

  return new (root) Field_json(NULL, (uchar *) "", 1, Field::NONE,
                               &empty_clex_str, table->s, target->charset());
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

/*
   Copyright (c) 2025, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include "sql_type_xmltype.h"
#include "sql_class.h"
#include "sql_lex.h"

Named_type_handler<Type_handler_xmltype> type_handler_xmltype("xmltype");
Type_collection_xmltype type_collection_xmltype;

const Type_collection *Type_handler_xmltype::type_collection() const
{
  return &type_collection_xmltype;
}

const Type_handler *Type_collection_xmltype::aggregate_for_comparison(
                       const Type_handler *a, const Type_handler *b) const
{
  if (a->type_collection() == this)
    swap_variables(const Type_handler *, a, b);
  if (a == &type_handler_xmltype      || a == &type_handler_hex_hybrid ||
      a == &type_handler_tiny_blob   || a == &type_handler_blob       ||
      a == &type_handler_medium_blob || a == &type_handler_long_blob  ||
      a == &type_handler_varchar     || a == &type_handler_string     ||
      a == &type_handler_null)
    return b;
  return NULL;
}

const Type_handler *Type_collection_xmltype::aggregate_for_result(
                       const Type_handler *a, const Type_handler *b) const
{
  return aggregate_for_comparison(a,b);
}

const Type_handler *Type_collection_xmltype::aggregate_for_min_max(
                       const Type_handler *a, const Type_handler *b) const
{
  return aggregate_for_comparison(a,b);
}

const Type_handler *Type_collection_xmltype::aggregate_for_num_op(
                      const Type_handler *a, const Type_handler *b) const
{
  return NULL;
}


const Type_handler *Type_handler_xmltype::type_handler_for_comparison() const
{
  return &type_handler_xmltype;
}


bool Type_handler_xmltype::
       Column_definition_fix_attributes(Column_definition *def) const
{
  def->flags|= BLOB_FLAG;
  return false;
}


bool Type_handler_xmltype::
       Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        column_definition_type_t type,
                                        const Column_derived_attributes
                                              *derived_attr) const
{
  def->prepare_stage1_simple(derived_attr->charset());
  def->create_length_to_internal_length_string();
  return FALSE;
}


bool Type_handler_xmltype::
       Column_definition_prepare_stage2(Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags) const
{
  return def->prepare_stage2_blob(file, table_flags, FIELDFLAG_BLOB);
}

bool Type_handler_xmltype::Key_part_spec_init_primary(Key_part_spec *part,
                                              const Column_definition &def,
                                              const handler *file) const
{
  return part->check_primary_key_for_blob(file);
}


bool Type_handler_xmltype::Key_part_spec_init_unique(Key_part_spec *part,
                                              const Column_definition &def,
                                              const handler *file,
                                              bool *hash_field_needed) const
{
  if (!part->length)
    *hash_field_needed= true;
  return part->check_key_for_blob(file);
}


bool Type_handler_xmltype::Key_part_spec_init_multiple(Key_part_spec *part,
                                               const Column_definition &def,
                                               const handler *file) const
{
  return part->init_multiple_key_for_blob(file);
}


bool Type_handler_xmltype::Key_part_spec_init_foreign(Key_part_spec *part,
                                               const Column_definition &def,
                                               const handler *file) const
{
  return part->check_foreign_key_for_blob(file);
}


Field *Type_handler_xmltype::make_conversion_table_field(
      MEM_ROOT *root, TABLE *table, uint metadata, const Field *target) const
{
  /* Copied from Type_handler_blob_common. */
  uint pack_length= metadata & 0x00ff;
  if (pack_length < 1 || pack_length > 4)
    return NULL; // Broken binary log?

  return new(root)
    Field_xmltype(NULL, (uchar *) "", 1, Field::NONE, &empty_clex_str,
                  table->s, pack_length, target->charset());
}


Item *Type_handler_xmltype::create_typecast_item(THD *thd, Item *item,
        const Type_cast_attributes &attr) const
{
  return NULL;
}


Field *Type_handler_xmltype::make_table_field(MEM_ROOT *root,
         const LEX_CSTRING *name, const Record_addr &addr,
         const Type_all_attributes &attr, TABLE_SHARE *share) const
{
  return new (root) Field_xmltype(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                                 Field::NONE, name, share, 4, attr.collation);
}


Field *Type_handler_xmltype::make_table_field_from_def(TABLE_SHARE *share,
         MEM_ROOT *root, const LEX_CSTRING *name, const Record_addr &rec,
         const Bit_addr &bit, const Column_definition_attributes *attr,
         uint32 flags) const
{
  return new (root) Field_xmltype(rec.ptr(), rec.null_ptr(), rec.null_bit(),
            attr->unireg_check, name, share, attr->pack_flag_to_pack_length(),
            attr->charset);
}

/*****************************************************************/
void Field_xmltype::sql_type(String &res) const
{
  res.set_ascii(STRING_WITH_LEN("xmltype"));
}


int Field_xmltype::report_wrong_value(const ErrConv &val)
{
  get_thd()->push_warning_truncated_value_for_field(
    Sql_condition::WARN_LEVEL_WARN, "xmltype", val.ptr(),
    table->s->db.str, table->s->table_name.str, field_name.str);
  reset();
  return 1;
}


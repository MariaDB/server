/*
   Copyright (c) 2015 MariaDB Foundation
   Copyright (c) 2019 MariaDB

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

#include "sql_type_geom.h"
#include "sql_class.h"
#include "item.h"

#ifdef HAVE_SPATIAL

const Name
  Type_handler_geometry::
    m_name_geometry(STRING_WITH_LEN("geometry")),
  Type_handler_point::
    m_name_point(STRING_WITH_LEN("point")),
  Type_handler_linestring::
    m_name_linestring(STRING_WITH_LEN("linestring")),
  Type_handler_polygon::
    m_name_polygon(STRING_WITH_LEN("polygon")),
  Type_handler_multipoint::
    m_name_multipoint(STRING_WITH_LEN("multipoint")),
  Type_handler_multilinestring::
    m_name_multilinestring(STRING_WITH_LEN("multilinestring")),
  Type_handler_multipolygon::
    m_name_multipolygon(STRING_WITH_LEN("multipolygon")),
  Type_handler_geometrycollection::
    m_name_geometrycollection(STRING_WITH_LEN("geometrycollection"));


Type_handler_geometry           type_handler_geometry;
Type_handler_point              type_handler_point;
Type_handler_linestring         type_handler_linestring;
Type_handler_polygon            type_handler_polygon;
Type_handler_multipoint         type_handler_multipoint;
Type_handler_multilinestring    type_handler_multilinestring;
Type_handler_multipolygon       type_handler_multipolygon;
Type_handler_geometrycollection type_handler_geometrycollection;


Type_collection_geometry        type_collection_geometry;


const Type_handler_geometry *
Type_handler_geometry::type_handler_geom_by_type(uint type)
{
  switch (type) {
  case Type_handler_geometry::GEOM_POINT:
    return &type_handler_point;
  case Type_handler_geometry::GEOM_LINESTRING:
    return &type_handler_linestring;
  case Type_handler_geometry::GEOM_POLYGON:
    return &type_handler_polygon;
  case Type_handler_geometry::GEOM_MULTIPOINT:
    return &type_handler_multipoint;
  case Type_handler_geometry::GEOM_MULTILINESTRING:
    return &type_handler_multilinestring;
  case Type_handler_geometry::GEOM_MULTIPOLYGON:
    return &type_handler_multipolygon;
  case Type_handler_geometry::GEOM_GEOMETRYCOLLECTION:
    return &type_handler_geometrycollection;
  case Type_handler_geometry::GEOM_GEOMETRY:
    break;
  }
  return &type_handler_geometry;
}


const Type_handler *
Type_collection_geometry::handler_by_name(const LEX_CSTRING &name) const
{
  if (type_handler_point.name().eq(name))
    return &type_handler_point;
  if (type_handler_linestring.name().eq(name))
    return &type_handler_linestring;
  if (type_handler_polygon.name().eq(name))
    return &type_handler_polygon;
  if (type_handler_multipoint.name().eq(name))
    return &type_handler_multipoint;
  if (type_handler_multilinestring.name().eq(name))
    return &type_handler_multilinestring;
  if (type_handler_multipolygon.name().eq(name))
    return &type_handler_multipolygon;
  if (type_handler_geometry.name().eq(name))
    return &type_handler_geometry;
  if (type_handler_geometrycollection.name().eq(name))
    return &type_handler_geometrycollection;
  return NULL;
}


const Type_collection *Type_handler_geometry::type_collection() const
{
  return &type_collection_geometry;
}


const Type_handler *
Type_handler_geometry::type_handler_frm_unpack(const uchar *buffer) const
{
  // charset and geometry_type share the same byte in frm
  return type_handler_geom_by_type((uint) buffer[14]);
}


bool Type_collection_geometry::init_aggregators(Type_handler_data *data,
                                                const Type_handler *geom) const
{
  Type_aggregator *r= &data->m_type_aggregator_for_result;
  Type_aggregator *c= &data->m_type_aggregator_for_comparison;
  return
    r->add(geom, &type_handler_null,        geom) ||
    r->add(geom, &type_handler_hex_hybrid,  &type_handler_long_blob) ||
    r->add(geom, &type_handler_tiny_blob,   &type_handler_long_blob) ||
    r->add(geom, &type_handler_blob,        &type_handler_long_blob) ||
    r->add(geom, &type_handler_medium_blob, &type_handler_long_blob) ||
    r->add(geom, &type_handler_long_blob,   &type_handler_long_blob) ||
    r->add(geom, &type_handler_varchar,     &type_handler_long_blob) ||
    r->add(geom, &type_handler_string,      &type_handler_long_blob) ||
    c->add(geom, &type_handler_null,        geom) ||
    c->add(geom, &type_handler_long_blob,   &type_handler_long_blob);
}


bool Type_collection_geometry::init(Type_handler_data *data)
{
#ifndef DBUG_OFF
  /*
    The rules (geometry,geometry)->geometry and (pont,point)->geometry
    are needed here to make sure
    (in gis-debug.test) that they do not affect anything, and these pairs
    returns an error in an expression like (POINT(0,0)+POINT(0,0)).
    Both sides are from the same type collection here,
    so aggregation goes only through Type_collection_xxx::aggregate_yyy()
    and never reaches Type_aggregator::find_handler().
  */
  Type_aggregator *nct= &data->m_type_aggregator_non_commutative_test;
  if (nct->add(&type_handler_geometry,
               &type_handler_geometry,
               &type_handler_geometry) ||
      nct->add(&type_handler_point,
               &type_handler_point,
               &type_handler_geometry) ||
      nct->add(&type_handler_point,
               &type_handler_varchar,
               &type_handler_long_blob))
  return true;
#endif // DBUG_OFF
  return
    init_aggregators(data, &type_handler_geometry) ||
    init_aggregators(data, &type_handler_geometrycollection) ||
    init_aggregators(data, &type_handler_point) ||
    init_aggregators(data, &type_handler_linestring) ||
    init_aggregators(data, &type_handler_polygon) ||
    init_aggregators(data, &type_handler_multipoint) ||
    init_aggregators(data, &type_handler_multilinestring) ||
    init_aggregators(data, &type_handler_multipolygon);
}


bool Type_handler_geometry::check_type_geom_or_binary(const char *opname,
                                                      const Item *item)
{
  const Type_handler *handler= item->type_handler();
  if (handler->type_handler_for_comparison() == &type_handler_geometry ||
      (handler->is_general_purpose_string_type() &&
       item->collation.collation == &my_charset_bin))
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           handler->name().ptr(), opname);
  return true;
}


bool Type_handler_geometry::check_types_geom_or_binary(const char *opname,
                                                       Item* const *args,
                                                       uint start, uint end)
{
  for (uint i= start; i < end ; i++)
  {
    if (check_type_geom_or_binary(opname, args[i]))
      return true;
  }
  return false;
}


const Type_handler *Type_handler_geometry::type_handler_for_comparison() const
{
  return &type_handler_geometry;
}


Field *Type_handler_geometry::make_conversion_table_field(TABLE *table,
                                                          uint metadata,
                                                          const Field *target)
                                                          const
{
  DBUG_ASSERT(target->type() == MYSQL_TYPE_GEOMETRY);
  /*
    We do not do not update feature_gis statistics here:
    status_var_increment(target->table->in_use->status_var.feature_gis);
    as this is only a temporary field.
    The statistics was already incremented when "target" was created.
  */
  const Field_geom *fg= static_cast<const Field_geom*>(target);
  return new(table->in_use->mem_root)
         Field_geom(NULL, (uchar *) "", 1, Field::NONE, &empty_clex_str,
                    table->s, 4, fg->type_handler_geom(), fg->srid);
}


bool Type_handler_geometry::
       Column_definition_fix_attributes(Column_definition *def) const
{
  def->flags|= BLOB_FLAG;
  return false;
}

void Type_handler_geometry::
       Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *def,
                                              const Field *field) const
{
  def->srid= ((Field_geom*) field)->srid;
}


bool Type_handler_geometry::
       Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags) const
{
  def->create_length_to_internal_length_string();
  return def->prepare_blob_field(thd);
}


bool Type_handler_geometry::
       Column_definition_prepare_stage2(Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags) const
{
  if (!(table_flags & HA_CAN_GEOMETRY))
  {
    my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), "GEOMETRY");
    return true;
  }
  return def->prepare_stage2_blob(file, table_flags, FIELDFLAG_GEOM);
}

bool Type_handler_geometry::Key_part_spec_init_primary(Key_part_spec *part,
                                              const Column_definition &def,
                                              const handler *file) const
{
  return part->check_primary_key_for_blob(file);
}


bool Type_handler_geometry::Key_part_spec_init_unique(Key_part_spec *part,
                                              const Column_definition &def,
                                              const handler *file,
                                              bool *hash_field_needed) const
{
  if (!part->length)
    *hash_field_needed= true;
  return part->check_key_for_blob(file);
}


bool Type_handler_geometry::Key_part_spec_init_multiple(Key_part_spec *part,
                                               const Column_definition &def,
                                               const handler *file) const
{
  return part->init_multiple_key_for_blob(file);
}


bool Type_handler_geometry::Key_part_spec_init_foreign(Key_part_spec *part,
                                               const Column_definition &def,
                                               const handler *file) const
{
  return part->check_foreign_key_for_blob(file);
}


bool Type_handler_geometry::Key_part_spec_init_spatial(Key_part_spec *part,
                                                  const Column_definition &def)
                                                  const
{
  if (part->length)
  {
    my_error(ER_WRONG_SUB_KEY, MYF(0));
    return true;
  }
  /*
    4 is: (Xmin,Xmax,Ymin,Ymax), this is for 2D case
    Lately we'll extend this code to support more dimensions
  */
  part->length= 4 * sizeof(double);
  return false;
}


bool Type_handler_point::Key_part_spec_init_primary(Key_part_spec *part,
                                              const Column_definition &def,
                                              const handler *file) const
{
  /*
    QQ:
    The below assignment (here and in all other Key_part_spec_init_xxx methods)
    overrides the explicitly given key part length, so in this query:
      CREATE OR REPLACE TABLE t1 (a POINT, KEY(a(10)));
    the key becomes KEY(a(25)).
    This might be a bug.
  */
  part->length= octet_length();
  return part->check_key_for_blob(file);
}


bool Type_handler_point::Key_part_spec_init_unique(Key_part_spec *part,
                                              const Column_definition &def,
                                              const handler *file,
                                              bool *hash_field_needed) const
{
  part->length= octet_length();
  return part->check_key_for_blob(file);
}


bool Type_handler_point::Key_part_spec_init_multiple(Key_part_spec *part,
                                              const Column_definition &def,
                                              const handler *file) const
{
  part->length= octet_length();
  return part->check_key_for_blob(file);
}


bool Type_handler_point::Key_part_spec_init_foreign(Key_part_spec *part,
                                              const Column_definition &def,
                                              const handler *file) const
{
  part->length= octet_length();
  return part->check_key_for_blob(file);
}


Item *
Type_handler_point::make_constructor_item(THD *thd, List<Item> *args) const
{
  if (!args || args->elements != 2)
    return NULL;
  Item_args tmp(thd, *args);
  return new (thd->mem_root) Item_func_point(thd,
                                             tmp.arguments()[0],
                                             tmp.arguments()[1]);
}


Item *
Type_handler_linestring::make_constructor_item(THD *thd, List<Item> *args) const
{
  return args ? new (thd->mem_root) Item_func_linestring(thd, *args) : NULL;
}


Item *
Type_handler_polygon::make_constructor_item(THD *thd, List<Item> *args) const
{
  return args ? new (thd->mem_root) Item_func_polygon(thd, *args) : NULL;
}


Item *
Type_handler_multipoint::make_constructor_item(THD *thd, List<Item> *args) const
{
  return args ? new (thd->mem_root) Item_func_multipoint(thd, *args) : NULL;
}


Item *
Type_handler_multilinestring::make_constructor_item(THD *thd,
                                                    List<Item> *args) const
{
  return args ? new (thd->mem_root) Item_func_multilinestring(thd, *args) :
                NULL;
}


Item *
Type_handler_multipolygon::make_constructor_item(THD *thd,
                                                 List<Item> *args) const
{
  return args ? new (thd->mem_root) Item_func_multipolygon(thd, *args) : NULL;
}


Item *
Type_handler_geometrycollection::make_constructor_item(THD *thd,
                                                       List<Item> *args) const
{
  return args ? new (thd->mem_root) Item_func_geometrycollection(thd, *args) :
                NULL;
}


uint32 Type_handler_geometry::calc_pack_length(uint32 length) const
{
  return 4 + portable_sizeof_char_ptr;
}


Field *Type_handler_geometry::make_table_field(const LEX_CSTRING *name,
                                               const Record_addr &addr,
                                               const Type_all_attributes &attr,
                                               TABLE *table) const
{
  return new (table->in_use->mem_root)
         Field_geom(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                    Field::NONE, name, table->s, 4, this, 0);
}


bool Type_handler_geometry::
       Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *func_name,
                                       Type_handler_hybrid_field_type *handler,
                                       Type_all_attributes *func,
                                       Item **items, uint nitems) const
{
  DBUG_ASSERT(nitems > 0);
  func->collation.set(&my_charset_bin);
  func->unsigned_flag= false;
  func->decimals= 0;
  func->max_length= (uint32) UINT_MAX32;
  func->set_maybe_null(true);
  return false;
}


bool Type_handler_geometry::
       Item_sum_sum_fix_length_and_dec(Item_sum_sum *item) const
{
  return Item_func_or_sum_illegal_param("sum");
}


bool Type_handler_geometry::
       Item_sum_avg_fix_length_and_dec(Item_sum_avg *item) const
{
  return Item_func_or_sum_illegal_param("avg");
}


bool Type_handler_geometry::
       Item_sum_variance_fix_length_and_dec(Item_sum_variance *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_geometry::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_geometry::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_geometry::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_geometry::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  return Item_func_or_sum_illegal_param(item);
}



bool Type_handler_geometry::
       Item_func_signed_fix_length_and_dec(Item_func_signed *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_geometry::
       Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_geometry::
       Item_double_typecast_fix_length_and_dec(Item_double_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_geometry::
       Item_float_typecast_fix_length_and_dec(Item_float_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_geometry::
       Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_geometry::
       Item_char_typecast_fix_length_and_dec(Item_char_typecast *item) const
{
  if (item->cast_charset() != &my_charset_bin)
    return Item_func_or_sum_illegal_param(item); // CAST(geom AS CHAR)
  item->fix_length_and_dec_str();
  return false; // CAST(geom AS BINARY)
}


bool Type_handler_geometry::
       Item_time_typecast_fix_length_and_dec(Item_time_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}



bool Type_handler_geometry::
       Item_date_typecast_fix_length_and_dec(Item_date_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_geometry::
       Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *item)
                                                 const
{
  return Item_func_or_sum_illegal_param(item);

}


bool Type_handler_geometry::
  Item_param_set_from_value(THD *thd,
                            Item_param *param,
                            const Type_all_attributes *attr,
                            const st_value *val) const
{
  param->unsigned_flag= false;
  param->setup_conversion_blob(thd);
  return param->set_str(val->m_string.ptr(), val->m_string.length(),
                        &my_charset_bin, &my_charset_bin);
}


void Type_handler_geometry::Item_param_set_param_func(Item_param *param,
                                                      uchar **pos,
                                                      ulong len) const
{
  param->set_null(); // Not possible type code in the client-server protocol
}


Field *Type_handler_geometry::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  status_var_increment(current_thd->status_var.feature_gis);
  return new (mem_root)
    Field_geom(rec.ptr(), rec.null_ptr(), rec.null_bit(),
               attr->unireg_check, name, share,
               attr->pack_flag_to_pack_length(), this, attr->srid);
}


void Type_handler_geometry::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  def->frm_pack_basic(buff);
  buff[11]= 0;
  buff[14]= (uchar) geometry_type();
}


bool Type_handler_geometry::
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const
{
  uint gis_opt_read, gis_length, gis_decimals;
  Field_geom::storage_type st_type;
  attr->frm_unpack_basic(buffer);
  gis_opt_read= gis_field_options_read(gis_options->str,
                                       gis_options->length,
                                       &st_type, &gis_length,
                                       &gis_decimals, &attr->srid);
  gis_options->str+= gis_opt_read;
  gis_options->length-= gis_opt_read;
  return false;
}


uint32
Type_handler_geometry::max_display_length_for_field(const Conv_source &src)
                                                    const
{
  return (uint32) my_set_bits(4 * 8);
}


enum_conv_type
Field_geom::rpl_conv_type_from(const Conv_source &source,
                               const Relay_log_info *rli,
                               const Conv_param &param) const
{
  return binlog_type() == source.real_field_type() ?
         rpl_conv_type_from_same_data_type(source.metadata(), rli, param) :
         CONV_TYPE_IMPOSSIBLE;
}


#endif // HAVE_SPATIAL

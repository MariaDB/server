/*
   Copyright (c) 2015, 2020, MariaDB

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

#include "mariadb.h"

#ifdef HAVE_SPATIAL

#include "sql_class.h"
#include "sql_type_geom.h"
#include "item_geofunc.h"

Named_type_handler<Type_handler_geometry> type_handler_geometry("geometry");
Named_type_handler<Type_handler_point> type_handler_point("point");
Named_type_handler<Type_handler_linestring> type_handler_linestring("linestring");
Named_type_handler<Type_handler_polygon> type_handler_polygon("polygon");
Named_type_handler<Type_handler_multipoint> type_handler_multipoint("multipoint");
Named_type_handler<Type_handler_multilinestring> type_handler_multilinestring("multilinestring");
Named_type_handler<Type_handler_multipolygon> type_handler_multipolygon("multipolygon");
Named_type_handler<Type_handler_geometrycollection> type_handler_geometrycollection("geometrycollection");

Type_collection_geometry        type_collection_geometry;


LEX_CSTRING Type_handler_geometry::extended_metadata_data_type_name() const
{
  return geometry_type() == GEOM_GEOMETRY ? null_clex_str :
                                            name().lex_cstring();
}


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


const Type_handler *
Type_collection_geometry::aggregate_for_comparison(const Type_handler *a,
                                                   const Type_handler *b)
                                                   const
{
  const Type_handler *h;
  if ((h= aggregate_common(a, b)) ||
      (h= aggregate_if_null(a, b)) ||
      (h= aggregate_if_long_blob(a, b)))
    return h;
  return NULL;
}


const Type_handler *
Type_collection_geometry::aggregate_for_result(const Type_handler *a,
                                               const Type_handler *b)
                                               const
{
  const Type_handler *h;
  if ((h= aggregate_common(a, b)) ||
      (h= aggregate_if_null(a, b)) ||
      (h= aggregate_if_long_blob(a, b)) ||
      (h= aggregate_if_string(a, b)))
    return h;
  return NULL;
}


const Type_handler *
Type_collection_geometry::aggregate_for_min_max(const Type_handler *a,
                                                const Type_handler *b)
                                                const
{
  const Type_handler *h;
  if ((h= aggregate_common(a, b)) ||
      (h= aggregate_if_null(a, b)) ||
      (h= aggregate_if_long_blob(a, b)) ||
      (h= aggregate_if_string(a, b)))
    return h;
  return NULL;
}


const Type_handler *
Type_collection_geometry::aggregate_if_string(const Type_handler *a,
                                              const Type_handler *b) const
{
  if (a->type_collection() == this)
  {
    DBUG_ASSERT(b->type_collection() != this);
    swap_variables(const Type_handler *, a, b);
  }
  if (a == &type_handler_hex_hybrid ||
      a == &type_handler_tiny_blob ||
      a == &type_handler_blob ||
      a == &type_handler_medium_blob ||
      a == &type_handler_varchar ||
      a == &type_handler_string)
    return &type_handler_long_blob;
  return NULL;
}


#ifndef DBUG_OFF
bool Type_collection_geometry::init_aggregators(Type_handler_data *data,
                                                const Type_handler *geom) const
{
  Type_aggregator *r= &data->m_type_aggregator_for_result;
  return
    r->add(geom, &type_handler_hex_hybrid,  &type_handler_long_blob) ||
    r->add(geom, &type_handler_tiny_blob,   &type_handler_long_blob) ||
    r->add(geom, &type_handler_blob,        &type_handler_long_blob) ||
    r->add(geom, &type_handler_medium_blob, &type_handler_long_blob) ||
    r->add(geom, &type_handler_varchar,     &type_handler_long_blob) ||
    r->add(geom, &type_handler_string,      &type_handler_long_blob);
}
#endif


bool Type_collection_geometry::init(Type_handler_data *data)
{
#ifndef DBUG_OFF
  Type_aggregator *nct= &data->m_type_aggregator_non_commutative_test;
  if (nct->add(&type_handler_point,
               &type_handler_varchar,
               &type_handler_long_blob))
    return true;
  return
    init_aggregators(data, &type_handler_geometry) ||
    init_aggregators(data, &type_handler_geometrycollection) ||
    init_aggregators(data, &type_handler_point) ||
    init_aggregators(data, &type_handler_linestring) ||
    init_aggregators(data, &type_handler_polygon) ||
    init_aggregators(data, &type_handler_multipoint) ||
    init_aggregators(data, &type_handler_multilinestring) ||
    init_aggregators(data, &type_handler_multipolygon);
#endif // DBUG_OFF
  return false;
}


bool Type_handler_geometry::
check_type_geom_or_binary(const LEX_CSTRING &opname,
                          const Item *item)
{
  const Type_handler *handler= item->type_handler();
  if (handler->type_handler_for_comparison() == &type_handler_geometry ||
      (handler->is_general_purpose_string_type() &&
       item->collation.collation == &my_charset_bin))
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           handler->name().ptr(), opname.str);
  return true;
}


bool Type_handler_geometry::
check_types_geom_or_binary(const LEX_CSTRING &opname,
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


Field *Type_handler_geometry::make_conversion_table_field(MEM_ROOT *root,
                                                          TABLE *table,
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
  return new (root)
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


Item *
Type_handler_geometry::create_typecast_item(THD *thd, Item *item,
                                           const Type_cast_attributes &attr)
                                           const
{
  DBUG_EXECUTE_IF("emulate_geometry_create_typecast_item",
    return new (thd->mem_root) Item_func_geometry_from_text(thd, item);
  );

  return NULL;
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


Field *Type_handler_geometry::make_table_field(MEM_ROOT *root,
                                               const LEX_CSTRING *name,
                                               const Record_addr &addr,
                                               const Type_all_attributes &attr,
                                               TABLE_SHARE *share) const
{
  return new (root)
         Field_geom(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                    Field::NONE, name, share, 4, this, 0);
}


bool Type_handler_geometry::
       Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &func_name,
                                       Type_handler_hybrid_field_type *handler,
                                       Type_all_attributes *func,
                                       Item **items, uint nitems) const
{
  DBUG_ASSERT(nitems > 0);
  func->collation.set(&my_charset_bin);
  func->unsigned_flag= false;
  func->decimals= 0;
  func->max_length= (uint32) UINT_MAX32;
  func->set_type_maybe_null(true);
  return false;
}


bool Type_handler_geometry::
       Item_sum_sum_fix_length_and_dec(Item_sum_sum *item) const
{
  LEX_CSTRING name= {STRING_WITH_LEN("sum") };
  return Item_func_or_sum_illegal_param(name);
}


bool Type_handler_geometry::
       Item_sum_avg_fix_length_and_dec(Item_sum_avg *item) const
{
  LEX_CSTRING name= {STRING_WITH_LEN("avg") };
  return Item_func_or_sum_illegal_param(name);
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
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  status_var_increment(current_thd->status_var.feature_gis);
  return new (root)
    Field_geom(rec.ptr(), rec.null_ptr(), rec.null_bit(),
               attr->unireg_check, name, share,
               attr->pack_flag_to_pack_length(), this, attr->srid);
}


void Type_handler_geometry::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  DBUG_ASSERT(f_decimals(def->pack_flag & ~FIELDFLAG_GEOM) == 0);
  def->frm_pack_basic(buff);
  buff[11]= 0;
  buff[14]= (uchar) geometry_type();
}



/* Values 1-40 reserved for 1-byte options,
   41-80 for 2-byte options,
   81-120 for 4-byte options,
   121-160 for 8-byte options,
   other - varied length in next 1-3 bytes.
*/
enum extra2_gis_field_options {
  FIELDGEOM_END=0,
  FIELDGEOM_STORAGE_MODEL=1,
  FIELDGEOM_PRECISION=2,
  FIELDGEOM_SCALE=3,
  FIELDGEOM_SRID=81,
};


uint
Type_handler_geometry::
  Column_definition_gis_options_image(uchar *cbuf,
                                      const Column_definition &def) const
{
  if (cbuf)
  {
    cbuf[0]= FIELDGEOM_STORAGE_MODEL;
    cbuf[1]= (uchar) Field_geom::GEOM_STORAGE_WKB;

    cbuf[2]= FIELDGEOM_PRECISION;
    cbuf[3]= (uchar) def.length;

    cbuf[4]= FIELDGEOM_SCALE;
    cbuf[5]= (uchar) def.decimals;

    cbuf[6]= FIELDGEOM_SRID;
    int4store(cbuf + 7, ((uint32) def.srid));

    cbuf[11]= FIELDGEOM_END;
  }
  return 12;
}


static uint gis_field_options_read(const uchar *buf, size_t buf_len,
                                   Field_geom::storage_type *st_type,
                                   uint *precision, uint *scale, uint *srid)
{
  const uchar *buf_end= buf + buf_len;
  const uchar *cbuf= buf;
  int option_id;

  *precision= *scale= *srid= 0;
  *st_type= Field_geom::GEOM_STORAGE_WKB;

  if (!buf)  /* can only happen with the old FRM file */
    goto end_of_record;

  while (cbuf < buf_end)
  {
    switch ((option_id= *(cbuf++)))
    {
    case FIELDGEOM_STORAGE_MODEL:
      *st_type= (Field_geom::storage_type) cbuf[0];
      break;
    case FIELDGEOM_PRECISION:
      *precision= cbuf[0];
      break;
    case FIELDGEOM_SCALE:
      *scale= cbuf[0];
      break;
    case FIELDGEOM_SRID:
      *srid= uint4korr(cbuf);
      break;
    case FIELDGEOM_END:
      goto end_of_record;
    }
    if (option_id > 0 && option_id <= 40)
      cbuf+= 1;
    else if (option_id > 40 && option_id <= 80)
      cbuf+= 2;
    else if (option_id > 80 && option_id <= 120)
      cbuf+= 4;
    else if (option_id > 120 && option_id <= 160)
      cbuf+= 8;
    else /* > 160 and <=255 */
      cbuf+= cbuf[0] ? 1 + cbuf[0] : 3 + uint2korr(cbuf+1);
  }

end_of_record:
  return (uint)(cbuf - buf);
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


/*****************************************************************/
void Field_geom::sql_type(String &res) const
{
  CHARSET_INFO *cs= &my_charset_latin1;
  const Name tmp= m_type_handler->name();
  res.set(tmp.ptr(), tmp.length(), cs);
}


int Field_geom::store(double nr)
{
  my_message(ER_CANT_CREATE_GEOMETRY_OBJECT,
             ER_THD(get_thd(), ER_CANT_CREATE_GEOMETRY_OBJECT), MYF(0));
  return -1;
}


int Field_geom::store(longlong nr, bool unsigned_val)
{
  my_message(ER_CANT_CREATE_GEOMETRY_OBJECT,
             ER_THD(get_thd(), ER_CANT_CREATE_GEOMETRY_OBJECT), MYF(0));
  return -1;
}


int Field_geom::store_decimal(const my_decimal *)
{
  my_message(ER_CANT_CREATE_GEOMETRY_OBJECT,
             ER_THD(get_thd(), ER_CANT_CREATE_GEOMETRY_OBJECT), MYF(0));
  return -1;
}


int Field_geom::store(const char *from, size_t length, CHARSET_INFO *cs)
{
  if (!length)
    bzero(ptr, Field_blob::pack_length());
  else
  {
    // Check given WKB
    uint32 wkb_type;
    if (length < SRID_SIZE + WKB_HEADER_SIZE + 4)
      goto err;
    wkb_type= uint4korr(from + SRID_SIZE + 1);
    if (wkb_type < (uint32) Geometry::wkb_point ||
	wkb_type > (uint32) Geometry::wkb_last)
      goto err;

    if (m_type_handler->geometry_type() != Type_handler_geometry::GEOM_GEOMETRY &&
        m_type_handler->geometry_type() != Type_handler_geometry::GEOM_GEOMETRYCOLLECTION &&
        (uint32) m_type_handler->geometry_type() != wkb_type)
    {
      const char *db= table->s->db.str;
      const char *tab_name= table->s->table_name.str;

      if (!db)
        db= "";
      if (!tab_name)
        tab_name= "";

      Geometry_buffer buffer;
      Geometry *geom= NULL;
      String wkt;
      const char *dummy;
      wkt.set_charset(&my_charset_latin1);
      if (!(geom= Geometry::construct(&buffer, from, uint32(length))) ||
          geom->as_wkt(&wkt, &dummy))
        goto err;

      my_error(ER_TRUNCATED_WRONG_VALUE_FOR_FIELD, MYF(0),
               Geometry::ci_collection[m_type_handler->geometry_type()]->m_name.str,
	       wkt.c_ptr_safe(),
               db, tab_name, field_name.str,
               (ulong) table->in_use->get_stmt_da()->
               current_row_for_warning());
      goto err_exit;
    }

    Field_blob::store_length(length);
    if ((table->copy_blobs || length <= MAX_FIELD_WIDTH) &&
        from != value.ptr())
    {						// Must make a copy
      value.copy(from, length, cs);
      from= value.ptr();
    }
    bmove(ptr + packlength, &from, sizeof(char*));
  }
  return 0;

err:
  my_message(ER_CANT_CREATE_GEOMETRY_OBJECT,
             ER_THD(get_thd(), ER_CANT_CREATE_GEOMETRY_OBJECT), MYF(0));
err_exit:
  bzero(ptr, Field_blob::pack_length());
  return -1;
}


bool Field_geom::is_equal(const Column_definition &new_field) const
{
  /*
    - Allow ALTER..INPLACE to supertype (GEOMETRY),
      e.g. POINT to GEOMETRY or POLYGON to GEOMETRY.
    - Allow ALTER..INPLACE to the same geometry type: POINT -> POINT
  */
  if (new_field.type_handler() == m_type_handler)
    return true;
  const Type_handler_geometry *gth=
    dynamic_cast<const Type_handler_geometry*>(new_field.type_handler());
  return gth && gth->is_binary_compatible_geom_super_type_for(m_type_handler);
}


bool Field_geom::can_optimize_range(const Item_bool_func *cond,
                                    const Item *item,
                                    bool is_eq_func) const
{
  return item->cmp_type() == STRING_RESULT;
}


bool Field_geom::load_data_set_no_data(THD *thd, bool fixed_format)
{
  return Field_geom::load_data_set_null(thd);
}


bool Field_geom::load_data_set_null(THD *thd)
{
  Field_blob::reset();
  if (!maybe_null())
  {
    my_error(ER_WARN_NULL_TO_NOTNULL, MYF(0), field_name.str,
             thd->get_stmt_da()->current_row_for_warning());
    return true;
  }
  set_null();
  set_has_explicit_value(); // Do not auto-update this field
  return false;
}


uint Field_geom::get_key_image(uchar *buff,uint length, const uchar *ptr_arg,
                               imagetype type_arg) const
{
  if (type_arg == itMBR)
  {
    LEX_CSTRING tmp;
    tmp.str= (const char *) get_ptr(ptr_arg);
    tmp.length= get_length(ptr_arg);
    return Geometry::get_key_image_itMBR(tmp, buff, length);
  }
  return Field_blob::get_key_image_itRAW(ptr_arg, buff, length);
}

Binlog_type_info Field_geom::binlog_type_info() const
{
  DBUG_ASSERT(Field_geom::type() == binlog_type());
  return Binlog_type_info(Field_geom::type(), pack_length_no_ptr(), 1,
                          field_charset(), type_handler_geom()->geometry_type());
}

#endif // HAVE_SPATIAL

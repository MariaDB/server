#ifndef SQL_TYPE_GEOM_H_INCLUDED
#define SQL_TYPE_GEOM_H_INCLUDED
/*
   Copyright (c) 2015 MariaDB Foundation
   Copyright (c) 2019, 2022, MariaDB Corporation.

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

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_type.h"

#ifdef HAVE_SPATIAL
class Type_handler_geometry: public Type_handler_string_result
{
public:
  enum geometry_types
  {
    GEOM_GEOMETRY = 0, GEOM_POINT = 1, GEOM_LINESTRING = 2, GEOM_POLYGON = 3,
    GEOM_MULTIPOINT = 4, GEOM_MULTILINESTRING = 5, GEOM_MULTIPOLYGON = 6,
    GEOM_GEOMETRYCOLLECTION = 7
  };
  static bool check_type_geom_or_binary(const LEX_CSTRING &opname,
                                        const Item *item);
  static bool check_types_geom_or_binary(const LEX_CSTRING &opname,
                                         Item * const *args,
                                         uint start, uint end);
  static const Type_handler_geometry *type_handler_geom_by_type(uint type);
  LEX_CSTRING extended_metadata_data_type_name() const;
public:
  virtual ~Type_handler_geometry() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_GEOMETRY; }
  bool Item_append_extended_type_info(Send_field_extended_metadata *to,
                                      const Item *item) const override
  {
    LEX_CSTRING tmp= extended_metadata_data_type_name();
    return tmp.length ? to->set_data_type_name(tmp) : false;
  }
  bool is_param_long_data_type() const override { return true; }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  uint32 calc_pack_length(uint32 length) const override;
  const Type_collection *type_collection() const override;
  const Type_handler *type_handler_for_comparison() const override;
  virtual geometry_types geometry_type() const { return GEOM_GEOMETRY; }
  virtual Item *create_typecast_item(THD *thd, Item *item,
                                     const Type_cast_attributes &attr)
                                     const override;
  const Type_handler *type_handler_frm_unpack(const uchar *buffer)
                                              const override;
  bool is_binary_compatible_geom_super_type_for(const Type_handler_geometry *th)
                                                const
  {
    return geometry_type() == GEOM_GEOMETRY ||
           geometry_type() == th->geometry_type();
  }
  bool type_can_have_key_part() const override { return true; }
  bool subquery_type_allows_materialization(const Item *, const Item *, bool)
    const override
  {
    return false; // Materialization does not work with GEOMETRY columns
  }
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  Log_event_data_type user_var_log_event_data_type(uint charset_nr)
                                                              const override
  {
    return Log_event_data_type(name().lex_cstring(), result_type(),
                               charset_nr, false/*unsigned*/);
  }

  uint Column_definition_gis_options_image(uchar *buff,
                                           const Column_definition &def)
                                           const override;
  bool Column_definition_data_type_info_image(Binary_string *to,
                                              const Column_definition &def)
                                              const override
  {
    return false;
  }
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const override;
  bool
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options) const
    override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  void Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *c,
                                              const Field *field) const
    override;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        column_definition_type_t type,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override;
  bool Key_part_spec_init_primary(Key_part_spec *part,
                                  const Column_definition &def,
                                  const handler *file) const override;
  bool Key_part_spec_init_unique(Key_part_spec *part,
                                 const Column_definition &def,
                                 const handler *file,
                                 bool *has_key_needed) const override;
  bool Key_part_spec_init_multiple(Key_part_spec *part,
                                   const Column_definition &def,
                                   const handler *file) const override;
  bool Key_part_spec_init_foreign(Key_part_spec *part,
                                  const Column_definition &def,
                                  const handler *file) const override;
  bool Key_part_spec_init_spatial(Key_part_spec *part,
                                  const Column_definition &def) const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;

  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;

  bool can_return_int() const override { return false; }
  bool can_return_decimal() const override { return false; }
  bool can_return_real() const override { return false; }
  bool can_return_text() const override { return false; }
  bool can_return_date() const override { return false; }
  bool can_return_time() const override { return false; }
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const override;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const override;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const override;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *h,
                                       Type_all_attributes *attr,
                                       Item **items, uint nitems) const
    override;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const override;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const override;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const override;

  bool Item_func_signed_fix_length_and_dec(Item_func_signed *) const override;
  bool Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *) const
    override;
  bool Item_double_typecast_fix_length_and_dec(Item_double_typecast *) const
    override;
  bool Item_float_typecast_fix_length_and_dec(Item_float_typecast *) const
    override;
  bool Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *) const
    override;
  bool Item_char_typecast_fix_length_and_dec(Item_char_typecast *) const
    override;
  bool Item_time_typecast_fix_length_and_dec(Item_time_typecast *) const
    override;
  bool Item_date_typecast_fix_length_and_dec(Item_date_typecast *) const
    override;
  bool Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *) const
    override;
};


class Type_handler_point: public Type_handler_geometry
{
  // Binary length of a POINT value: 4 byte SRID + 21 byte WKB POINT
  static uint octet_length() { return 25; }
public:
  geometry_types geometry_type() const override { return GEOM_POINT; }
  Item *make_constructor_item(THD *thd, List<Item> *args) const override;
  bool Key_part_spec_init_primary(Key_part_spec *part,
                                  const Column_definition &def,
                                  const handler *file) const override;
  bool Key_part_spec_init_unique(Key_part_spec *part,
                                 const Column_definition &def,
                                 const handler *file,
                                 bool *has_key_needed) const override;
  bool Key_part_spec_init_multiple(Key_part_spec *part,
                                   const Column_definition &def,
                                   const handler *file) const override;
  bool Key_part_spec_init_foreign(Key_part_spec *part,
                                  const Column_definition &def,
                                  const handler *file) const override;
};


class Type_handler_linestring: public Type_handler_geometry
{
public:
  geometry_types geometry_type() const override { return GEOM_LINESTRING; }
  Item *make_constructor_item(THD *thd, List<Item> *args) const override;
};


class Type_handler_polygon: public Type_handler_geometry
{
public:
  geometry_types geometry_type() const override { return GEOM_POLYGON; }
  Item *make_constructor_item(THD *thd, List<Item> *args) const override;
};


class Type_handler_multipoint: public Type_handler_geometry
{
public:
  geometry_types geometry_type() const override { return GEOM_MULTIPOINT; }
  Item *make_constructor_item(THD *thd, List<Item> *args) const override;
};


class Type_handler_multilinestring: public Type_handler_geometry
{
public:
  geometry_types geometry_type() const override { return GEOM_MULTILINESTRING; }
  Item *make_constructor_item(THD *thd, List<Item> *args) const override;
};


class Type_handler_multipolygon: public Type_handler_geometry
{
public:
  geometry_types geometry_type() const override { return GEOM_MULTIPOLYGON; }
  Item *make_constructor_item(THD *thd, List<Item> *args) const override;
};


class Type_handler_geometrycollection: public Type_handler_geometry
{
public:
  geometry_types geometry_type() const override { return GEOM_GEOMETRYCOLLECTION; }
  Item *make_constructor_item(THD *thd, List<Item> *args) const override;
};

extern Named_type_handler<Type_handler_geometry> type_handler_geometry;
extern Named_type_handler<Type_handler_point> type_handler_point;
extern Named_type_handler<Type_handler_linestring> type_handler_linestring;
extern Named_type_handler<Type_handler_polygon> type_handler_polygon;
extern Named_type_handler<Type_handler_multipoint> type_handler_multipoint;
extern Named_type_handler<Type_handler_multilinestring> type_handler_multilinestring;
extern Named_type_handler<Type_handler_multipolygon> type_handler_multipolygon;
extern Named_type_handler<Type_handler_geometrycollection> type_handler_geometrycollection;

class Type_collection_geometry: public Type_collection
{
  const Type_handler *aggregate_common(const Type_handler *a,
                                       const Type_handler *b) const
  {
    if (a == b)
      return a;
    if (dynamic_cast<const Type_handler_geometry*>(a) &&
        dynamic_cast<const Type_handler_geometry*>(b))
      return &type_handler_geometry;
    return NULL;
  }
  const Type_handler *aggregate_if_null(const Type_handler *a,
                                        const Type_handler *b) const
  {
    return a == &type_handler_null ? b :
           b == &type_handler_null ? a :
           NULL;
  }
  const Type_handler *aggregate_if_long_blob(const Type_handler *a,
                                             const Type_handler *b) const
  {
    return a == &type_handler_long_blob ? &type_handler_long_blob :
           b == &type_handler_long_blob ? &type_handler_long_blob :
           NULL;
  }
  const Type_handler *aggregate_if_string(const Type_handler *a,
                                          const Type_handler *b) const;
#ifndef DBUG_OFF
  bool init_aggregators(Type_handler_data *data, const Type_handler *geom) const;
#endif
public:
  bool init(Type_handler_data *data) override;
  const Type_handler *aggregate_for_result(const Type_handler *a,
                                           const Type_handler *b)
                                           const override;
  const Type_handler *aggregate_for_comparison(const Type_handler *a,
                                               const Type_handler *b)
                                               const override;
  const Type_handler *aggregate_for_min_max(const Type_handler *a,
                                            const Type_handler *b)
                                            const override;
  const Type_handler *aggregate_for_num_op(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    return NULL;
  }
};

extern Type_collection_geometry type_collection_geometry;
const Type_handler *
Type_collection_geometry_handler_by_name(const LEX_CSTRING &name);

#include "field.h"

class Field_geom :public Field_blob
{
  const Type_handler_geometry *m_type_handler;
public:
  uint srid;
  uint precision;
  enum storage_type { GEOM_STORAGE_WKB= 0, GEOM_STORAGE_BINARY= 1};
  enum storage_type storage;

  Field_geom(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
	     enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
	     TABLE_SHARE *share, uint blob_pack_length,
	     const Type_handler_geometry *gth,
	     uint field_srid)
     :Field_blob(ptr_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
                 field_name_arg, share, blob_pack_length, &my_charset_bin),
      m_type_handler(gth)
  { srid= field_srid; }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  enum ha_base_keytype key_type() const  override
  {
    return HA_KEYTYPE_VARBINARY2;
  }
  const Type_handler *type_handler() const override
  {
    return m_type_handler;
  }
  const Type_handler_geometry *type_handler_geom() const
  {
    return m_type_handler;
  }
  void set_type_handler(const Type_handler_geometry *th)
  {
    m_type_handler= th;
  }
  enum_field_types type() const override
  {
    return MYSQL_TYPE_GEOMETRY;
  }
  enum_field_types real_type() const override
  {
    return MYSQL_TYPE_GEOMETRY;
  }
  Information_schema_character_attributes
    information_schema_character_attributes() const override
  {
    return Information_schema_character_attributes();
  }
  void make_send_field(Send_field *to) override
  {
    Field_longstr::make_send_field(to);
    LEX_CSTRING tmp= m_type_handler->extended_metadata_data_type_name();
    if (tmp.length)
      to->set_data_type_name(tmp);
  }
  Data_type_compatibility can_optimize_range(const Item_bool_func *cond,
                                             const Item *item,
                                             bool is_eq_func) const override;
  void sql_type(String &str) const override;
  Copy_func *get_copy_func(const Field *from) const override
  {
    const Type_handler_geometry *fth=
      dynamic_cast<const Type_handler_geometry*>(from->type_handler());
    if (fth && m_type_handler->is_binary_compatible_geom_super_type_for(fth))
      return get_identical_copy_func();
    return do_conv_blob;
  }
  bool memcpy_field_possible(const Field *from) const override
  {
    const Type_handler_geometry *fth=
      dynamic_cast<const Type_handler_geometry*>(from->type_handler());
    return fth &&
           m_type_handler->is_binary_compatible_geom_super_type_for(fth) &&
           !table->copy_blobs;
  }
  bool is_equal(const Column_definition &new_field) const override;
  int  store(const char *to, size_t length, CHARSET_INFO *charset) override;
  int  store(double nr) override;
  int  store(longlong nr, bool unsigned_val) override;
  int  store_decimal(const my_decimal *) override;
  uint size_of() const  override{ return sizeof(*this); }
  /**
   Key length is provided only to support hash joins. (compared byte for byte)
   Ex: SELECT .. FROM t1,t2 WHERE t1.field_geom1=t2.field_geom2.

   The comparison is not very relevant, as identical geometry might be
   represented differently, but we need to support it either way.
  */
  uint32 key_length() const  override{ return packlength; }
  uint get_key_image(uchar *buff,uint length,
                     const uchar *ptr_arg, imagetype type_arg) const override;

  /**
    Non-nullable GEOMETRY types cannot have defaults,
    but the underlying blob must still be reset.
   */
  int reset(void)  override{ return Field_blob::reset() || !maybe_null(); }
  bool load_data_set_null(THD *thd) override;
  bool load_data_set_no_data(THD *thd, bool fixed_format) override;

  uint get_srid() const { return srid; }
  void print_key_value(String *out, uint32 length) override
  {
    out->append(STRING_WITH_LEN("unprintable_geometry_value"));
  }
  Binlog_type_info binlog_type_info() const override;
};

#endif // HAVE_SPATIAL

#endif // SQL_TYPE_GEOM_H_INCLUDED

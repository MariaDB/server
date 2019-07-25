#ifndef SQL_TYPE_GEOM_H_INCLUDED
#define SQL_TYPE_GEOM_H_INCLUDED
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

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_type.h"

#ifdef HAVE_SPATIAL
class Type_handler_geometry: public Type_handler_string_result
{
  static const Name m_name_geometry;
public:
  enum geometry_types
  {
    GEOM_GEOMETRY = 0, GEOM_POINT = 1, GEOM_LINESTRING = 2, GEOM_POLYGON = 3,
    GEOM_MULTIPOINT = 4, GEOM_MULTILINESTRING = 5, GEOM_MULTIPOLYGON = 6,
    GEOM_GEOMETRYCOLLECTION = 7
  };
  static bool check_type_geom_or_binary(const char *opname, const Item *item);
  static bool check_types_geom_or_binary(const char *opname,
                                         Item * const *args,
                                         uint start, uint end);
  static const Type_handler_geometry *type_handler_geom_by_type(uint type);
public:
  virtual ~Type_handler_geometry() {}
  const Name name() const override { return m_name_geometry; }
  enum_field_types field_type() const override { return MYSQL_TYPE_GEOMETRY; }
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
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer) const override
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
  uint Column_definition_gis_options_image(uchar *buff,
                                           const Column_definition &def)
                                           const override;
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
                                        handler *file,
                                        ulonglong table_flags) const override;
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
                          TABLE *table) const override;

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
                                       const char *name,
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
  static const Name m_name_point;
  // Binary length of a POINT value: 4 byte SRID + 21 byte WKB POINT
  static uint octet_length() { return 25; }
public:
  geometry_types geometry_type() const override { return GEOM_POINT; }
  const Name name() const override { return m_name_point; }
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
  static const Name m_name_linestring;
public:
  geometry_types geometry_type() const { return GEOM_LINESTRING; }
  const Name name() const { return m_name_linestring; }
  Item *make_constructor_item(THD *thd, List<Item> *args) const override;
};


class Type_handler_polygon: public Type_handler_geometry
{
  static const Name m_name_polygon;
public:
  geometry_types geometry_type() const { return GEOM_POLYGON; }
  const Name name() const { return m_name_polygon; }
  Item *make_constructor_item(THD *thd, List<Item> *args) const override;
};


class Type_handler_multipoint: public Type_handler_geometry
{
  static const Name m_name_multipoint;
public:
  geometry_types geometry_type() const { return GEOM_MULTIPOINT; }
  const Name name() const { return m_name_multipoint; }
  Item *make_constructor_item(THD *thd, List<Item> *args) const override;
};


class Type_handler_multilinestring: public Type_handler_geometry
{
  static const Name m_name_multilinestring;
public:
  geometry_types geometry_type() const { return GEOM_MULTILINESTRING; }
  const Name name() const { return m_name_multilinestring; }
  Item *make_constructor_item(THD *thd, List<Item> *args) const override;
};


class Type_handler_multipolygon: public Type_handler_geometry
{
  static const Name m_name_multipolygon;
public:
  geometry_types geometry_type() const { return GEOM_MULTIPOLYGON; }
  const Name name() const { return m_name_multipolygon; }
  Item *make_constructor_item(THD *thd, List<Item> *args) const override;
};


class Type_handler_geometrycollection: public Type_handler_geometry
{
  static const Name m_name_geometrycollection;
public:
  geometry_types geometry_type() const { return GEOM_GEOMETRYCOLLECTION; }
  const Name name() const { return m_name_geometrycollection; }
  Item *make_constructor_item(THD *thd, List<Item> *args) const override;
};


extern MYSQL_PLUGIN_IMPORT Type_handler_geometry   type_handler_geometry;
extern MYSQL_PLUGIN_IMPORT Type_handler_point      type_handler_point;
extern MYSQL_PLUGIN_IMPORT Type_handler_linestring type_handler_linestring;
extern MYSQL_PLUGIN_IMPORT Type_handler_polygon    type_handler_polygon;
extern MYSQL_PLUGIN_IMPORT Type_handler_multipoint      type_handler_multipoint;
extern MYSQL_PLUGIN_IMPORT Type_handler_multilinestring type_handler_multilinestring;
extern MYSQL_PLUGIN_IMPORT Type_handler_multipolygon    type_handler_multipolygon;
extern MYSQL_PLUGIN_IMPORT Type_handler_geometrycollection type_handler_geometrycollection;


class Function_collection_geometry: public Function_collection
{
public:
  bool init() override;
  void cleanup() override;
  Create_func *find_native_function_builder(THD *thd,
                                            const LEX_CSTRING &name)
                                            const override;
};


class Type_collection_geometry: public Type_collection
{
  const Type_handler *aggregate_common(const Type_handler *a,
                                       const Type_handler *b) const
  {
    if (a == b)
      return a;
    DBUG_ASSERT(dynamic_cast<const Type_handler_geometry*>(a));
    DBUG_ASSERT(dynamic_cast<const Type_handler_geometry*>(b));
    return &type_handler_geometry;
  }
  bool init_aggregators(Type_handler_data *data, const Type_handler *geom) const;
public:
  bool init(Type_handler_data *data) override;
  const Type_handler *handler_by_name(const LEX_CSTRING &name) const override;
  const Type_handler *aggregate_for_result(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    return aggregate_common(a, b);
  }
  const Type_handler *aggregate_for_comparison(const Type_handler *a,
                                               const Type_handler *b)
                                               const override
  {
    return aggregate_common(a, b);
  }
  const Type_handler *aggregate_for_min_max(const Type_handler *a,
                                            const Type_handler *b)
                                            const override
  {
    return aggregate_common(a, b);
  }
  const Type_handler *aggregate_for_num_op(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    return NULL;
  }
};


extern MYSQL_PLUGIN_IMPORT
  Function_collection_geometry function_collection_geometry;

extern MYSQL_PLUGIN_IMPORT Type_collection_geometry type_collection_geometry;


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
                                    const Conv_param &param) const;
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_VARBINARY2; }
  const Type_handler *type_handler() const
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
  enum_field_types type() const
  {
    return MYSQL_TYPE_GEOMETRY;
  }
  enum_field_types real_type() const
  {
    return MYSQL_TYPE_GEOMETRY;
  }
  Information_schema_character_attributes
    information_schema_character_attributes() const
  {
    return Information_schema_character_attributes();
  }
  void make_send_field(Send_field *to)
  {
    Field_longstr::make_send_field(to);
  }
  bool can_optimize_range(const Item_bool_func *cond,
                                  const Item *item,
                                  bool is_eq_func) const;
  void sql_type(String &str) const;
  Copy_func *get_copy_func(const Field *from) const
  {
    const Type_handler_geometry *fth=
      dynamic_cast<const Type_handler_geometry*>(from->type_handler());
    if (fth && m_type_handler->is_binary_compatible_geom_super_type_for(fth))
      return get_identical_copy_func();
    return do_conv_blob;
  }
  bool memcpy_field_possible(const Field *from) const
  {
    const Type_handler_geometry *fth=
      dynamic_cast<const Type_handler_geometry*>(from->type_handler());
    return fth &&
           m_type_handler->is_binary_compatible_geom_super_type_for(fth) &&
           !table->copy_blobs;
  }
  bool is_equal(const Column_definition &new_field) const;
  bool can_be_converted_by_engine(const Column_definition &new_type) const
  {
    return false; // Override the Field_blob behavior
  }

  int  store(const char *to, size_t length, CHARSET_INFO *charset);
  int  store(double nr);
  int  store(longlong nr, bool unsigned_val);
  int  store_decimal(const my_decimal *);
  uint size_of() const { return sizeof(*this); }
  /**
   Key length is provided only to support hash joins. (compared byte for byte)
   Ex: SELECT .. FROM t1,t2 WHERE t1.field_geom1=t2.field_geom2.

   The comparison is not very relevant, as identical geometry might be
   represented differently, but we need to support it either way.
  */
  uint32 key_length() const { return packlength; }
  uint get_key_image(uchar *buff,uint length, imagetype type_arg);

  /**
    Non-nullable GEOMETRY types cannot have defaults,
    but the underlying blob must still be reset.
   */
  int reset(void) { return Field_blob::reset() || !maybe_null(); }
  bool load_data_set_null(THD *thd);
  bool load_data_set_no_data(THD *thd, bool fixed_format);

  uint get_srid() const { return srid; }
  void print_key_value(String *out, uint32 length)
  {
    out->append(STRING_WITH_LEN("unprintable_geometry_value"));
  }
};

#endif // HAVE_SPATIAL

#endif // SQL_TYPE_GEOM_H_INCLUDED

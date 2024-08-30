#ifndef ITEM_GEOFUNC_INCLUDED
#define ITEM_GEOFUNC_INCLUDED

/* Copyright (c) 2000, 2016 Oracle and/or its affiliates.
   Copyright (C) 2011, 2021, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */


/* This file defines all spatial functions */

#ifdef HAVE_SPATIAL

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "sql_type_geom.h"
#include "item.h"
#include "gstream.h"
#include "spatial.h"
#include "gcalc_slicescan.h"
#include "gcalc_tools.h"

class Item_geometry_func: public Item_str_func
{
public:
  Item_geometry_func(THD *thd): Item_str_func(thd) {}
  Item_geometry_func(THD *thd, Item *a): Item_str_func(thd, a) {}
  Item_geometry_func(THD *thd, Item *a, Item *b): Item_str_func(thd, a, b) {}
  Item_geometry_func(THD *thd, Item *a, Item *b, Item *c):
    Item_str_func(thd, a, b, c) {}
  Item_geometry_func(THD *thd, List<Item> &list): Item_str_func(thd, list) {}
  bool fix_length_and_dec(THD *thd) override;
  const Type_handler *type_handler() const override
  { return &type_handler_geometry; }
};


/*
  Functions returning REAL measurements of a single GEOMETRY argument
*/
class Item_real_func_args_geometry: public Item_real_func
{
protected:
  String value;
  bool check_arguments() const override
  {
    DBUG_ASSERT(arg_count == 1);
    return Type_handler_geometry::check_type_geom_or_binary(func_name_cstring(),
                                                            args[0]);
  }
public:
  Item_real_func_args_geometry(THD *thd, Item *a)
   :Item_real_func(thd, a) {}
};


/*
  Functions returning INT measurements of a single GEOMETRY argument
*/
class Item_long_func_args_geometry: public Item_long_func
{
  bool check_arguments() const override
  {
    DBUG_ASSERT(arg_count == 1);
    return Type_handler_geometry::check_type_geom_or_binary(func_name_cstring(),
                                                            args[0]);
  }
protected:
  String value;
public:
  Item_long_func_args_geometry(THD *thd, Item *a)
   :Item_long_func(thd, a) {}
};


/*
  Functions returning BOOL measurements of a single GEOMETRY argument
*/
class Item_bool_func_args_geometry: public Item_bool_func
{
protected:
  String value;
  bool check_arguments() const override
  {
    DBUG_ASSERT(arg_count == 1);
    return Type_handler_geometry::check_type_geom_or_binary(func_name_cstring(),
                                                            args[0]);
  }
public:
  Item_bool_func_args_geometry(THD *thd, Item *a)
   :Item_bool_func(thd, a) {}
};


/*
  Functions returning ASCII string measurements of a single GEOMETRY argument
*/
class Item_str_ascii_func_args_geometry: public Item_str_ascii_func
{
protected:
  bool check_arguments() const override
  {
    DBUG_ASSERT(arg_count >= 1);
    return Type_handler_geometry::check_type_geom_or_binary(func_name_cstring(),
                                                            args[0]);
  }
public:
  Item_str_ascii_func_args_geometry(THD *thd, Item *a)
   :Item_str_ascii_func(thd, a) {}
  Item_str_ascii_func_args_geometry(THD *thd, Item *a, Item *b)
   :Item_str_ascii_func(thd, a, b) {}
  Item_str_ascii_func_args_geometry(THD *thd, Item *a, Item *b, Item *c)
   :Item_str_ascii_func(thd, a, b, c) {}
};


/*
  Functions returning binary string measurements of a single GEOMETRY argument
*/
class Item_binary_func_args_geometry: public Item_str_func
{
protected:
  bool check_arguments() const override
  {
    DBUG_ASSERT(arg_count >= 1);
    return Type_handler_geometry::check_type_geom_or_binary(func_name_cstring(),
                                                            args[0]);
  }
public:
  Item_binary_func_args_geometry(THD *thd, Item *a)
   :Item_str_func(thd, a) {}
};


/*
  Functions returning GEOMETRY measurements of a single GEOEMETRY argument
*/
class Item_geometry_func_args_geometry: public Item_geometry_func
{
protected:
  bool check_arguments() const override
  {
    DBUG_ASSERT(arg_count >= 1);
    return Type_handler_geometry::check_type_geom_or_binary(func_name_cstring(),
                                                            args[0]);
  }
public:
  Item_geometry_func_args_geometry(THD *thd, Item *a)
   :Item_geometry_func(thd, a) {}
  Item_geometry_func_args_geometry(THD *thd, Item *a, Item *b)
   :Item_geometry_func(thd, a, b) {}
};


/*
  Functions returning REAL result relationships between two GEOMETRY arguments
*/
class Item_real_func_args_geometry_geometry: public Item_real_func
{
protected:
  bool check_arguments() const override
  {
    DBUG_ASSERT(arg_count >= 2);
    return Type_handler_geometry::check_types_geom_or_binary(func_name_cstring(),
                                                             args, 0, 2);
  }
public:
  Item_real_func_args_geometry_geometry(THD *thd, Item *a, Item *b)
   :Item_real_func(thd, a, b) {}
};


/*
  Functions returning BOOL result relationships between two GEOMETRY arguments
*/
class Item_bool_func_args_geometry_geometry: public Item_bool_func
{
protected:
  String value;
  bool check_arguments() const override
  {
    DBUG_ASSERT(arg_count >= 2);
    return Type_handler_geometry::check_types_geom_or_binary(func_name_cstring(),
                                                             args, 0, 2);
  }
public:
  Item_bool_func_args_geometry_geometry(THD *thd, Item *a, Item *b, Item *c)
   :Item_bool_func(thd, a, b, c) {}
};


class Item_func_geometry_from_text: public Item_geometry_func
{
  bool check_arguments() const override
  {
    return args[0]->check_type_general_purpose_string(func_name_cstring()) ||
           check_argument_types_can_return_int(1, MY_MIN(2, arg_count));
  }
public:
  Item_func_geometry_from_text(THD *thd, Item *a): Item_geometry_func(thd, a) {}
  Item_func_geometry_from_text(THD *thd, Item *a, Item *srid):
    Item_geometry_func(thd, a, srid) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_geometryfromtext") };
    return name;
  }
  String *val_str(String *) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_geometry_from_text>(thd, this); }
};

class Item_func_geometry_from_wkb: public Item_geometry_func
{
  bool check_arguments() const override
  {
    return
      Type_handler_geometry::check_type_geom_or_binary(func_name_cstring(), args[0]) ||
      check_argument_types_can_return_int(1, MY_MIN(2, arg_count));
  }
public:
  Item_func_geometry_from_wkb(THD *thd, Item *a): Item_geometry_func(thd, a) {}
  Item_func_geometry_from_wkb(THD *thd, Item *a, Item *srid):
    Item_geometry_func(thd, a, srid) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_geometryfromwkb") };
    return name;
  }
  String *val_str(String *) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_geometry_from_wkb>(thd, this); }
};


class Item_func_geometry_from_json: public Item_geometry_func
{
  String tmp_js;
  bool check_arguments() const override
  {
    // TODO: check with Alexey, for better args[1] and args[2] type control
    return args[0]->check_type_general_purpose_string(func_name_cstring()) ||
           check_argument_types_traditional_scalar(1, MY_MIN(3, arg_count));
  }
public:
  Item_func_geometry_from_json(THD *thd, Item *js): Item_geometry_func(thd, js) {}
  Item_func_geometry_from_json(THD *thd, Item *js, Item *opt):
    Item_geometry_func(thd, js, opt) {}
  Item_func_geometry_from_json(THD *thd, Item *js, Item *opt, Item *srid):
    Item_geometry_func(thd, js, opt, srid) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_geomfromgeojson") };
    return name;
  }
  String *val_str(String *) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_geometry_from_json>(thd, this); }
};


class Item_func_as_wkt: public Item_str_ascii_func_args_geometry
{
public:
  Item_func_as_wkt(THD *thd, Item *a)
   :Item_str_ascii_func_args_geometry(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_astext") };
    return name;
  }
  String *val_str_ascii(String *) override;
  bool fix_length_and_dec(THD *thd) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_as_wkt>(thd, this); }
};

class Item_func_as_wkb: public Item_binary_func_args_geometry
{
public:
  Item_func_as_wkb(THD *thd, Item *a)
   :Item_binary_func_args_geometry(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_aswkb") };
    return name;
  }
  String *val_str(String *) override;
  const Type_handler *type_handler() const override
  { return &type_handler_long_blob; }
  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(&my_charset_bin);
    decimals=0;
    max_length= (uint32) UINT_MAX32;
    set_maybe_null();
    return FALSE;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_as_wkb>(thd, this); }
};


class Item_func_as_geojson: public Item_str_ascii_func_args_geometry
{
  bool check_arguments() const override
  {
    // TODO: check with Alexey, for better args[1] and args[2] type control
    return Item_str_ascii_func_args_geometry::check_arguments() ||
           check_argument_types_traditional_scalar(1, MY_MIN(3, arg_count));
  }
public:
  Item_func_as_geojson(THD *thd, Item *js)
   :Item_str_ascii_func_args_geometry(thd, js) {}
  Item_func_as_geojson(THD *thd, Item *js, Item *max_dec_digits)
   :Item_str_ascii_func_args_geometry(thd, js, max_dec_digits) {}
  Item_func_as_geojson(THD *thd, Item *js, Item *max_dec_digits, Item *opt)
   :Item_str_ascii_func_args_geometry(thd, js, max_dec_digits, opt) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_asgeojson") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  String *val_str_ascii(String *) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_as_geojson>(thd, this); }
};


class Item_func_geometry_type: public Item_str_ascii_func_args_geometry
{
public:
  Item_func_geometry_type(THD *thd, Item *a)
   :Item_str_ascii_func_args_geometry(thd, a) {}
  String *val_str_ascii(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_geometrytype") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    // "GeometryCollection" is the longest
    fix_length_and_charset(20, default_charset());
    set_maybe_null();
    return FALSE;
  };
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_geometry_type>(thd, this); }
};


// #define HEAVY_CONVEX_HULL
class Item_func_convexhull: public Item_geometry_func_args_geometry
{
  class ch_node: public Gcalc_dyn_list::Item
  {
  public:
    const Gcalc_heap::Info *pi;
    ch_node *prev;
    Gcalc_dyn_list::Item *next;
    ch_node *get_next() { return (ch_node *) next; }
  };

  Gcalc_heap collector;
  Gcalc_function func;
  Gcalc_dyn_list res_heap;

  Gcalc_result_receiver res_receiver;
  String tmp_value;
#ifdef HEAVY_CONVEX_HULL
  Gcalc_scan_iterator scan_it;
#endif /*HEAVY_CONVEX_HULL*/
  ch_node *new_ch_node() { return (ch_node *) res_heap.new_item(); }
  int add_node_to_line(ch_node **p_cur, int dir, const Gcalc_heap::Info *pi);
public:
  Item_func_convexhull(THD *thd, Item *a)
   :Item_geometry_func_args_geometry(thd, a),
    res_heap(8192, sizeof(ch_node))
    {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_convexhull") };
    return name;
  }
  String *val_str(String *) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_convexhull>(thd, this); }
};


class Item_func_centroid: public Item_geometry_func_args_geometry
{
public:
  Item_func_centroid(THD *thd, Item *a)
   :Item_geometry_func_args_geometry(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_centroid") };
    return name;
  }
  String *val_str(String *) override;
  const Type_handler *type_handler() const override
  {
    return &type_handler_point;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_centroid>(thd, this); }
};

class Item_func_envelope: public Item_geometry_func_args_geometry
{
public:
  Item_func_envelope(THD *thd, Item *a)
   :Item_geometry_func_args_geometry(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_envelope") };
    return name;
  }
  String *val_str(String *) override;
  const Type_handler *type_handler() const override
  {
    return &type_handler_polygon;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_envelope>(thd, this); }
};


class Item_func_boundary: public Item_geometry_func_args_geometry
{
  class Transporter : public Gcalc_shape_transporter
  {
    Gcalc_result_receiver *m_receiver;
    uint n_points;
    Gcalc_function::shape_type current_type;
    double last_x, last_y;
  public:
    Transporter(Gcalc_result_receiver *receiver) :
      Gcalc_shape_transporter(NULL), m_receiver(receiver)
    {}
    int single_point(double x, double y) override;
    int start_line() override;
    int complete_line() override;
    int start_poly() override;
    int complete_poly() override;
    int start_ring() override;
    int complete_ring() override;
    int add_point(double x, double y) override;

    int start_collection(int n_objects) override;
  };
  Gcalc_result_receiver res_receiver;
public:
  Item_func_boundary(THD *thd, Item *a)
   :Item_geometry_func_args_geometry(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_boundary") };
    return name;
  }
  String *val_str(String *) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_boundary>(thd, this); }
};


class Item_func_point: public Item_geometry_func
{
  bool check_arguments() const override
  { return check_argument_types_can_return_real(0, 2); }
public:
  Item_func_point(THD *thd, Item *a, Item *b): Item_geometry_func(thd, a, b) {}
  Item_func_point(THD *thd, Item *a, Item *b, Item *srid):
    Item_geometry_func(thd, a, b, srid) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("point") };
    return name;
  }
  String *val_str(String *) override;
  const Type_handler *type_handler() const override
  {
    return &type_handler_point;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_point>(thd, this); }
};

class Item_func_spatial_decomp: public Item_geometry_func_args_geometry
{
  enum Functype decomp_func;
public:
  Item_func_spatial_decomp(THD *thd, Item *a, Item_func::Functype ft):
    Item_geometry_func_args_geometry(thd, a) { decomp_func = ft; }
  LEX_CSTRING func_name_cstring() const override
  { 
    static LEX_CSTRING startpoint= {STRING_WITH_LEN("st_startpoint") };
    static LEX_CSTRING endpoint= {STRING_WITH_LEN("st_endpoint") };
    static LEX_CSTRING exteriorring= {STRING_WITH_LEN("st_exteriorring") };
    static LEX_CSTRING unknown= {STRING_WITH_LEN("spatial_decomp_unknown") };
    switch (decomp_func) {
      case SP_STARTPOINT:
        return startpoint;
      case SP_ENDPOINT:
        return endpoint;
      case SP_EXTERIORRING:
        return exteriorring;
      default:
	DBUG_ASSERT(0);  // Should never happened
        return unknown;
    }
  }
  String *val_str(String *) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_spatial_decomp>(thd, this); }
};

class Item_func_spatial_decomp_n: public Item_geometry_func_args_geometry
{
  enum Functype decomp_func_n;
  bool check_arguments() const override
  {
    return Item_geometry_func_args_geometry::check_arguments() ||
           args[1]->check_type_can_return_int(func_name_cstring());
  }
public:
  Item_func_spatial_decomp_n(THD *thd, Item *a, Item *b, Item_func::Functype ft)
   :Item_geometry_func_args_geometry(thd, a, b),
    decomp_func_n(ft)
  { }
  LEX_CSTRING func_name_cstring() const override
  { 
    static LEX_CSTRING pointn= {STRING_WITH_LEN("st_pointn") };
    static LEX_CSTRING geometryn= {STRING_WITH_LEN("st_geometryn") };
    static LEX_CSTRING interiorringn= {STRING_WITH_LEN("st_interiorringn") };
    static LEX_CSTRING unknown= {STRING_WITH_LEN("spatial_decomp_unknown") };

    switch (decomp_func_n) {
      case SP_POINTN:
        return pointn;
      case SP_GEOMETRYN:
        return geometryn;
      case SP_INTERIORRINGN:
        return interiorringn;
      default:
	DBUG_ASSERT(0);  // Should never happened
        return unknown;
    }
  }
  String *val_str(String *) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_spatial_decomp_n>(thd, this); }
};

class Item_func_spatial_collection: public Item_geometry_func
{
  bool check_arguments() const override
  {
    return Type_handler_geometry::check_types_geom_or_binary(func_name_cstring(), args,
                                                             0, arg_count);
  }
  enum Geometry::wkbType coll_type; 
  enum Geometry::wkbType item_type;
public:
  Item_func_spatial_collection(THD *thd,
     List<Item> &list, enum Geometry::wkbType ct, enum Geometry::wkbType it):
  Item_geometry_func(thd, list)
  {
    coll_type=ct;
    item_type=it;
  }
  String *val_str(String *) override;
  bool fix_length_and_dec(THD *thd) override
  {
    if (Item_geometry_func::fix_length_and_dec(thd))
      return TRUE;
    for (unsigned int i= 0; i < arg_count; ++i)
    {
      if (args[i]->fixed() && args[i]->field_type() != MYSQL_TYPE_GEOMETRY)
      {
        String str;
        args[i]->print(&str, QT_NO_DATA_EXPANSION);
        str.append('\0');
        my_error(ER_ILLEGAL_VALUE_FOR_TYPE, MYF(0), "non geometric",
                 str.ptr());
        return TRUE;
      }
    }
    return FALSE;
  }
};


class Item_func_geometrycollection: public Item_func_spatial_collection
{
public:
  Item_func_geometrycollection(THD *thd, List<Item> &list)
   :Item_func_spatial_collection(thd, list,
                                 Geometry::wkb_geometrycollection,
                                 Geometry::wkb_point)
  { }
  const Type_handler *type_handler() const override
  {
    return &type_handler_geometrycollection;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("geometrycollection") };
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_geometrycollection>(thd, this); }
};


class Item_func_linestring: public Item_func_spatial_collection
{
public:
  Item_func_linestring(THD *thd, List<Item> &list)
   :Item_func_spatial_collection(thd, list,
                                 Geometry::wkb_linestring,
                                 Geometry::wkb_point)
  { }
  const Type_handler *type_handler() const override
  { return &type_handler_linestring; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("linestring") };
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_linestring>(thd, this); }
};


class Item_func_polygon: public Item_func_spatial_collection
{
public:
  Item_func_polygon(THD *thd, List<Item> &list)
   :Item_func_spatial_collection(thd, list,
                                 Geometry::wkb_polygon,
                                 Geometry::wkb_linestring)
  { }
  const Type_handler *type_handler() const override
  { return &type_handler_polygon; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("polygon") };
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_polygon>(thd, this); }
};


class Item_func_multilinestring: public Item_func_spatial_collection
{
public:
  Item_func_multilinestring(THD *thd, List<Item> &list)
   :Item_func_spatial_collection(thd, list,
                                 Geometry::wkb_multilinestring,
                                 Geometry::wkb_linestring)
  { }
  const Type_handler *type_handler() const override
  {
    return &type_handler_multilinestring;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("multilinestring") };
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_multilinestring>(thd, this); }
};


class Item_func_multipoint: public Item_func_spatial_collection
{
public:
  Item_func_multipoint(THD *thd, List<Item> &list)
   :Item_func_spatial_collection(thd, list,
                                 Geometry::wkb_multipoint,
                                 Geometry::wkb_point)
  { }
  const Type_handler *type_handler() const override
  {
    return &type_handler_multipoint;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("multipoint") };
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_multipoint>(thd, this); }
};


class Item_func_multipolygon: public Item_func_spatial_collection
{
public:
  Item_func_multipolygon(THD *thd, List<Item> &list)
   :Item_func_spatial_collection(thd, list,
                                 Geometry::wkb_multipolygon,
                                 Geometry::wkb_polygon)
  { }
  const Type_handler *type_handler() const override
  {
    return &type_handler_multipolygon;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("multipolygon") };
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_multipolygon>(thd, this); }
};



/*
  Spatial relations
*/

class Item_func_spatial_rel: public Item_bool_func2_with_rev
{
protected:
  enum Functype spatial_rel;
  String tmp_value1, tmp_value2;
  SEL_ARG *get_mm_leaf(RANGE_OPT_PARAM *param, Field *field,
                       KEY_PART *key_part,
                       Item_func::Functype type, Item *value) override;
  bool check_arguments() const override
  {
    DBUG_ASSERT(arg_count >= 2);
    return Type_handler_geometry::check_types_geom_or_binary(func_name_cstring(),
                                                             args, 0, 2);
  }
public:
  Item_func_spatial_rel(THD *thd, Item *a, Item *b, enum Functype sp_rel):
    Item_bool_func2_with_rev(thd, a, b), spatial_rel(sp_rel)
  {
    set_maybe_null();
  }
  enum Functype functype() const override { return spatial_rel; }
  enum Functype rev_functype() const override
  {
    switch (spatial_rel)
    {
      case SP_CONTAINS_FUNC:
        return SP_WITHIN_FUNC;
      case SP_WITHIN_FUNC:
        return SP_CONTAINS_FUNC;
      default:
        return spatial_rel;
    }
  }
  bool is_null() override { (void) val_int(); return null_value; }
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                      uint *and_level, table_map usable_tables,
                      SARGABLE_PARAM **sargables) override
  {
    return add_key_fields_optimize_op(join, key_fields, and_level,
                                      usable_tables, sargables, false);
  }
  bool need_parentheses_in_default() override { return false; }
  Item *do_build_clone(THD *thd) const override { return nullptr; }
};


class Item_func_spatial_mbr_rel: public Item_func_spatial_rel
{
public:
  Item_func_spatial_mbr_rel(THD *thd, Item *a, Item *b, enum Functype sp_rel):
    Item_func_spatial_rel(thd, a, b, sp_rel)
  { }
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_spatial_mbr_rel>(thd, this); }
};


class Item_func_spatial_precise_rel: public Item_func_spatial_rel
{
  Gcalc_heap collector;
  Gcalc_scan_iterator scan_it;
  Gcalc_function func;
public:
  Item_func_spatial_precise_rel(THD *thd, Item *a, Item *b, enum Functype sp_rel):
    Item_func_spatial_rel(thd, a, b, sp_rel), collector()
  { }
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_spatial_precise_rel>(thd, this); }
};


class Item_func_spatial_relate: public Item_bool_func_args_geometry_geometry
{
  Gcalc_heap collector;
  Gcalc_scan_iterator scan_it;
  Gcalc_function func;
  String tmp_value1, tmp_value2, tmp_matrix;
  bool check_arguments() const override
  {
    return Item_bool_func_args_geometry_geometry::check_arguments() ||
           args[2]->check_type_general_purpose_string(func_name_cstring());
  }
public:
  Item_func_spatial_relate(THD *thd, Item *a, Item *b, Item *matrix):
    Item_bool_func_args_geometry_geometry(thd, a, b, matrix)
  { }
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_relate") };
    return name;
  }
  bool need_parentheses_in_default() override { return false; }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_spatial_relate>(thd, this); }
};


/*
  Spatial operations
*/

class Item_func_spatial_operation final: public Item_geometry_func
{
  bool check_arguments() const override
  {
    DBUG_ASSERT(arg_count >= 2);
    return Type_handler_geometry::check_types_geom_or_binary(func_name_cstring(),
                                                             args, 0, 2);
  }
public:
  Gcalc_function::op_type spatial_op;
  Gcalc_heap collector;
  Gcalc_function func;

  Gcalc_result_receiver res_receiver;
  Gcalc_operation_reducer operation;
  String tmp_value1,tmp_value2;
public:
  Item_func_spatial_operation(THD *thd, Item *a,Item *b,
                              Gcalc_function::op_type sp_op):
    Item_geometry_func(thd, a, b), spatial_op(sp_op)
  {}
  virtual ~Item_func_spatial_operation();
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override;
  void print(String *str, enum_query_type query_type) override
  {
    Item_func::print(str, query_type);
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_spatial_operation>(thd, this); }
};


class Item_func_buffer final : public Item_geometry_func_args_geometry
{
  bool check_arguments() const override
  {
    return Item_geometry_func_args_geometry::check_arguments() ||
           args[1]->check_type_can_return_real(func_name_cstring());
  }
protected:
  class Transporter : public Gcalc_operation_transporter
  {
    int m_npoints;
    double m_d;
    double x1,y1,x2,y2;
    double x00,y00,x01,y01;
    int add_edge_buffer(double x3, double y3, bool round_p1, bool round_p2);
    int add_last_edge_buffer();
    int add_point_buffer(double x, double y);
    int complete();
    int m_nshapes;
    Gcalc_function::op_type buffer_op;
    int last_shape_pos;
    bool skip_line;

  public:
    Transporter(Gcalc_function *fn, Gcalc_heap *heap, double d) :
      Gcalc_operation_transporter(fn, heap), m_npoints(0), m_d(d),
      m_nshapes(0), buffer_op((d > 0.0) ? Gcalc_function::op_union :
                                          Gcalc_function::op_difference),
      skip_line(FALSE)
    {}
    int single_point(double x, double y) override;
    int start_line() override;
    int complete_line() override;
    int start_poly() override;
    int complete_poly() override;
    int start_ring() override;
    int complete_ring() override;
    int add_point(double x, double y) override;

    int start_collection(int n_objects) override;
  };
  Gcalc_heap collector;
  Gcalc_function func;

  Gcalc_result_receiver res_receiver;
  Gcalc_operation_reducer operation;

public:
  Item_func_buffer(THD *thd, Item *obj, Item *distance)
   :Item_geometry_func_args_geometry(thd, obj, distance) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_buffer") };
    return name;
  }
  String *val_str(String *) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_buffer>(thd, this); }
};


class Item_func_isempty: public Item_bool_func_args_geometry
{
public:
  Item_func_isempty(THD *thd, Item *a)
   :Item_bool_func_args_geometry(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_isempty") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  { set_maybe_null(); return FALSE; }
  bool need_parentheses_in_default() override { return false; }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_isempty>(thd, this); }
};

class Item_func_issimple: public Item_long_func_args_geometry
{
  Gcalc_heap collector;
  Gcalc_function func;
  Gcalc_scan_iterator scan_it;
  String tmp;
public:
  Item_func_issimple(THD *thd, Item *a)
   :Item_long_func_args_geometry(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_issimple") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override { decimals=0; max_length=2; return FALSE; }
  decimal_digits_t decimal_precision() const override { return 1; }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_issimple>(thd, this); }
};

class Item_func_isclosed: public Item_long_func_args_geometry
{
public:
  Item_func_isclosed(THD *thd, Item *a)
   :Item_long_func_args_geometry(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_isclosed") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override { decimals=0; max_length=2; return FALSE; }
  decimal_digits_t decimal_precision() const override { return 1; }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_isclosed>(thd, this); }
};

class Item_func_isring: public Item_func_issimple
{
public:
  Item_func_isring(THD *thd, Item *a): Item_func_issimple(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_isring") };
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_isring>(thd, this); }
};

class Item_func_dimension: public Item_long_func_args_geometry
{
public:
  Item_func_dimension(THD *thd, Item *a)
   :Item_long_func_args_geometry(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_dimension") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  { max_length= 10; set_maybe_null(); return FALSE; }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_dimension>(thd, this); }
};


class Item_func_x: public Item_real_func_args_geometry
{
public:
  Item_func_x(THD *thd, Item *a): Item_real_func_args_geometry(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_x") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    if (Item_real_func::fix_length_and_dec(thd))
      return TRUE;
    set_maybe_null();
    return FALSE;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_x>(thd, this); }
};


class Item_func_y: public Item_real_func_args_geometry
{
public:
  Item_func_y(THD *thd, Item *a): Item_real_func_args_geometry(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_y") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    if (Item_real_func::fix_length_and_dec(thd))
      return TRUE;
    set_maybe_null();
    return FALSE;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_y>(thd, this); }
};


class Item_func_numgeometries: public Item_long_func_args_geometry
{
public:
  Item_func_numgeometries(THD *thd, Item *a)
   :Item_long_func_args_geometry(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_numgeometries") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  { max_length= 10; set_maybe_null(); return FALSE; }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_numgeometries>(thd, this); }
};


class Item_func_numinteriorring: public Item_long_func_args_geometry
{
public:
  Item_func_numinteriorring(THD *thd, Item *a)
   :Item_long_func_args_geometry(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_numinteriorrings") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  { max_length= 10; set_maybe_null(); return FALSE; }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_numinteriorring>(thd, this); }
};


class Item_func_numpoints: public Item_long_func_args_geometry
{
public:
  Item_func_numpoints(THD *thd, Item *a)
   :Item_long_func_args_geometry(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_numpoints") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  { max_length= 10; set_maybe_null(); return FALSE; }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_numpoints>(thd, this); }
};


class Item_func_area: public Item_real_func_args_geometry
{
public:
  Item_func_area(THD *thd, Item *a): Item_real_func_args_geometry(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_area") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    if (Item_real_func::fix_length_and_dec(thd))
      return TRUE;
    set_maybe_null();
    return FALSE;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_area>(thd, this); }
};


class Item_func_glength: public Item_real_func_args_geometry
{
  String value;
public:
  Item_func_glength(THD *thd, Item *a)
   :Item_real_func_args_geometry(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_length") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    if (Item_real_func::fix_length_and_dec(thd))
      return TRUE;
    set_maybe_null();
    return FALSE;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_glength>(thd, this); }
};


class Item_func_srid: public Item_long_func_args_geometry
{
public:
  Item_func_srid(THD *thd, Item *a)
   :Item_long_func_args_geometry(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("srid") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  { max_length= 10; set_maybe_null(); return FALSE; }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_srid>(thd, this); }
};


class Item_func_distance: public Item_real_func_args_geometry_geometry
{
  String tmp_value1;
  String tmp_value2;
  Gcalc_heap collector;
  Gcalc_function func;
  Gcalc_scan_iterator scan_it;
public:
  Item_func_distance(THD *thd, Item *a, Item *b)
   :Item_real_func_args_geometry_geometry(thd, a, b) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_distance") };
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_distance>(thd, this); }
};


class Item_func_sphere_distance: public Item_real_func
{
  double spherical_distance_points(Geometry *g1, Geometry *g2,
                                   const double sphere_r);
public:
  Item_func_sphere_distance(THD *thd, List<Item> &list):
    Item_real_func(thd, list) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_distance_sphere") };
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_sphere_distance>(thd, this); }
};


class Item_func_pointonsurface: public Item_geometry_func_args_geometry
{
  String tmp_value;
  Gcalc_heap collector;
  Gcalc_function func;
  Gcalc_scan_iterator scan_it;
public:
  Item_func_pointonsurface(THD *thd, Item *a)
   :Item_geometry_func_args_geometry(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_pointonsurface") };
    return name;
  }
  String *val_str(String *) override;
  const Type_handler *type_handler() const override
  {
    return &type_handler_point;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_pointonsurface>(thd, this); }
};


#ifndef DBUG_OFF
class Item_func_gis_debug: public Item_long_func
{
  public:
    Item_func_gis_debug(THD *thd, Item *a): Item_long_func(thd, a)
    { null_value= false; }
    bool fix_length_and_dec(THD *thd) override { fix_char_length(10); return FALSE; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("st_gis_debug") };
    return name;
  }
    longlong val_int() override;
    bool check_vcol_func_processor(void *arg) override
    {
      return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
    }
    Item *do_get_copy(THD *thd) const override
    { return get_item_copy<Item_func_gis_debug>(thd, this); }
};
#endif


#define GEOM_NEW(thd, obj_constructor) new (thd->mem_root) obj_constructor
#define GEOM_TYPE(x) (x)

#else /*HAVE_SPATIAL*/

#define GEOM_NEW(thd, obj_constructor) NULL
#define GEOM_TYPE(x) NULL

#endif /*HAVE_SPATIAL*/
#endif /* ITEM_GEOFUNC_INCLUDED */

#ifndef ITEM_GEOFUNC_INCLUDED
#define ITEM_GEOFUNC_INCLUDED

/* Copyright (c) 2000, 2016 Oracle and/or its affiliates.
   Copyright (C) 2011, 2016, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */


/* This file defines all spatial functions */

#ifdef HAVE_SPATIAL

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

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
  void fix_length_and_dec();
  enum_field_types field_type() const  { return MYSQL_TYPE_GEOMETRY; }
  Field *tmp_table_field(TABLE *t_arg);
};

class Item_func_geometry_from_text: public Item_geometry_func
{
public:
  Item_func_geometry_from_text(THD *thd, Item *a): Item_geometry_func(thd, a) {}
  Item_func_geometry_from_text(THD *thd, Item *a, Item *srid):
    Item_geometry_func(thd, a, srid) {}
  const char *func_name() const { return "st_geometryfromtext"; }
  String *val_str(String *);
};

class Item_func_geometry_from_wkb: public Item_geometry_func
{
public:
  Item_func_geometry_from_wkb(THD *thd, Item *a): Item_geometry_func(thd, a) {}
  Item_func_geometry_from_wkb(THD *thd, Item *a, Item *srid):
    Item_geometry_func(thd, a, srid) {}
  const char *func_name() const { return "st_geometryfromwkb"; }
  String *val_str(String *);
};

class Item_func_as_wkt: public Item_str_ascii_func
{
public:
  Item_func_as_wkt(THD *thd, Item *a): Item_str_ascii_func(thd, a) {}
  const char *func_name() const { return "st_astext"; }
  String *val_str_ascii(String *);
  void fix_length_and_dec();
};

class Item_func_as_wkb: public Item_geometry_func
{
public:
  Item_func_as_wkb(THD *thd, Item *a): Item_geometry_func(thd, a) {}
  const char *func_name() const { return "st_aswkb"; }
  String *val_str(String *);
  enum_field_types field_type() const  { return MYSQL_TYPE_BLOB; }
};

class Item_func_geometry_type: public Item_str_ascii_func
{
public:
  Item_func_geometry_type(THD *thd, Item *a): Item_str_ascii_func(thd, a) {}
  String *val_str_ascii(String *);
  const char *func_name() const { return "st_geometrytype"; }
  void fix_length_and_dec() 
  {
    // "GeometryCollection" is the longest
    fix_length_and_charset(20, default_charset());
    maybe_null= 1;
  };
};


// #define HEAVY_CONVEX_HULL
class Item_func_convexhull: public Item_geometry_func
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
  Item_func_convexhull(THD *thd, Item *a): Item_geometry_func(thd, a),
    res_heap(8192, sizeof(ch_node))
    {}
  const char *func_name() const { return "st_convexhull"; }
  String *val_str(String *);
};


class Item_func_centroid: public Item_geometry_func
{
public:
  Item_func_centroid(THD *thd, Item *a): Item_geometry_func(thd, a) {}
  const char *func_name() const { return "st_centroid"; }
  String *val_str(String *);
  Field::geometry_type get_geometry_type() const;
};

class Item_func_envelope: public Item_geometry_func
{
public:
  Item_func_envelope(THD *thd, Item *a): Item_geometry_func(thd, a) {}
  const char *func_name() const { return "st_envelope"; }
  String *val_str(String *);
  Field::geometry_type get_geometry_type() const;
};


class Item_func_boundary: public Item_geometry_func
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
    int single_point(double x, double y);
    int start_line();
    int complete_line();
    int start_poly();
    int complete_poly();
    int start_ring();
    int complete_ring();
    int add_point(double x, double y);

    int start_collection(int n_objects);
  };
  Gcalc_result_receiver res_receiver;
public:
  Item_func_boundary(THD *thd, Item *a): Item_geometry_func(thd, a) {}
  const char *func_name() const { return "st_boundary"; }
  String *val_str(String *);
};


class Item_func_point: public Item_geometry_func
{
public:
  Item_func_point(THD *thd, Item *a, Item *b): Item_geometry_func(thd, a, b) {}
  Item_func_point(THD *thd, Item *a, Item *b, Item *srid):
    Item_geometry_func(thd, a, b, srid) {}
  const char *func_name() const { return "point"; }
  String *val_str(String *);
  Field::geometry_type get_geometry_type() const;
};

class Item_func_spatial_decomp: public Item_geometry_func
{
  enum Functype decomp_func;
public:
  Item_func_spatial_decomp(THD *thd, Item *a, Item_func::Functype ft):
    Item_geometry_func(thd, a) { decomp_func = ft; }
  const char *func_name() const 
  { 
    switch (decomp_func)
    {
      case SP_STARTPOINT:
        return "st_startpoint";
      case SP_ENDPOINT:
        return "st_endpoint";
      case SP_EXTERIORRING:
        return "st_exteriorring";
      default:
	DBUG_ASSERT(0);  // Should never happened
        return "spatial_decomp_unknown"; 
    }
  }
  String *val_str(String *);
};

class Item_func_spatial_decomp_n: public Item_geometry_func
{
  enum Functype decomp_func_n;
public:
  Item_func_spatial_decomp_n(THD *thd, Item *a, Item *b, Item_func::Functype ft):
    Item_geometry_func(thd, a, b) { decomp_func_n = ft; }
  const char *func_name() const 
  { 
    switch (decomp_func_n)
    {
      case SP_POINTN:
        return "st_pointn";
      case SP_GEOMETRYN:
        return "st_geometryn";
      case SP_INTERIORRINGN:
        return "st_interiorringn";
      default:
	DBUG_ASSERT(0);  // Should never happened
        return "spatial_decomp_n_unknown"; 
    }
  }
  String *val_str(String *);
};

class Item_func_spatial_collection: public Item_geometry_func
{
  String tmp_value;
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
  String *val_str(String *);
  void fix_length_and_dec()
  {
    Item_geometry_func::fix_length_and_dec();
    for (unsigned int i= 0; i < arg_count; ++i)
    {
      if (args[i]->fixed && args[i]->field_type() != MYSQL_TYPE_GEOMETRY)
      {
        String str;
        args[i]->print(&str, QT_NO_DATA_EXPANSION);
        str.append('\0');
        my_error(ER_ILLEGAL_VALUE_FOR_TYPE, MYF(0), "non geometric",
                 str.ptr());
      }
    }
  }
 
  const char *func_name() const { return "st_multipoint"; }
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
                       Item_func::Functype type, Item *value);
public:
  Item_func_spatial_rel(THD *thd, Item *a, Item *b, enum Functype sp_rel):
    Item_bool_func2_with_rev(thd, a, b), spatial_rel(sp_rel)
  {
    maybe_null= true;
  }
  enum Functype functype() const { return spatial_rel; }
  enum Functype rev_functype() const { return spatial_rel; }
  bool is_null() { (void) val_int(); return null_value; }
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                      uint *and_level, table_map usable_tables,
                      SARGABLE_PARAM **sargables)
  {
    return add_key_fields_optimize_op(join, key_fields, and_level,
                                      usable_tables, sargables, false);
  }
};


class Item_func_spatial_mbr_rel: public Item_func_spatial_rel
{
public:
  Item_func_spatial_mbr_rel(THD *thd, Item *a, Item *b, enum Functype sp_rel):
    Item_func_spatial_rel(thd, a, b, sp_rel)
  { }
  longlong val_int();
  const char *func_name() const;
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
  longlong val_int();
  const char *func_name() const;
};


class Item_func_spatial_relate: public Item_bool_func
{
  Gcalc_heap collector;
  Gcalc_scan_iterator scan_it;
  Gcalc_function func;
  String tmp_value1, tmp_value2, tmp_matrix;
public:
  Item_func_spatial_relate(THD *thd, Item *a, Item *b, Item *matrix):
    Item_bool_func(thd, a, b, matrix)
  { }
  longlong val_int();
  const char *func_name() const { return "st_relate"; }
};


/*
  Spatial operations
*/

class Item_func_spatial_operation: public Item_geometry_func
{
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
  String *val_str(String *);
  const char *func_name() const;
  virtual inline void print(String *str, enum_query_type query_type)
  {
    Item_func::print(str, query_type);
  }
};


class Item_func_buffer: public Item_geometry_func
{
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
    int single_point(double x, double y);
    int start_line();
    int complete_line();
    int start_poly();
    int complete_poly();
    int start_ring();
    int complete_ring();
    int add_point(double x, double y);

    int start_collection(int n_objects);
  };
  Gcalc_heap collector;
  Gcalc_function func;

  Gcalc_result_receiver res_receiver;
  Gcalc_operation_reducer operation;
  String tmp_value;

public:
  Item_func_buffer(THD *thd, Item *obj, Item *distance):
    Item_geometry_func(thd, obj, distance) {}
  const char *func_name() const { return "st_buffer"; }
  String *val_str(String *);
};


class Item_func_isempty: public Item_bool_func
{
public:
  Item_func_isempty(THD *thd, Item *a): Item_bool_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "st_isempty"; }
  void fix_length_and_dec() { maybe_null= 1; }
};

class Item_func_issimple: public Item_int_func
{
  Gcalc_heap collector;
  Gcalc_function func;
  Gcalc_scan_iterator scan_it;
  String tmp;
public:
  Item_func_issimple(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "st_issimple"; }
  void fix_length_and_dec() { decimals=0; max_length=2; }
  uint decimal_precision() const { return 1; }
};

class Item_func_isclosed: public Item_int_func
{
public:
  Item_func_isclosed(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "st_isclosed"; }
  void fix_length_and_dec() { decimals=0; max_length=2; }
  uint decimal_precision() const { return 1; }
};

class Item_func_isring: public Item_func_issimple
{
public:
  Item_func_isring(THD *thd, Item *a): Item_func_issimple(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "st_isring"; }
};

class Item_func_dimension: public Item_int_func
{
  String value;
public:
  Item_func_dimension(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "st_dimension"; }
  void fix_length_and_dec() { max_length= 10; maybe_null= 1; }
};

class Item_func_x: public Item_real_func
{
  String value;
public:
  Item_func_x(THD *thd, Item *a): Item_real_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "st_x"; }
  void fix_length_and_dec() 
  { 
    Item_real_func::fix_length_and_dec();
    maybe_null= 1; 
  }
};


class Item_func_y: public Item_real_func
{
  String value;
public:
  Item_func_y(THD *thd, Item *a): Item_real_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "st_y"; }
  void fix_length_and_dec() 
  { 
    Item_real_func::fix_length_and_dec();
    maybe_null= 1; 
  }
};


class Item_func_numgeometries: public Item_int_func
{
  String value;
public:
  Item_func_numgeometries(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "st_numgeometries"; }
  void fix_length_and_dec() { max_length= 10; maybe_null= 1; }
};


class Item_func_numinteriorring: public Item_int_func
{
  String value;
public:
  Item_func_numinteriorring(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "st_numinteriorrings"; }
  void fix_length_and_dec() { max_length= 10; maybe_null= 1; }
};


class Item_func_numpoints: public Item_int_func
{
  String value;
public:
  Item_func_numpoints(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "st_numpoints"; }
  void fix_length_and_dec() { max_length= 10; maybe_null= 1; }
};


class Item_func_area: public Item_real_func
{
  String value;
public:
  Item_func_area(THD *thd, Item *a): Item_real_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "st_area"; }
  void fix_length_and_dec() 
  { 
    Item_real_func::fix_length_and_dec();
    maybe_null= 1; 
  }
};


class Item_func_glength: public Item_real_func
{
  String value;
public:
  Item_func_glength(THD *thd, Item *a): Item_real_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "st_length"; }
  void fix_length_and_dec() 
  { 
    Item_real_func::fix_length_and_dec();
    maybe_null= 1; 
  }
};


class Item_func_srid: public Item_int_func
{
  String value;
public:
  Item_func_srid(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "srid"; }
  void fix_length_and_dec() { max_length= 10; maybe_null= 1; }
};


class Item_func_distance: public Item_real_func
{
  String tmp_value1;
  String tmp_value2;
  Gcalc_heap collector;
  Gcalc_function func;
  Gcalc_scan_iterator scan_it;
public:
  Item_func_distance(THD *thd, Item *a, Item *b): Item_real_func(thd, a, b) {}
  double val_real();
  const char *func_name() const { return "st_distance"; }
};


class Item_func_pointonsurface: public Item_geometry_func
{
  String tmp_value;
  Gcalc_heap collector;
  Gcalc_function func;
  Gcalc_scan_iterator scan_it;
public:
  Item_func_pointonsurface(THD *thd, Item *a): Item_geometry_func(thd, a) {}
  const char *func_name() const { return "st_pointonsurface"; }
  String *val_str(String *);
  Field::geometry_type get_geometry_type() const;
};


#ifndef DBUG_OFF
class Item_func_gis_debug: public Item_int_func
{
  public:
    Item_func_gis_debug(THD *thd, Item *a): Item_int_func(thd, a)
    { null_value= false; }
    const char *func_name() const  { return "st_gis_debug"; }
    longlong val_int();
};
#endif


#define GEOM_NEW(thd, obj_constructor) new (thd->mem_root) obj_constructor

#else /*HAVE_SPATIAL*/

#define GEOM_NEW(thd, obj_constructor) NULL

#endif /*HAVE_SPATIAL*/
#endif /* ITEM_GEOFUNC_INCLUDED */

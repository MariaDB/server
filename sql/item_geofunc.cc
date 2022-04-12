/* Copyright (c) 2003, 2016, Oracle and/or its affiliates.
   Copyright (c) 2011, 2022, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


/**
  @file

  @brief
  This file defines all spatial functions
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
/*
  It is necessary to include set_var.h instead of item.h because there
  are dependencies on include order for set_var.h and item.h. This
  will be resolved later.
*/
#include "sql_class.h"                          // THD, set_var.h: THD
#include "set_var.h"
#ifdef HAVE_SPATIAL
#include <m_ctype.h>
#include "opt_range.h"
#include "item_geofunc.h"
#include "item_create.h"


bool Item_geometry_func::fix_length_and_dec(THD *thd)
{
  collation.set(&my_charset_bin);
  decimals=0;
  max_length= (uint32) UINT_MAX32;
  set_maybe_null();
  return FALSE;
}


String *Item_func_geometry_from_text::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  Geometry_buffer buffer;
  String arg_val;
  String *wkt= args[0]->val_str_ascii(&arg_val);

  if ((null_value= args[0]->null_value))
    return 0;

  Gis_read_stream trs(wkt->charset(), wkt->ptr(), wkt->length());
  uint32 srid= 0;

  if ((arg_count == 2) && !args[1]->null_value)
    srid= (uint32)args[1]->val_int();

  str->set_charset(&my_charset_bin);
  str->length(0);
  if (str->reserve(SRID_SIZE, 512))
    return 0;
  str->q_append(srid);
  if ((null_value= !Geometry::create_from_wkt(&buffer, &trs, str, 0)))
    return 0;
  return str;
}


String *Item_func_geometry_from_wkb::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String arg_val;
  String *wkb;
  Geometry_buffer buffer;
  uint32 srid= 0;

  if (args[0]->field_type() == MYSQL_TYPE_GEOMETRY)
  {
    String *str_ret= args[0]->val_str(str);
    null_value= args[0]->null_value;
    return str_ret;
  }

  wkb= args[0]->val_str(&arg_val);

  if ((arg_count == 2) && !args[1]->null_value)
    srid= (uint32)args[1]->val_int();

  str->set_charset(&my_charset_bin);
  str->length(0);
  if (str->reserve(SRID_SIZE, 512))
  {
    null_value= TRUE;                           /* purecov: inspected */
    return 0;                                   /* purecov: inspected */
  }
  str->q_append(srid);
  if ((null_value= 
        (args[0]->null_value ||
         !Geometry::create_from_wkb(&buffer, wkb->ptr(), wkb->length(), str))))
    return 0;
  return str;
}


String *Item_func_geometry_from_json::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  Geometry_buffer buffer;
  String *js= args[0]->val_str_ascii(&tmp_js);
  uint32 srid= 0;
  longlong options= 0;
  json_engine_t je;

  if ((null_value= args[0]->null_value))
    return 0;

  if (arg_count > 1 && !args[1]->null_value)
  {
    options= args[1]->val_int();
    if (options > 4 || options < 1)
    {
      String *sv= args[1]->val_str(&tmp_js);
      my_error(ER_WRONG_VALUE_FOR_TYPE, MYF(0),
               "option", sv->c_ptr_safe(), "ST_GeomFromGeoJSON");
      null_value= 1;
      return 0;
    }
  }

  if ((arg_count == 3) && !args[2]->null_value)
    srid= (uint32)args[2]->val_int();

  str->set_charset(&my_charset_bin);
  str->length(0);
  if (str->reserve(SRID_SIZE, 512))
    return 0;
  str->q_append(srid);

  json_scan_start(&je, js->charset(), (const uchar *) js->ptr(),
                  (const uchar *) js->end());

  if ((null_value= !Geometry::create_from_json(&buffer, &je, options==1,  str)))
  {
    int code= 0;

    switch (je.s.error)
    {
    case Geometry::GEOJ_INCORRECT_GEOJSON:
      code= ER_GEOJSON_INCORRECT;
      break;
    case Geometry::GEOJ_TOO_FEW_POINTS:
      code= ER_GEOJSON_TOO_FEW_POINTS;
      break;
    case Geometry::GEOJ_EMPTY_COORDINATES:
      code= ER_GEOJSON_EMPTY_COORDINATES;
      break;
    case Geometry::GEOJ_POLYGON_NOT_CLOSED:
      code= ER_GEOJSON_NOT_CLOSED;
      break;
    case Geometry::GEOJ_DIMENSION_NOT_SUPPORTED:
      my_error(ER_GIS_INVALID_DATA, MYF(0), "ST_GeomFromGeoJSON");
      break;
    default:
      report_json_error_ex(js->ptr(), &je, func_name(), 0,
                           Sql_condition::WARN_LEVEL_WARN);
      return NULL;
    }

    if (code)
    {
      THD *thd= current_thd;
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, code,
                          ER_THD(thd, code));
    }
    return 0;
  }
  return str;
}


String *Item_func_as_wkt::val_str_ascii(String *str)
{
  DBUG_ASSERT(fixed());
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  Geometry_buffer buffer;
  Geometry *geom= NULL;
  const char *dummy;

  if ((null_value=
       (args[0]->null_value ||
	!(geom= Geometry::construct(&buffer, swkb->ptr(), swkb->length())))))
    return 0;

  str->length(0);
  str->set_charset(&my_charset_latin1);
  if ((null_value= geom->as_wkt(str, &dummy)))
    return 0;

  return str;
}


bool Item_func_as_wkt::fix_length_and_dec(THD *thd)
{
  collation.set(default_charset(), DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
  max_length= (uint32) UINT_MAX32;
  set_maybe_null();
  return FALSE;
}


String *Item_func_as_wkb::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  Geometry_buffer buffer;

  if ((null_value=
       (args[0]->null_value ||
	!(Geometry::construct(&buffer, swkb->ptr(), swkb->length())))))
    return 0;

  str->copy(swkb->ptr() + SRID_SIZE, swkb->length() - SRID_SIZE,
	    &my_charset_bin);
  return str;
}


bool Item_func_as_geojson::fix_length_and_dec(THD *thd)
{
  collation.set(default_charset(), DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
  max_length=MAX_BLOB_WIDTH;
  set_maybe_null();
  return FALSE;
}


String *Item_func_as_geojson::val_str_ascii(String *str)
{
  DBUG_ASSERT(fixed());
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  uint max_dec= FLOATING_POINT_DECIMALS;
  longlong options= 0;
  Geometry_buffer buffer;
  Geometry *geom= NULL;
  const char *dummy;

  if ((null_value=
       (args[0]->null_value ||
	!(geom= Geometry::construct(&buffer, swkb->ptr(), swkb->length())))))
    return 0;

  if (arg_count > 1)
  {
    max_dec= (uint) args[1]->val_int();
    if (args[1]->null_value)
      max_dec= FLOATING_POINT_DECIMALS;
    if (arg_count > 2)
    {
      options= args[2]->val_int();
      if (args[2]->null_value)
        options= 0;
    }
  }

  str->length(0);
  str->set_charset(&my_charset_latin1);

  if (str->reserve(1, 512))
    return 0;

  str->qs_append('{');

  if (options & 1)
  {
    if (geom->bbox_as_json(str) || str->append(", ", 2))
      goto error;
  }

  if ((geom->as_json(str, max_dec, &dummy) || str->append('}')))
      goto error;

  return str;

error:
  null_value= 1;
  return 0;
}


String *Item_func_geometry_type::val_str_ascii(String *str)
{
  DBUG_ASSERT(fixed());
  String *swkb= args[0]->val_str(str);
  Geometry_buffer buffer;
  Geometry *geom= NULL;

  if ((null_value=
       (args[0]->null_value ||
	!(geom= Geometry::construct(&buffer, swkb->ptr(), swkb->length())))))
    return 0;
  /* String will not move */
  str->copy(geom->get_class_info()->m_name.str,
	    geom->get_class_info()->m_name.length,
            &my_charset_latin1);
  return str;
}


String *Item_func_envelope::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  Geometry_buffer buffer;
  Geometry *geom= NULL;
  uint32 srid;
  
  if ((null_value=
       args[0]->null_value ||
       !(geom= Geometry::construct(&buffer, swkb->ptr(), swkb->length()))))
    return 0;
  
  srid= uint4korr(swkb->ptr());
  str->set_charset(&my_charset_bin);
  str->length(0);
  if (str->reserve(SRID_SIZE, 512))
    return 0;
  str->q_append(srid);
  return (null_value= geom->envelope(str)) ? 0 : str;
}


int Item_func_boundary::Transporter::single_point(double x, double y)
{
  return 0;
}


int Item_func_boundary::Transporter::start_line()
{
  n_points= 0;
  current_type= Gcalc_function::shape_line;
  return 0;
}


int Item_func_boundary::Transporter::complete_line()
{
  current_type= (Gcalc_function::shape_type) 0;
  if (n_points > 1)
    return m_receiver->single_point(last_x, last_y);
  return 0;
}


int Item_func_boundary::Transporter::start_poly()
{
  current_type= Gcalc_function::shape_polygon;
  return 0;
}


int Item_func_boundary::Transporter::complete_poly()
{
  current_type= (Gcalc_function::shape_type) 0;
  return 0;
}


int Item_func_boundary::Transporter::start_ring()
{
  n_points= 0;
  return m_receiver->start_shape(Gcalc_function::shape_line);
}


int Item_func_boundary::Transporter::complete_ring()
{
  if (n_points > 1)
  {
     m_receiver->add_point(last_x, last_y);
  }
  m_receiver->complete_shape();
  return 0;
}


int Item_func_boundary::Transporter::add_point(double x, double y)
{
  ++n_points;
  if (current_type== Gcalc_function::shape_polygon)
  {
    /* Polygon's ring case */
    if (n_points == 1)
    {
      last_x= x;
      last_y= y;
    }
    return m_receiver->add_point(x, y);
  }
  
  if (current_type== Gcalc_function::shape_line)
  {
    /* Line's case */
    last_x= x;
    last_y= y;
    if (n_points == 1)
      return m_receiver->single_point(x, y);
  }
  return 0;
}


int Item_func_boundary::Transporter::start_collection(int n_objects)
{
  return 0;
}


String *Item_func_boundary::val_str(String *str_value)
{
  DBUG_ENTER("Item_func_boundary::val_str");
  DBUG_ASSERT(fixed());
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);

  if ((null_value= args[0]->null_value))
    DBUG_RETURN(0);

  Geometry_buffer buffer;
  uint32 srid= 0;
  Transporter trn(&res_receiver);

  Geometry *g= Geometry::construct(&buffer, swkb->ptr(), swkb->length());
  if (!g)
    DBUG_RETURN(0);

  if (g->store_shapes(&trn))
    goto mem_error;

  str_value->set_charset(&my_charset_bin);
  str_value->length(0);
  if (str_value->reserve(SRID_SIZE, 512))
    goto mem_error;
  str_value->q_append(srid);

  if (!Geometry::create_from_opresult(&buffer, str_value, res_receiver))
    goto mem_error;

  res_receiver.reset();
  DBUG_RETURN(str_value);

mem_error:
  null_value= 1;
  DBUG_RETURN(0);
}


String *Item_func_centroid::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  Geometry_buffer buffer;
  Geometry *geom= NULL;
  uint32 srid;

  if ((null_value= args[0]->null_value ||
       !(geom= Geometry::construct(&buffer, swkb->ptr(), swkb->length()))))
    return 0;

  str->set_charset(&my_charset_bin);
  str->length(0);
  if (str->reserve(SRID_SIZE, 512))
    return 0;
  srid= uint4korr(swkb->ptr());
  str->q_append(srid);

  return (null_value= MY_TEST(geom->centroid(str))) ? 0 : str;
}


int Item_func_convexhull::add_node_to_line(ch_node **p_cur, int dir,
                                           const Gcalc_heap::Info *pi)
{
  ch_node *new_node;
  ch_node *cur= *p_cur;

  while (cur->prev)
  {
    int v_sign= Gcalc_scan_iterator::point::cmp_dx_dy(
                  cur->prev->pi, cur->pi, cur->pi, pi);
    if (v_sign*dir <0)
      break;
    new_node= cur;
    cur= cur->prev;
    res_heap.free_item(new_node);
  }
  if (!(new_node= new_ch_node()))
    return 1;
  cur->next= new_node;
  new_node->prev= cur;
  new_node->pi= pi;
  *p_cur= new_node;
  return 0;
}


#ifndef HEAVY_CONVEX_HULL
String *Item_func_convexhull::val_str(String *str_value)
{
  Geometry_buffer buffer;
  Geometry *geom= NULL;
  MBR mbr;
  const char *c_end;
  Gcalc_operation_transporter trn(&func, &collector);
  uint32 srid= 0;
  ch_node *left_first, *left_cur, *right_first, *right_cur;
  Gcalc_heap::Info *cur_pi;
  
  DBUG_ENTER("Item_func_convexhull::val_str");
  DBUG_ASSERT(fixed());
  String *swkb= args[0]->val_str(&tmp_value);

  if ((null_value=
       args[0]->null_value ||
       !(geom= Geometry::construct(&buffer, swkb->ptr(), swkb->length()))))
    DBUG_RETURN(0);
  
  geom->get_mbr(&mbr, &c_end);
  collector.set_extent(mbr.xmin, mbr.xmax, mbr.ymin, mbr.ymax);
  if ((null_value= geom->store_shapes(&trn)))
  {
    str_value= 0;
    goto mem_error;
  }

  collector.prepare_operation();
  if (!(cur_pi= collector.get_first()))
    goto build_result; /* An EMPTY GEOMETRY */

  if (!cur_pi->get_next())
  {
    /* Single point. */
    if (res_receiver.single_point(cur_pi->node.shape.x, cur_pi->node.shape.y))
      goto mem_error;
    goto build_result;
  }

  left_cur= left_first= new_ch_node();
  right_cur= right_first= new_ch_node();
  right_first->prev= left_first->prev= 0;
  right_first->pi= left_first->pi= cur_pi;

  while ((cur_pi= cur_pi->get_next()))
  {
    /* Handle left part of the hull, then the right part. */
    if (add_node_to_line(&left_cur, 1, cur_pi))
      goto mem_error;
    if (add_node_to_line(&right_cur, -1, cur_pi))
      goto mem_error;
  }

  left_cur->next= 0;
  if (left_first->get_next()->get_next() == NULL &&
      right_cur->prev->prev == NULL)
  {
    /* We only have 2 nodes in the result, so we create a polyline. */
    if (res_receiver.start_shape(Gcalc_function::shape_line) ||
        res_receiver.add_point(left_first->pi->node.shape.x, left_first->pi->node.shape.y) ||
        res_receiver.add_point(left_cur->pi->node.shape.x, left_cur->pi->node.shape.y) ||
        res_receiver.complete_shape())

      goto mem_error;

    goto build_result;
  }

  if (res_receiver.start_shape(Gcalc_function::shape_polygon))
    goto mem_error;

  while (left_first)
  {
    if (res_receiver.add_point(left_first->pi->node.shape.x, left_first->pi->node.shape.y))
      goto mem_error;
    left_first= left_first->get_next();
  }

  /* Skip last point in the right part as it coincides */
  /* with the last one in the left.                    */
  right_cur= right_cur->prev;
  while (right_cur->prev)
  {
    if (res_receiver.add_point(right_cur->pi->node.shape.x, right_cur->pi->node.shape.y))
      goto mem_error;
    right_cur= right_cur->prev;
  }
  res_receiver.complete_shape();

build_result:
  str_value->set_charset(&my_charset_bin);
  str_value->length(0);
  if (str_value->reserve(SRID_SIZE, 512))
    goto mem_error;
  str_value->q_append(srid);

  if (!Geometry::create_from_opresult(&buffer, str_value, res_receiver))
    goto mem_error;

mem_error:
  collector.reset();
  func.reset();
  res_receiver.reset();
  res_heap.reset();
  DBUG_RETURN(str_value);
}

#else /*HEAVY_CONVEX_HULL*/
String *Item_func_convexhull::val_str(String *str_value)
{
  Geometry_buffer buffer;
  Geometry *geom= NULL;
  MBR mbr;
  const char *c_end;
  Gcalc_operation_transporter trn(&func, &collector);
  const Gcalc_scan_iterator::event_point *ev;
  uint32 srid= 0;
  ch_node *left_first, *left_cur, *right_first, *right_cur;
  
  DBUG_ENTER("Item_func_convexhull::val_str");
  DBUG_ASSERT(fixed());
  String *swkb= args[0]->val_str(&tmp_value);

  if ((null_value=
       args[0]->null_value ||
       !(geom= Geometry::construct(&buffer, swkb->ptr(), swkb->length()))))
    DBUG_RETURN(0);
  
  geom->get_mbr(&mbr, &c_end);
  collector.set_extent(mbr.xmin, mbr.xmax, mbr.ymin, mbr.ymax);
  if ((null_value= geom->store_shapes(&trn)))
  {
    str_value= 0;
    goto mem_error;
  }

  collector.prepare_operation();
  scan_it.init(&collector);
  scan_it.killed= (int *) &(current_thd->killed);

  if (!scan_it.more_points())
    goto build_result; /* An EMPTY GEOMETRY */

  if (scan_it.step())
    goto mem_error;

  if (!scan_it.more_points())
  {
    /* Single point. */
    if (res_receiver.single_point(scan_it.get_events()->pi->x,
                                  scan_it.get_events()->pi->y))
      goto mem_error;
    goto build_result;
  }

  left_cur= left_first= new_ch_node();
  right_cur= right_first= new_ch_node();
  right_first->prev= left_first->prev= 0;
  right_first->pi= left_first->pi= scan_it.get_events()->pi;

  while (scan_it.more_points())
  {
    if (scan_it.step())
      goto mem_error;
    ev= scan_it.get_events();
    
    /* Skip the intersections-only events. */
    while (ev->event == scev_intersection)
    {
      ev= ev->get_next();
      if (!ev)
        goto skip_point;
    }

    {
      Gcalc_point_iterator pit(&scan_it);
      if (!pit.point() || scan_it.get_event_position() == pit.point())
      {
        /* Handle left part of the hull. */
        if (add_node_to_line(&left_cur, 1, ev->pi))
          goto mem_error;
      }
      if (pit.point())
      {
        /* Check the rightmost point */
        for(; pit.point()->c_get_next(); ++pit)
          ;
      }
      if (!pit.point() || pit.point()->event ||
          scan_it.get_event_position() == pit.point()->c_get_next())
      {
        /* Handle right part of the hull. */
        if (add_node_to_line(&right_cur, -1, ev->pi))
          goto mem_error;
      }
    }
skip_point:;
  }

  left_cur->next= 0;
  if (left_first->get_next()->get_next() == NULL &&
      right_cur->prev->prev == NULL)
  {
    /* We only have 2 nodes in the result, so we create a polyline. */
    if (res_receiver.start_shape(Gcalc_function::shape_line) ||
        res_receiver.add_point(left_first->pi->x, left_first->pi->y) ||
        res_receiver.add_point(left_cur->pi->x, left_cur->pi->y) ||
        res_receiver.complete_shape())

      goto mem_error;

    goto build_result;
  }

  if (res_receiver.start_shape(Gcalc_function::shape_polygon))
    goto mem_error;

  while (left_first)
  {
    if (res_receiver.add_point(left_first->pi->x, left_first->pi->y))
      goto mem_error;
    left_first= left_first->get_next();
  }

  /* Skip last point in the right part as it coincides */
  /* with the last one in the left.                    */
  right_cur= right_cur->prev;
  while (right_cur->prev)
  {
    if (res_receiver.add_point(right_cur->pi->x, right_cur->pi->y))
      goto mem_error;
    right_cur= right_cur->prev;
  }
  res_receiver.complete_shape();

build_result:
  str_value->set_charset(&my_charset_bin);
  str_value->length(0);
  if (str_value->reserve(SRID_SIZE, 512))
    goto mem_error;
  str_value->q_append(srid);

  if (!Geometry::create_from_opresult(&buffer, str_value, res_receiver))
    goto mem_error;

mem_error:
  collector.reset();
  func.reset();
  res_receiver.reset();
  res_heap.reset();
  DBUG_RETURN(str_value);
}
#endif /*HEAVY_CONVEX_HULL*/


/*
  Spatial decomposition functions
*/

String *Item_func_spatial_decomp::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  Geometry_buffer buffer;
  Geometry *geom= NULL;
  uint32 srid;

  if ((null_value=
       (args[0]->null_value ||
	!(geom= Geometry::construct(&buffer, swkb->ptr(), swkb->length())))))
    return 0;

  srid= uint4korr(swkb->ptr());
  str->set_charset(&my_charset_bin);
  str->length(0);
  if (str->reserve(SRID_SIZE, 512))
    goto err;
  str->q_append(srid);
  switch (decomp_func) {
    case SP_STARTPOINT:
      if (geom->start_point(str))
        goto err;
      break;

    case SP_ENDPOINT:
      if (geom->end_point(str))
        goto err;
      break;

    case SP_EXTERIORRING:
      if (geom->exterior_ring(str))
        goto err;
      break;

    default:
      goto err;
  }
  return str;

err:
  null_value= 1;
  return 0;
}


String *Item_func_spatial_decomp_n::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  long n= (long) args[1]->val_int();
  Geometry_buffer buffer;
  Geometry *geom= NULL;
  uint32 srid;

  if ((null_value=
       (args[0]->null_value || args[1]->null_value ||
	!(geom= Geometry::construct(&buffer, swkb->ptr(), swkb->length())))))
    return 0;

  str->set_charset(&my_charset_bin);
  str->length(0);
  if (str->reserve(SRID_SIZE, 512))
    goto err;
  srid= uint4korr(swkb->ptr());
  str->q_append(srid);
  switch (decomp_func_n)
  {
    case SP_POINTN:
      if (geom->point_n(n,str))
        goto err;
      break;

    case SP_GEOMETRYN:
      if (geom->geometry_n(n,str))
        goto err;
      break;

    case SP_INTERIORRINGN:
      if (geom->interior_ring_n(n,str))
        goto err;
      break;

    default:
      goto err;
  }
  return str;

err:
  null_value=1;
  return 0;
}


/*
  Functions to concatenate various spatial objects
*/


/*
*  Concatenate doubles into Point
*/


String *Item_func_point::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  double x= args[0]->val_real();
  double y= args[1]->val_real();
  uint32 srid= 0;

  if ((null_value= (args[0]->null_value ||
                    args[1]->null_value ||
                    str->alloc(4/*SRID*/ + 1 + 4 + SIZEOF_STORED_DOUBLE * 2))))
    return 0;

  str->set_charset(&my_charset_bin);
  str->length(0);
  str->q_append(srid);
  str->q_append((char)Geometry::wkb_ndr);
  str->q_append((uint32)Geometry::wkb_point);
  str->q_append(x);
  str->q_append(y);
  return str;
}


/**
  Concatenates various items into various collections
  with checkings for valid wkb type of items.
  For example, MultiPoint can be a collection of Points only.
  coll_type contains wkb type of target collection.
  item_type contains a valid wkb type of items.
  In the case when coll_type is wkbGeometryCollection,
  we do not check wkb type of items, any is valid.
*/

String *Item_func_spatial_collection::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String arg_value;
  uint i;
  uint32 srid= 0;

  str->set_charset(&my_charset_bin);
  str->length(0);
  if (str->reserve(4/*SRID*/ + 1 + 4 + 4, 512))
    goto err;

  str->q_append(srid);
  str->q_append((char) Geometry::wkb_ndr);
  str->q_append((uint32) coll_type);
  str->q_append((uint32) arg_count);

  for (i= 0; i < arg_count; ++i)
  {
    String *res= args[i]->val_str(&arg_value);
    uint32 len;
    if (args[i]->null_value || ((len= res->length()) < WKB_HEADER_SIZE))
      goto err;

    if (coll_type == Geometry::wkb_geometrycollection)
    {
      /*
	In the case of GeometryCollection we don't need any checkings
	for item types, so just copy them into target collection
      */
      if (str->append(res->ptr() + 4/*SRID*/, len - 4/*SRID*/, (uint32) 512))
        goto err;
    }
    else
    {
      enum Geometry::wkbType wkb_type;
      const uint data_offset= 4/*SRID*/ + 1;
      if (res->length() < data_offset + sizeof(uint32))
        goto err;
      const char *data= res->ptr() + data_offset;

      /*
	In the case of named collection we must check that items
	are of specific type, let's do this checking now
      */

      wkb_type= (Geometry::wkbType) uint4korr(data);
      data+= 4;
      len-= 5 + 4/*SRID*/;
      if (wkb_type != item_type)
        goto err;

      switch (coll_type) {
      case Geometry::wkb_multipoint:
      case Geometry::wkb_multilinestring:
      case Geometry::wkb_multipolygon:
	if (len < WKB_HEADER_SIZE ||
	    str->append(data-WKB_HEADER_SIZE, len+WKB_HEADER_SIZE, 512))
	  goto err;
	break;

      case Geometry::wkb_linestring:
	if (len < POINT_DATA_SIZE || str->append(data, POINT_DATA_SIZE, 512))
	  goto err;
	break;
      case Geometry::wkb_polygon:
      {
	uint32 n_points;
	double x1, y1, x2, y2;
	const char *org_data= data;

	if (len < 4)
	  goto err;

	n_points= uint4korr(data);
	data+= 4;

        if (n_points < 2 || len < 4 + n_points * POINT_DATA_SIZE)
          goto err;
        
	float8get(x1, data);
	data+= SIZEOF_STORED_DOUBLE;
	float8get(y1, data);
	data+= SIZEOF_STORED_DOUBLE;

	data+= (n_points - 2) * POINT_DATA_SIZE;

	float8get(x2, data);
	float8get(y2, data + SIZEOF_STORED_DOUBLE);

	if ((x1 != x2) || (y1 != y2) ||
	    str->append(org_data, len, 512))
	  goto err;
      }
      break;

      default:
	goto err;
      }
    }
  }
  if (str->length() > current_thd->variables.max_allowed_packet)
  {
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WARN_ALLOWED_PACKET_OVERFLOWED,
			ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
			func_name(), thd->variables.max_allowed_packet);
    goto err;
  }

  null_value = 0;
  return str;

err:
  null_value= 1;
  return 0;
}


/*
  Functions for spatial relations
*/

static SEL_ARG sel_arg_impossible(SEL_ARG::IMPOSSIBLE);

SEL_ARG *
Item_func_spatial_rel::get_mm_leaf(RANGE_OPT_PARAM *param,
                                   Field *field, KEY_PART *key_part,
                                   Item_func::Functype type, Item *value)
{
  DBUG_ENTER("Item_func_spatial_rel::get_mm_leaf");
  if (key_part->image_type != Field::itMBR)
    DBUG_RETURN(0);
  if (value->cmp_type() != STRING_RESULT)
    DBUG_RETURN(&sel_arg_impossible);

  if (param->using_real_indexes &&
      !field->optimize_range(param->real_keynr[key_part->key],
                             key_part->part))
    DBUG_RETURN(0);

  Field_geom *field_geom= dynamic_cast<Field_geom*>(field);
  DBUG_ASSERT(field_geom);
  const Type_handler_geometry *sav_geom_type= field_geom->type_handler_geom();
  // We have to be able to store all sorts of spatial features here
  field_geom->set_type_handler(&type_handler_geometry);
  bool rc= value->save_in_field_no_warnings(field, 1);
  field_geom->set_type_handler(sav_geom_type);

  if (rc)
    DBUG_RETURN(&sel_arg_impossible);            // Bad GEOMETRY value

  DBUG_ASSERT(!field->real_maybe_null()); // SPATIAL keys do not support NULL

  uchar *str= (uchar*) alloc_root(param->mem_root, key_part->store_length + 1);
  if (!str)
    DBUG_RETURN(0);                              // out of memory
  field->get_key_image(str, key_part->length, key_part->image_type);
  SEL_ARG *tree;

  if (!(tree= new (param->mem_root) SEL_ARG(field, str, str)))
    DBUG_RETURN(0);                              // out of memory

  switch (type) {
  case SP_EQUALS_FUNC:
    tree->min_flag= GEOM_FLAG | HA_READ_MBR_EQUAL;// NEAR_MIN;//512;
    tree->max_flag= NO_MAX_RANGE;
    break;
  case SP_DISJOINT_FUNC:
    tree->min_flag= GEOM_FLAG | HA_READ_MBR_DISJOINT;// NEAR_MIN;//512;
    tree->max_flag= NO_MAX_RANGE;
    break;
  case SP_INTERSECTS_FUNC:
    tree->min_flag= GEOM_FLAG | HA_READ_MBR_INTERSECT;// NEAR_MIN;//512;
    tree->max_flag= NO_MAX_RANGE;
    break;
  case SP_TOUCHES_FUNC:
    tree->min_flag= GEOM_FLAG | HA_READ_MBR_INTERSECT;// NEAR_MIN;//512;
    tree->max_flag= NO_MAX_RANGE;
    break;
  case SP_CROSSES_FUNC:
    tree->min_flag= GEOM_FLAG | HA_READ_MBR_INTERSECT;// NEAR_MIN;//512;
    tree->max_flag= NO_MAX_RANGE;
    break;
  case SP_WITHIN_FUNC:
    tree->min_flag= GEOM_FLAG | HA_READ_MBR_CONTAIN;// NEAR_MIN;//512;
    tree->max_flag= NO_MAX_RANGE;
    break;
  case SP_CONTAINS_FUNC:
    tree->min_flag= GEOM_FLAG | HA_READ_MBR_WITHIN;// NEAR_MIN;//512;
    tree->max_flag= NO_MAX_RANGE;
    break;
  case SP_OVERLAPS_FUNC:
    tree->min_flag= GEOM_FLAG | HA_READ_MBR_INTERSECT;// NEAR_MIN;//512;
    tree->max_flag= NO_MAX_RANGE;
    break;
  default:
    DBUG_ASSERT(0);
    break;
  }
  DBUG_RETURN(tree);
}


LEX_CSTRING Item_func_spatial_mbr_rel::func_name_cstring() const
{ 
  switch (spatial_rel) {
    case SP_CONTAINS_FUNC:
      return { STRING_WITH_LEN("mbrcontains") };
    case SP_WITHIN_FUNC:
      return { STRING_WITH_LEN("mbrwithin") } ;
    case SP_EQUALS_FUNC:
      return { STRING_WITH_LEN("mbrequals") };
    case SP_DISJOINT_FUNC:
      return { STRING_WITH_LEN("mbrdisjoint") };
    case SP_INTERSECTS_FUNC:
      return { STRING_WITH_LEN("mbrintersects") };
    case SP_TOUCHES_FUNC:
      return { STRING_WITH_LEN("mbrtouches") };
    case SP_CROSSES_FUNC:
      return { STRING_WITH_LEN("mbrcrosses") };
    case SP_OVERLAPS_FUNC:
      return { STRING_WITH_LEN("mbroverlaps") };
    default:
      DBUG_ASSERT(0);  // Should never happened
      return { STRING_WITH_LEN("mbrsp_unknown") };
  }
}


longlong Item_func_spatial_mbr_rel::val_int()
{
  DBUG_ASSERT(fixed());
  String *res1= args[0]->val_str(&tmp_value1);
  String *res2= args[1]->val_str(&tmp_value2);
  Geometry_buffer buffer1, buffer2;
  Geometry *g1, *g2;
  MBR mbr1, mbr2;
  const char *dummy;

  if ((null_value=
       (args[0]->null_value ||
	args[1]->null_value ||
	!(g1= Geometry::construct(&buffer1, res1->ptr(), res1->length())) ||
	!(g2= Geometry::construct(&buffer2, res2->ptr(), res2->length())) ||
	g1->get_mbr(&mbr1, &dummy) || !mbr1.valid() ||
	g2->get_mbr(&mbr2, &dummy) || !mbr2.valid())))
   return 0;

  switch (spatial_rel) {
    case SP_CONTAINS_FUNC:
      return mbr1.contains(&mbr2);
    case SP_WITHIN_FUNC:
      return mbr1.within(&mbr2);
    case SP_EQUALS_FUNC:
      return mbr1.equals(&mbr2);
    case SP_DISJOINT_FUNC:
      return mbr1.disjoint(&mbr2);
    case SP_INTERSECTS_FUNC:
      return mbr1.intersects(&mbr2);
    case SP_TOUCHES_FUNC:
      return mbr1.touches(&mbr2);
    case SP_OVERLAPS_FUNC:
      return mbr1.overlaps(&mbr2);
    case SP_CROSSES_FUNC:
      return 0;
    default:
      break;
  }

  null_value=1;
  return 0;
}


LEX_CSTRING Item_func_spatial_precise_rel::func_name_cstring() const
{ 
  switch (spatial_rel) {
    case SP_CONTAINS_FUNC:
      return { STRING_WITH_LEN("st_contains") };
    case SP_WITHIN_FUNC:
      return { STRING_WITH_LEN("st_within") };
    case SP_EQUALS_FUNC:
      return { STRING_WITH_LEN("st_equals") };
    case SP_DISJOINT_FUNC:
      return { STRING_WITH_LEN("st_disjoint") };
    case SP_INTERSECTS_FUNC:
      return { STRING_WITH_LEN("st_intersects") };
    case SP_TOUCHES_FUNC:
      return { STRING_WITH_LEN("st_touches") };
    case SP_CROSSES_FUNC:
      return { STRING_WITH_LEN("st_crosses") };
    case SP_OVERLAPS_FUNC:
      return { STRING_WITH_LEN("st_overlaps") } ;
    default:
      DBUG_ASSERT(0);  // Should never happened
      return { STRING_WITH_LEN("sp_unknown") };
  }
}


static double count_edge_t(const Gcalc_heap::Info *ea,
                           const Gcalc_heap::Info *eb,
                           const Gcalc_heap::Info *v,
                           double &ex, double &ey, double &vx, double &vy,
                           double &e_sqrlen)
{
  ex= eb->node.shape.x - ea->node.shape.x;
  ey= eb->node.shape.y - ea->node.shape.y;
  vx= v->node.shape.x - ea->node.shape.x;
  vy= v->node.shape.y - ea->node.shape.y;
  e_sqrlen= ex * ex + ey * ey;
  return (ex * vx + ey * vy) / e_sqrlen;
}


static double distance_to_line(double ex, double ey, double vx, double vy,
                               double e_sqrlen)
{
  return fabs(vx * ey - vy * ex) / sqrt(e_sqrlen);
}


static double distance_points(const Gcalc_heap::Info *a,
                              const Gcalc_heap::Info *b)
{
  double x= a->node.shape.x - b->node.shape.x;
  double y= a->node.shape.y - b->node.shape.y;
  return sqrt(x * x + y * y);
}


static Gcalc_function::op_type op_matrix(int n)
{
  switch (n)
  {
    case 0:
      return Gcalc_function::op_internals;
    case 1:
      return Gcalc_function::op_border;
    case 2:
      return (Gcalc_function::op_type)
        ((int) Gcalc_function::op_not | (int) Gcalc_function::op_union);
  };
  GCALC_DBUG_ASSERT(FALSE);
  return Gcalc_function::op_any;
}


static int setup_relate_func(Geometry *g1, Geometry *g2,
    Gcalc_operation_transporter *trn, Gcalc_function *func,
    const char *mask)
{
  int do_store_shapes=1;
  uint UNINIT_VAR(shape_a), UNINIT_VAR(shape_b);
  uint n_operands= 0;
  int last_shape_pos;

  last_shape_pos= func->get_next_expression_pos();
  if (func->reserve_op_buffer(1))
    return 1;
  func->add_operation(Gcalc_function::op_intersection, 0);
  for (int nc=0; nc<9; nc++)
  {
    uint cur_op;

    cur_op= Gcalc_function::op_intersection;
    switch (mask[nc])
    {
      case '*':
        continue;
      case 'T':
      case '0':
      case '1':
      case '2':
        cur_op|= Gcalc_function::v_find_t;
        break;
      case 'F':
        cur_op|= (Gcalc_function::op_not | Gcalc_function::v_find_f);
        break;
      default:
        return 1;
    };
    ++n_operands;
    if (func->reserve_op_buffer(3))
      return 1;
    func->add_operation(cur_op, 2);

    func->add_operation(op_matrix(nc/3), 1);
    if (do_store_shapes)
    {
      shape_a= func->get_next_expression_pos();
      if (g1->store_shapes(trn))
        return 1;
    }
    else
      func->repeat_expression(shape_a);
    if (func->reserve_op_buffer(1))
      return 1;
    func->add_operation(op_matrix(nc%3), 1);
    if (do_store_shapes)
    {
      shape_b= func->get_next_expression_pos();
      if (g2->store_shapes(trn))
        return 1;
      do_store_shapes= 0;
    }
    else
      func->repeat_expression(shape_b);
  }
  
  func->add_operands_to_op(last_shape_pos, n_operands);
  return 0;
}


#define GIS_ZERO 0.00000000001

class Geometry_ptr_with_buffer_and_mbr
{
public:
  Geometry *geom;
  Geometry_buffer buffer;
  MBR mbr;
  bool construct(Item *item, String *tmp_value)
  {
    const char *c_end;
    String *res= item->val_str(tmp_value);
    return
      item->null_value ||
      !(geom= Geometry::construct(&buffer, res->ptr(), res->length())) ||
      geom->get_mbr(&mbr, &c_end) || !mbr.valid();
  }
  int store_shapes(Gcalc_shape_transporter *trn) const
  { return geom->store_shapes(trn); }
};


longlong Item_func_spatial_relate::val_int()
{
  DBUG_ENTER("Item_func_spatial_relate::val_int");
  DBUG_ASSERT(fixed());
  Geometry_ptr_with_buffer_and_mbr g1, g2;
  int result= 0;

  if ((null_value= (g1.construct(args[0], &tmp_value1) ||
                    g2.construct(args[1], &tmp_value2) ||
                    func.reserve_op_buffer(1))))
    DBUG_RETURN(0);

  MBR umbr(g1.mbr, g2.mbr);
  collector.set_extent(umbr.xmin, umbr.xmax, umbr.ymin, umbr.ymax);
  g1.mbr.buffer(1e-5);
  Gcalc_operation_transporter trn(&func, &collector);

  String *matrix= args[2]->val_str(&tmp_matrix);
  if ((null_value= args[2]->null_value || matrix->length() != 9 ||
                   setup_relate_func(g1.geom, g2.geom,
                                     &trn, &func, matrix->ptr())))
    goto exit;

  collector.prepare_operation();
  scan_it.init(&collector);
  scan_it.killed= (int *) &(current_thd->killed);
  if (!func.alloc_states())
    result= func.check_function(scan_it);

exit:
  collector.reset();
  func.reset();
  scan_it.reset();
  DBUG_RETURN(result);
}


longlong Item_func_spatial_precise_rel::val_int()
{
  DBUG_ENTER("Item_func_spatial_precise_rel::val_int");
  DBUG_ASSERT(fixed());
  Geometry_ptr_with_buffer_and_mbr g1, g2;
  int result= 0;
  uint shape_a, shape_b;

  if ((null_value= (g1.construct(args[0], &tmp_value1) ||
                    g2.construct(args[1], &tmp_value2) ||
                    func.reserve_op_buffer(1))))
    DBUG_RETURN(0);

  Gcalc_operation_transporter trn(&func, &collector);

  MBR umbr(g1.mbr, g2.mbr);
  collector.set_extent(umbr.xmin, umbr.xmax, umbr.ymin, umbr.ymax);

  g1.mbr.buffer(1e-5);

  switch (spatial_rel) {
    case SP_CONTAINS_FUNC:
      if (!g1.mbr.contains(&g2.mbr))
        goto exit;
      func.add_operation(Gcalc_function::v_find_f |
                         Gcalc_function::op_not |
                         Gcalc_function::op_difference, 2);
      /* Mind the g2 goes first. */
      null_value= g2.store_shapes(&trn) || g1.store_shapes(&trn);
      break;
    case SP_WITHIN_FUNC:
      g2.mbr.buffer(2e-5);
      if (!g1.mbr.within(&g2.mbr))
        goto exit;
      func.add_operation(Gcalc_function::v_find_f |
                         Gcalc_function::op_not |
                         Gcalc_function::op_difference, 2);
      null_value= g1.store_shapes(&trn) || g2.store_shapes(&trn);
      break;
    case SP_EQUALS_FUNC:
      if (!g1.mbr.contains(&g2.mbr))
        goto exit;
      func.add_operation(Gcalc_function::v_find_f |
                         Gcalc_function::op_not |
                         Gcalc_function::op_symdifference, 2);
      null_value= g1.store_shapes(&trn) || g2.store_shapes(&trn);
      break;
    case SP_DISJOINT_FUNC:
      func.add_operation(Gcalc_function::v_find_f |
                         Gcalc_function::op_not |
                         Gcalc_function::op_intersection, 2);
      null_value= g1.store_shapes(&trn) || g2.store_shapes(&trn);
      break;
    case SP_INTERSECTS_FUNC:
      if (!g1.mbr.intersects(&g2.mbr))
        goto exit;
      func.add_operation(Gcalc_function::v_find_t |
                         Gcalc_function::op_intersection, 2);
      null_value= g1.store_shapes(&trn) || g2.store_shapes(&trn);
      break;
    case SP_OVERLAPS_FUNC:
    case SP_CROSSES_FUNC:
      func.add_operation(Gcalc_function::op_intersection, 2);
      if (func.reserve_op_buffer(3))
        break;
      func.add_operation(Gcalc_function::v_find_t |
                         Gcalc_function::op_intersection, 2);
      shape_a= func.get_next_expression_pos();
      if ((null_value= g1.store_shapes(&trn)))
        break;
      shape_b= func.get_next_expression_pos();
      if ((null_value= g2.store_shapes(&trn)))
        break;
      if (func.reserve_op_buffer(7))
        break;
      func.add_operation(Gcalc_function::op_intersection, 2);
      func.add_operation(Gcalc_function::v_find_t |
                         Gcalc_function::op_difference, 2);
      func.repeat_expression(shape_a);
      func.repeat_expression(shape_b);
      func.add_operation(Gcalc_function::v_find_t |
                         Gcalc_function::op_difference, 2);
      func.repeat_expression(shape_b);
      func.repeat_expression(shape_a);
      break;
    case SP_TOUCHES_FUNC:
      if (func.reserve_op_buffer(5))
        break;
      func.add_operation(Gcalc_function::op_intersection, 2);
      func.add_operation(Gcalc_function::v_find_f |
                         Gcalc_function::op_not |
                         Gcalc_function::op_intersection, 2);
      func.add_operation(Gcalc_function::op_internals, 1);
      shape_a= func.get_next_expression_pos();
      if ((null_value= g1.store_shapes(&trn)) ||
          func.reserve_op_buffer(1))
        break;
      func.add_operation(Gcalc_function::op_internals, 1);
      shape_b= func.get_next_expression_pos();
      if ((null_value= g2.store_shapes(&trn)) ||
          func.reserve_op_buffer(1))
        break;
      func.add_operation(Gcalc_function::v_find_t |
                         Gcalc_function::op_intersection, 2);
      func.repeat_expression(shape_a);
      func.repeat_expression(shape_b);
      break;
    default:
      DBUG_ASSERT(FALSE);
      break;
  }

  if (null_value)
    goto exit;

  collector.prepare_operation();
  scan_it.init(&collector);
  scan_it.killed= (int *) &(current_thd->killed);

  if (func.alloc_states())
    goto exit;

  result= func.check_function(scan_it);

exit:
  collector.reset();
  func.reset();
  scan_it.reset();
  DBUG_RETURN(result);
}


Item_func_spatial_operation::~Item_func_spatial_operation()
{
}


String *Item_func_spatial_operation::val_str(String *str_value)
{
  DBUG_ENTER("Item_func_spatial_operation::val_str");
  DBUG_ASSERT(fixed());
  Geometry_ptr_with_buffer_and_mbr g1, g2;
  uint32 srid= 0;
  Gcalc_operation_transporter trn(&func, &collector);

  if (func.reserve_op_buffer(1))
    DBUG_RETURN(0);
  func.add_operation(spatial_op, 2);

  if ((null_value= (g1.construct(args[0], &tmp_value1) ||
                    g2.construct(args[1], &tmp_value2))))
  {
    str_value= 0;
    goto exit;
  }

  g1.mbr.add_mbr(&g2.mbr);
  collector.set_extent(g1.mbr.xmin, g1.mbr.xmax, g1.mbr.ymin, g1.mbr.ymax);
  
  if ((null_value= g1.store_shapes(&trn) || g2.store_shapes(&trn)))
  {
    str_value= 0;
    goto exit;
  }

  collector.prepare_operation();
  if (func.alloc_states())
    goto exit;

  operation.init(&func);

  if (operation.count_all(&collector) ||
      operation.get_result(&res_receiver))
    goto exit;


  str_value->set_charset(&my_charset_bin);
  str_value->length(0);
  if (str_value->reserve(SRID_SIZE, 512))
    goto exit;
  str_value->q_append(srid);

  if (!Geometry::create_from_opresult(&g1.buffer, str_value, res_receiver))
    goto exit;

exit:
  collector.reset();
  func.reset();
  res_receiver.reset();
  DBUG_RETURN(str_value);
}


LEX_CSTRING Item_func_spatial_operation::func_name_cstring() const
{ 
  switch (spatial_op) {
    case Gcalc_function::op_intersection:
      return { STRING_WITH_LEN("st_intersection") };
    case Gcalc_function::op_difference:
      return { STRING_WITH_LEN("st_difference") };
    case Gcalc_function::op_union:
      return { STRING_WITH_LEN("st_union") };
    case Gcalc_function::op_symdifference:
      return { STRING_WITH_LEN("st_symdifference") };
    default:
      DBUG_ASSERT(0);  // Should never happen
      return { STRING_WITH_LEN("sp_unknown") };
  }
}


static const int SINUSES_CALCULATED= 32;
static double n_sinus[SINUSES_CALCULATED+1]=
{
  0,
  0.04906767432741802,
  0.0980171403295606,
  0.1467304744553618,
  0.1950903220161283,
  0.2429801799032639,
  0.2902846772544623,
  0.3368898533922201,
  0.3826834323650898,
  0.4275550934302821,
  0.4713967368259976,
  0.5141027441932217,
  0.5555702330196022,
  0.5956993044924334,
  0.6343932841636455,
  0.6715589548470183,
  0.7071067811865475,
  0.7409511253549591,
  0.773010453362737,
  0.8032075314806448,
  0.8314696123025452,
  0.8577286100002721,
  0.8819212643483549,
  0.9039892931234433,
  0.9238795325112867,
  0.9415440651830208,
  0.9569403357322089,
  0.970031253194544,
  0.9807852804032304,
  0.989176509964781,
  0.9951847266721968,
  0.9987954562051724,
  1
};


static void get_n_sincos(int n, double *sinus, double *cosinus)
{
  DBUG_ASSERT(n > 0 && n < SINUSES_CALCULATED*2+1);
  if (n < (SINUSES_CALCULATED + 1))
  {
    *sinus= n_sinus[n];
    *cosinus= n_sinus[SINUSES_CALCULATED - n];
  }
  else
  {
    n-= SINUSES_CALCULATED;
    *sinus= n_sinus[SINUSES_CALCULATED - n];
    *cosinus= -n_sinus[n];
  }
}


static int fill_half_circle(Gcalc_shape_transporter *trn, double x, double y,
                            double ax, double ay)
{
  double n_sin, n_cos;
  double x_n, y_n;
  for (int n = 1; n < (SINUSES_CALCULATED * 2 - 1); n++)
  {
    get_n_sincos(n, &n_sin, &n_cos);
    x_n= ax * n_cos - ay * n_sin;
    y_n= ax * n_sin + ay * n_cos;
    if (trn->add_point(x_n + x, y_n + y))
      return 1;
  }
  return 0;
}


static int fill_gap(Gcalc_shape_transporter *trn,
                    double x, double y,
                    double ax, double ay, double bx, double by, double d,
                    bool *empty_gap)
{
  double ab= ax * bx + ay * by;
  double cosab= ab / (d * d) + GIS_ZERO;
  double n_sin, n_cos;
  double x_n, y_n;
  int n=1;

  *empty_gap= true;
  for (;;)
  {
    get_n_sincos(n++, &n_sin, &n_cos);
    if (n_cos <= cosab)
      break;
    *empty_gap= false;
    x_n= ax * n_cos - ay * n_sin;
    y_n= ax * n_sin + ay * n_cos;
    if (trn->add_point(x_n + x, y_n + y))
      return 1;
  }
  return 0;
}


/*
  Calculates the vector (p2,p1) and
  negatively orthogonal to it with the length of d.
  The result is (ex,ey) - the vector, (px,py) - the orthogonal.
*/

static void calculate_perpendicular(
    double x1, double y1, double x2, double y2, double d,
    double *ex, double *ey,
    double *px, double *py)
{
  double q;
  *ex= x1 - x2;
  *ey= y1 - y2;
  q= d / sqrt((*ex) * (*ex) + (*ey) * (*ey));
  *px= (*ey) * q;
  *py= -(*ex) * q;
}


int Item_func_buffer::Transporter::single_point(double x, double y)
{
  if (buffer_op == Gcalc_function::op_difference)
  {
    if (m_fn->reserve_op_buffer(1))
      return 1;
    m_fn->add_operation(Gcalc_function::op_false, 0);
    return 0;
  }
  
  m_nshapes= 0;
  return add_point_buffer(x, y);
}


int Item_func_buffer::Transporter::add_edge_buffer(
  double x3, double y3, bool round_p1, bool round_p2)
{
  Gcalc_operation_transporter trn(m_fn, m_heap);
  double e1_x, e1_y, e2_x, e2_y, p1_x, p1_y, p2_x, p2_y;
  double e1e2;
  double sin1, cos1;
  double x_n, y_n;
  bool empty_gap1, empty_gap2;

  ++m_nshapes;
  if (trn.start_simple_poly())
    return 1;

  calculate_perpendicular(x1, y1, x2, y2, m_d, &e1_x, &e1_y, &p1_x, &p1_y);
  calculate_perpendicular(x3, y3, x2, y2, m_d, &e2_x, &e2_y, &p2_x, &p2_y);

  e1e2= e1_x * e2_y - e2_x * e1_y;
  sin1= n_sinus[1];
  cos1= n_sinus[31];
  if (e1e2 < 0)
  {
    empty_gap2= false;
    x_n= x2 + p2_x * cos1 - p2_y * sin1;
    y_n= y2 + p2_y * cos1 + p2_x * sin1;
    if (fill_gap(&trn, x2, y2, -p1_x,-p1_y, p2_x,p2_y, m_d, &empty_gap1) ||
        trn.add_point(x2 + p2_x, y2 + p2_y) ||
        trn.add_point(x_n, y_n))
      return 1;
  }
  else
  {
    x_n= x2 - p2_x * cos1 - p2_y * sin1;
    y_n= y2 - p2_y * cos1 + p2_x * sin1;
    if (trn.add_point(x_n, y_n) ||
        trn.add_point(x2 - p2_x, y2 - p2_y) ||
        fill_gap(&trn, x2, y2, -p2_x, -p2_y, p1_x, p1_y, m_d, &empty_gap2))
      return 1;
    empty_gap1= false;
  }
  if ((!empty_gap2 && trn.add_point(x2 + p1_x, y2 + p1_y)) ||
      trn.add_point(x1 + p1_x, y1 + p1_y))
    return 1;

  if (round_p1 && fill_half_circle(&trn, x1, y1, p1_x, p1_y))
    return 1;

  if (trn.add_point(x1 - p1_x, y1 - p1_y) ||
      (!empty_gap1 && trn.add_point(x2 - p1_x, y2 - p1_y)))
    return 1;
  return trn.complete_simple_poly();
}


int Item_func_buffer::Transporter::add_last_edge_buffer()
{
  Gcalc_operation_transporter trn(m_fn, m_heap);
  double e1_x, e1_y, p1_x, p1_y;

  ++m_nshapes;
  if (trn.start_simple_poly())
    return 1;

  calculate_perpendicular(x1, y1, x2, y2, m_d, &e1_x, &e1_y, &p1_x, &p1_y);

  if (trn.add_point(x1 + p1_x, y1 + p1_y) ||
      trn.add_point(x1 - p1_x, y1 - p1_y) ||
      trn.add_point(x2 - p1_x, y2 - p1_y) ||
      fill_half_circle(&trn, x2, y2, -p1_x, -p1_y) ||
      trn.add_point(x2 + p1_x, y2 + p1_y))
    return 1;
  return trn.complete_simple_poly();
}


int Item_func_buffer::Transporter::add_point_buffer(double x, double y)
{
  Gcalc_operation_transporter trn(m_fn, m_heap);

  m_nshapes++;
  if (trn.start_simple_poly())
    return 1;
  if (trn.add_point(x - m_d, y) ||
      fill_half_circle(&trn, x, y, -m_d, 0.0) ||
      trn.add_point(x + m_d, y) ||
      fill_half_circle(&trn, x, y, m_d, 0.0))
    return 1;
  return trn.complete_simple_poly();
}


int Item_func_buffer::Transporter::start_line()
{
  if (buffer_op == Gcalc_function::op_difference)
  {
    if (m_fn->reserve_op_buffer(1))
      return 1;
    m_fn->add_operation(Gcalc_function::op_false, 0);
    skip_line= TRUE;
    return 0;
  }
  
  m_nshapes= 0;

  if (m_fn->reserve_op_buffer(2))
    return 1;
  last_shape_pos= m_fn->get_next_expression_pos();
  m_fn->add_operation(buffer_op, 0);
  m_npoints= 0;
  int_start_line();
  return 0;
}


int Item_func_buffer::Transporter::start_poly()
{
  m_nshapes= 1;

  if (m_fn->reserve_op_buffer(2))
    return 1;
  last_shape_pos= m_fn->get_next_expression_pos();
  m_fn->add_operation(buffer_op, 0);
  return Gcalc_operation_transporter::start_poly();
}


int Item_func_buffer::Transporter::complete_poly()
{
  if (Gcalc_operation_transporter::complete_poly())
    return 1;
  m_fn->add_operands_to_op(last_shape_pos, m_nshapes);
  return 0;
}


int Item_func_buffer::Transporter::start_ring()
{
  m_npoints= 0;
  return Gcalc_operation_transporter::start_ring();
}


int Item_func_buffer::Transporter::start_collection(int n_objects)
{
  if (m_fn->reserve_op_buffer(1))
    return 1;
  m_fn->add_operation(Gcalc_function::op_union, n_objects);
  return 0;
}


int Item_func_buffer::Transporter::add_point(double x, double y)
{
  if (skip_line)
    return 0;

  if (m_npoints && x == x2 && y == y2)
    return 0;

  ++m_npoints;

  if (m_npoints == 1)
  {
    x00= x;
    y00= y;
  }
  else if (m_npoints == 2)
  {
    x01= x;
    y01= y;
  }
  else if (add_edge_buffer(x, y, (m_npoints == 3) && line_started(), false))
    return 1;

  x1= x2;
  y1= y2;
  x2= x;
  y2= y;

  return line_started() ? 0 : Gcalc_operation_transporter::add_point(x, y);
}


int Item_func_buffer::Transporter::complete()
{
  if (m_npoints)
  {
    if (m_npoints == 1)
    {
      if (add_point_buffer(x2, y2))
        return 1;
    }
    else if (m_npoints == 2)
    {
      if (add_edge_buffer(x1, y1, true, true))
        return 1;
    }
    else if (line_started())
    {
      if (add_last_edge_buffer())
        return 1;
    }
    else
    {
      if (x2 != x00 || y2 != y00)
      {
        if (add_edge_buffer(x00, y00, false, false))
          return 1;
        x1= x2;
        y1= y2;
        x2= x00;
        y2= y00;
      }
      if (add_edge_buffer(x01, y01, false, false))
        return 1;
    }
  }

  return 0;
}


int Item_func_buffer::Transporter::complete_line()
{
  if (!skip_line)
  {
    if (complete())
      return 1;
    int_complete_line();
    m_fn->add_operands_to_op(last_shape_pos, m_nshapes);
  }
  skip_line= FALSE;
  return 0;
}


int Item_func_buffer::Transporter::complete_ring()
{
  return complete() ||
         Gcalc_operation_transporter::complete_ring();
}


String *Item_func_buffer::val_str(String *str_value)
{
  DBUG_ENTER("Item_func_buffer::val_str");
  DBUG_ASSERT(fixed());
  String *obj= args[0]->val_str(str_value);
  double dist= args[1]->val_real();
  Geometry_buffer buffer;
  Geometry *g;
  uint32 srid= 0;
  String *str_result= NULL;
  Transporter trn(&func, &collector, dist);
  MBR mbr;
  const char *c_end;

  null_value= 1;
  if (args[0]->null_value || args[1]->null_value ||
      !(g= Geometry::construct(&buffer, obj->ptr(), obj->length())) ||
      g->get_mbr(&mbr, &c_end))
    goto mem_error;

  if (dist > 0.0)
    mbr.buffer(dist);
  else
  {
    /* This happens when dist is too far negative. */
    if (mbr.xmax + dist < mbr.xmin || mbr.ymax + dist < mbr.ymin)
      goto return_empty_result;
  }

  collector.set_extent(mbr.xmin, mbr.xmax, mbr.ymin, mbr.ymax);
  /*
    If the distance given is 0, the Buffer function is in fact NOOP,
    so it's natural just to return the argument1.
    Besides, internal calculations here can't handle zero distance anyway.
  */
  if (fabs(dist) < GIS_ZERO)
  {
    null_value= 0;
    str_result= obj;
    goto mem_error;
  }

  if (g->store_shapes(&trn))
    goto mem_error;

  collector.prepare_operation();
  if (func.alloc_states())
    goto mem_error;
  operation.init(&func);
  operation.killed= (int *) &(current_thd->killed);

  if (operation.count_all(&collector) ||
      operation.get_result(&res_receiver))
    goto mem_error;


return_empty_result:
  str_value->set_charset(&my_charset_bin);
  str_value->length(0);
  if (str_value->reserve(SRID_SIZE, 512))
    goto mem_error;
  str_value->q_append(srid);

  if (!Geometry::create_from_opresult(&buffer, str_value, res_receiver))
    goto mem_error;

  null_value= 0;
  str_result= str_value;
mem_error:
  collector.reset();
  func.reset();
  res_receiver.reset();
  DBUG_RETURN(str_result);
}


longlong Item_func_isempty::val_int()
{
  DBUG_ASSERT(fixed());
  String tmp;
  String *swkb= args[0]->val_str(&tmp);
  Geometry_buffer buffer;
  
  null_value= args[0]->null_value ||
              !(Geometry::construct(&buffer, swkb->ptr(), swkb->length()));
  return null_value ? 1 : 0;
}


longlong Item_func_issimple::val_int()
{
  String *swkb= args[0]->val_str(&tmp);
  Geometry_buffer buffer;
  Gcalc_operation_transporter trn(&func, &collector);
  Geometry *g;
  int result= 1;
  MBR mbr;
  const char *c_end;

  DBUG_ENTER("Item_func_issimple::val_int");
  DBUG_ASSERT(fixed());
  
  null_value= 0;
  if ((args[0]->null_value ||
       !(g= Geometry::construct(&buffer, swkb->ptr(), swkb->length())) ||
       g->get_mbr(&mbr, &c_end)))
  {
    /* We got NULL as an argument. Have to return -1 */
    DBUG_RETURN(-1);
  }

  collector.set_extent(mbr.xmin, mbr.xmax, mbr.ymin, mbr.ymax);

  if (g->get_class_info()->m_type_id == Geometry::wkb_point)
    DBUG_RETURN(1);

  if (g->store_shapes(&trn))
    goto mem_error;

  collector.prepare_operation();
  scan_it.init(&collector);

  while (scan_it.more_points())
  {
    const Gcalc_scan_iterator::event_point *ev, *next_ev;

    if (scan_it.step())
      goto mem_error;

    ev= scan_it.get_events();
    if (ev->simple_event())
      continue;

    next_ev= ev->get_next();
    if ((ev->event & (scev_thread | scev_single_point)) && !next_ev)
      continue;

    if ((ev->event == scev_two_threads) && !next_ev->get_next())
      continue;

    /* If the first and last points of a curve coincide - that is     */
    /* an exception to the rule and the line is considered as simple. */
    if ((next_ev && !next_ev->get_next()) &&
        (ev->event & (scev_thread | scev_end)) &&
        (next_ev->event & (scev_thread | scev_end)))
      continue;

    result= 0;
    break;
  }

  collector.reset();
  func.reset();
  scan_it.reset();
  DBUG_RETURN(result);
mem_error:
  null_value= 1;
  DBUG_RETURN(0);
}


longlong Item_func_isclosed::val_int()
{
  DBUG_ASSERT(fixed());
  String tmp;
  String *swkb= args[0]->val_str(&tmp);
  Geometry_buffer buffer;
  Geometry *geom;
  int isclosed= 0;				// In case of error

  null_value= 0;
  if (!swkb || 
      args[0]->null_value ||
      !(geom= Geometry::construct(&buffer, swkb->ptr(), swkb->length())) ||
      geom->is_closed(&isclosed))
  {
    /* IsClosed(NULL) should return -1 */
    return -1;
  }

  return (longlong) isclosed;
}


longlong Item_func_isring::val_int()
{
  /* It's actually a combination of two functions - IsClosed and IsSimple */
  DBUG_ASSERT(fixed());
  String tmp;
  String *swkb= args[0]->val_str(&tmp);
  Geometry_buffer buffer;
  Geometry *geom;
  int isclosed= 0;				// In case of error

  null_value= 0;
  if (!swkb || 
      args[0]->null_value ||
      !(geom= Geometry::construct(&buffer, swkb->ptr(), swkb->length())) ||
      geom->is_closed(&isclosed))
  {
    /* IsRing(NULL) should return -1 */
    return -1;
  }

  if (!isclosed)
    return 0;

  return Item_func_issimple::val_int();
}


/*
  Numerical functions
*/


longlong Item_func_dimension::val_int()
{
  DBUG_ASSERT(fixed());
  uint32 dim= 0;				// In case of error
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;
  const char *dummy;

  null_value= (!swkb || 
	       args[0]->null_value ||
	       !(geom= Geometry::construct(&buffer, swkb->ptr(), swkb->length())) ||
	       geom->dimension(&dim, &dummy));
  return (longlong) dim;
}


longlong Item_func_numinteriorring::val_int()
{
  DBUG_ASSERT(fixed());
  uint32 num= 0;				// In case of error
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;

  null_value= (!swkb || 
	       !(geom= Geometry::construct(&buffer,
                                           swkb->ptr(), swkb->length())) ||
	       geom->num_interior_ring(&num));
  return (longlong) num;
}


longlong Item_func_numgeometries::val_int()
{
  DBUG_ASSERT(fixed());
  uint32 num= 0;				// In case of errors
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;

  null_value= (!swkb ||
	       !(geom= Geometry::construct(&buffer,
                                           swkb->ptr(), swkb->length())) ||
	       geom->num_geometries(&num));
  return (longlong) num;
}


longlong Item_func_numpoints::val_int()
{
  DBUG_ASSERT(fixed());
  uint32 num= 0;				// In case of errors
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;

  null_value= (!swkb ||
	       args[0]->null_value ||
	       !(geom= Geometry::construct(&buffer,
                                           swkb->ptr(), swkb->length())) ||
	       geom->num_points(&num));
  return (longlong) num;
}


double Item_func_x::val_real()
{
  DBUG_ASSERT(fixed());
  double res= 0.0;				// In case of errors
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;

  null_value= (!swkb ||
	       !(geom= Geometry::construct(&buffer,
                                           swkb->ptr(), swkb->length())) ||
	       geom->get_x(&res));
  return res;
}


double Item_func_y::val_real()
{
  DBUG_ASSERT(fixed());
  double res= 0;				// In case of errors
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;

  null_value= (!swkb ||
	       !(geom= Geometry::construct(&buffer,
                                           swkb->ptr(), swkb->length())) ||
	       geom->get_y(&res));
  return res;
}


double Item_func_area::val_real()
{
  DBUG_ASSERT(fixed());
  double res= 0;				// In case of errors
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;
  const char *dummy;

  null_value= (!swkb ||
	       !(geom= Geometry::construct(&buffer,
                                           swkb->ptr(), swkb->length())) ||
	       geom->area(&res, &dummy));
  return res;
}

double Item_func_glength::val_real()
{
  DBUG_ASSERT(fixed());
  double res= 0;				// In case of errors
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;
  const char *end;

  null_value= (!swkb || 
	       !(geom= Geometry::construct(&buffer,
                                           swkb->ptr(),
                                           swkb->length())) ||
	       geom->geom_length(&res, &end));
  return res;
}

longlong Item_func_srid::val_int()
{
  DBUG_ASSERT(fixed());
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  
  null_value= (!swkb || 
	       !Geometry::construct(&buffer,
                                    swkb->ptr(), swkb->length()));
  if (null_value)
    return 0;

  return (longlong) (uint4korr(swkb->ptr()));
}


double Item_func_distance::val_real()
{
  bool cur_point_edge;
  const Gcalc_scan_iterator::point *evpos;
  const Gcalc_heap::Info *cur_point, *dist_point;
  const Gcalc_scan_iterator::event_point *ev;
  double t, distance, cur_distance;
  double x1, x2, y1, y2;
  double ex, ey, vx, vy, e_sqrlen;
  uint obj2_si;
  Gcalc_operation_transporter trn(&func, &collector);

  DBUG_ENTER("Item_func_distance::val_real");
  DBUG_ASSERT(fixed());
  String *res1= args[0]->val_str(&tmp_value1);
  String *res2= args[1]->val_str(&tmp_value2);
  Geometry_buffer buffer1, buffer2;
  Geometry *g1, *g2;
  MBR mbr1, mbr2;
  const char *c_end;

  if (args[0]->null_value || args[1]->null_value)
    goto mem_error;
  g1= Geometry::construct(&buffer1, res1->ptr(), res1->length());
  if (!g1)
    goto mem_error;
  g2= Geometry::construct(&buffer2, res2->ptr(), res2->length());
  if (!g2)
    goto mem_error;
  if (g1->get_mbr(&mbr1, &c_end) || g2->get_mbr(&mbr2, &c_end))
    goto mem_error;

  mbr1.add_mbr(&mbr2);
  collector.set_extent(mbr1.xmin, mbr1.xmax, mbr1.ymin, mbr1.ymax);

  if ((g1->get_class_info()->m_type_id == Geometry::wkb_point) &&
      (g2->get_class_info()->m_type_id == Geometry::wkb_point))
  {
    if (((Gis_point *) g1)->get_xy(&x1, &y1) ||
        ((Gis_point *) g2)->get_xy(&x2, &y2))
      goto mem_error;
    ex= x2 - x1;
    ey= y2 - y1;
    DBUG_RETURN(sqrt(ex * ex + ey * ey));
  }

  if (func.reserve_op_buffer(1))
    goto mem_error;
  func.add_operation(Gcalc_function::op_intersection, 2);

  if (g1->store_shapes(&trn))
    goto mem_error;
  obj2_si= func.get_nshapes();
  if (g2->store_shapes(&trn) || func.alloc_states())
    goto mem_error;

  if (obj2_si == 0 || func.get_nshapes() == obj2_si)
  {
    distance= 0.0;
    null_value= 1;
    goto exit;
  }


  collector.prepare_operation();
  scan_it.init(&collector);

  distance= DBL_MAX;
  while (scan_it.more_points())
  {
    if (scan_it.step())
      goto mem_error;
    evpos= scan_it.get_event_position();
    ev= scan_it.get_events();

    if (ev->simple_event())
    {
      cur_point= ev->pi;
      goto count_distance;
    }
    /*
       handling intersection we only need to check if it's the intersecion
       of objects 1 and 2. In this case distance is 0
    */
    cur_point= NULL;

    /*
       having these events we need to check for possible intersection
       of objects
       scev_thread | scev_two_threads | scev_single_point
    */
    func.clear_i_states();
    for (Gcalc_point_iterator pit(&scan_it); pit.point() != evpos; ++pit)
    {
      gcalc_shape_info si= pit.point()->get_shape();
      if ((func.get_shape_kind(si) == Gcalc_function::shape_polygon))
        func.invert_i_state(si);
    }

    func.clear_b_states();
    for (; ev; ev= ev->get_next())
    {
      if (ev->event != scev_intersection)
        cur_point= ev->pi;
      func.set_b_state(ev->get_shape());
      if (func.count())
      {
        /* Point of one object is inside the other - intersection found */
        distance= 0;
        goto exit;
      }
    }

    if (!cur_point)
      continue;

count_distance:
    if (cur_point->node.shape.shape >= obj2_si)
      continue;
    cur_point_edge= !cur_point->is_bottom();

    for (dist_point= collector.get_first(); dist_point; dist_point= dist_point->get_next())
    {
      /* We only check vertices of object 2 */
      if (dist_point->type != Gcalc_heap::nt_shape_node ||
          dist_point->node.shape.shape < obj2_si)
        continue;

      /* if we have an edge to check */
      if (dist_point->node.shape.left)
      {
        t= count_edge_t(dist_point, dist_point->node.shape.left, cur_point,
                        ex, ey, vx, vy, e_sqrlen);
        if ((t>0.0) && (t<1.0))
        {
          cur_distance= distance_to_line(ex, ey, vx, vy, e_sqrlen);
          if (distance > cur_distance)
            distance= cur_distance;
        }
      }
      if (cur_point_edge)
      {
        t= count_edge_t(cur_point, cur_point->node.shape.left, dist_point,
                        ex, ey, vx, vy, e_sqrlen);
        if ((t>0.0) && (t<1.0))
        {
          cur_distance= distance_to_line(ex, ey, vx, vy, e_sqrlen);
          if (distance > cur_distance)
            distance= cur_distance;
        }
      }
      cur_distance= distance_points(cur_point, dist_point);
      if (distance > cur_distance)
        distance= cur_distance;
    }
  }
exit:
  collector.reset();
  func.reset();
  scan_it.reset();
  DBUG_RETURN(distance);
mem_error:
  null_value= 1;
  DBUG_RETURN(0);
}


double Item_func_sphere_distance::val_real()
{
  /* To test null_value of item, first get well-known bytes as a backups */
  String bak1, bak2;
  String *arg1= args[0]->val_str(&bak1);
  String *arg2= args[1]->val_str(&bak2);
  double distance= 0.0;
  double sphere_radius= 6370986.0; // Default radius equals Earth radius
  
  null_value= (args[0]->null_value || args[1]->null_value);
  if (null_value)
  {
    return 0;
  }

  if (arg_count == 3)
  {
    sphere_radius= args[2]->val_real();
    // Radius cannot be Null
    if (args[2]->null_value)
    {
      null_value= true;
      return 0;
    }
    if (sphere_radius <= 0)
    {
      my_error(ER_INTERNAL_ERROR, MYF(0), "Radius must be greater than zero.");
      return 1;
    }
  }
  Geometry_buffer buffer1, buffer2;
  Geometry *g1, *g2;
  if (!(g1= Geometry::construct(&buffer1, arg1->ptr(), arg1->length())) ||
      !(g2= Geometry::construct(&buffer2, arg2->ptr(), arg2->length())))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), "ST_Distance_Sphere");
    goto handle_errors;
  }
// Method allowed for points and multipoints
  if (!(g1->get_class_info()->m_type_id == Geometry::wkb_point ||
        g1->get_class_info()->m_type_id == Geometry::wkb_multipoint) ||
      !(g2->get_class_info()->m_type_id == Geometry::wkb_point ||
        g2->get_class_info()->m_type_id == Geometry::wkb_multipoint))
  {
    // Generate error message in case different geometry is used? 
    my_error(ER_INTERNAL_ERROR, MYF(0), func_name());
    return 0;
  }
  distance= spherical_distance_points(g1, g2, sphere_radius);
  if (distance < 0)
  {
    my_error(ER_INTERNAL_ERROR, MYF(0), "Returned distance cannot be negative.");
    return 1;
  }
  return distance;

  handle_errors:
    return 0;
}


double Item_func_sphere_distance::spherical_distance_points(Geometry *g1,
                                                            Geometry *g2,
                                                            const double r)
{
  double res= 0.0;
   // Length for the single point (25 Bytes)
  uint32 len= SRID_SIZE + POINT_DATA_SIZE + WKB_HEADER_SIZE;
  int error= 0;

  switch (g2->get_class_info()->m_type_id)
  {
    case Geometry::wkb_point:
    // Optimization for point-point case
      if (g1->get_class_info()->m_type_id == Geometry::wkb_point)
      {
        res= static_cast<Gis_point *>(g2)->calculate_haversine(g1, r, &error);
      }
      else
      {
        // Optimization for single point in Multipoint
        if (g1->get_data_size() == len)
        {
          res= static_cast<Gis_point *>(g2)->calculate_haversine(g1, r, &error);
        }
        else
        {
          // There are multipoints in g1
          // g1 is MultiPoint and calculate MP.sphericaldistance from g2 Point
          if (g1->get_data_size() != GET_SIZE_ERROR)
            static_cast<Gis_point *>(g2)->spherical_distance_multipoints(
                                        (Gis_multi_point *)g1, r, &res, &error);
        }
      }
      break;

    case Geometry::wkb_multipoint:
      // Optimization for point-point case
      if (g1->get_class_info()->m_type_id == Geometry::wkb_point)
      {
         // Optimization for single point in Multipoint g2
        if (g2->get_data_size() == len)
        {
          res= static_cast<Gis_point *>(g1)->calculate_haversine(g2, r, &error);
        }
        else
        {
          if (g2->get_data_size() != GET_SIZE_ERROR)
          // g1 is a point (casted to multi_point) and g2 multipoint
            static_cast<Gis_point *>(g1)->spherical_distance_multipoints(
                                        (Gis_multi_point *)g2, r, &res, &error);
        }
      }
      else
      {
        // Multipoints in g1 and g2 - no optimization
        static_cast<Gis_multi_point *>(g1)->spherical_distance_multipoints(
                                        (Gis_multi_point *)g2, r, &res, &error);
      }
      break;

    default:
      DBUG_ASSERT(0);
      break;
  }

  if (res < 0)
    goto handle_error;

  handle_error:
    if (error > 0)
      my_error(ER_STD_OUT_OF_RANGE_ERROR, MYF(0),
               "Longitude should be [-180,180]", "ST_Distance_Sphere");
    else if(error < 0)
      my_error(ER_STD_OUT_OF_RANGE_ERROR, MYF(0),
               "Latitude should be [-90,90]", "ST_Distance_Sphere");
  return res;
}


String *Item_func_pointonsurface::val_str(String *str)
{
  Gcalc_operation_transporter trn(&func, &collector);
  String *res= args[0]->val_str(&tmp_value);
  Geometry_buffer buffer;
  Geometry *g;
  MBR mbr;
  const char *c_end;
  double UNINIT_VAR(px), UNINIT_VAR(py), x0, UNINIT_VAR(y0);
  String *result= 0;
  const Gcalc_scan_iterator::point *pprev= NULL;
  uint32 srid;
  DBUG_ENTER("Item_func_pointonsurface::val_str");
  DBUG_ASSERT(fixed());

  null_value= 1;
  if ((args[0]->null_value ||
       !(g= Geometry::construct(&buffer, res->ptr(), res->length())) ||
       g->get_mbr(&mbr, &c_end)))
    goto mem_error;

  collector.set_extent(mbr.xmin, mbr.xmax, mbr.ymin, mbr.ymax);

  if (g->store_shapes(&trn))
    goto mem_error;

  collector.prepare_operation();
  scan_it.init(&collector);

  while (scan_it.more_points())
  {
    if (scan_it.step())
      goto mem_error;

    if (scan_it.get_h() > GIS_ZERO)
    {
      y0= scan_it.get_y();
      break;
    }
  }

  if (!scan_it.more_points())
  {
    goto exit;
  }

  if (scan_it.step())
    goto mem_error;

  for (Gcalc_point_iterator pit(&scan_it); pit.point(); ++pit)
  {
    if (pprev == NULL)
    {
      pprev= pit.point();
      continue;
    }
    x0= scan_it.get_sp_x(pprev);
    px= scan_it.get_sp_x(pit.point());
    if (px - x0 > GIS_ZERO)
    {
      if (scan_it.get_h() > GIS_ZERO)
      {
        px= (px + x0) / 2.0;
        py= scan_it.get_y();
      }
      else
      {
        px= (px + x0) / 2.0;
        py= (y0 + scan_it.get_y()) / 2.0;
      }
      null_value= 0;
      break;
    }
    pprev= NULL;
  }

  if (null_value)
    goto exit;

  str->set_charset(&my_charset_bin);
  str->length(0);
  if (str->reserve(SRID_SIZE, 512))
    goto mem_error;

  srid= uint4korr(res->ptr());
  str->q_append(srid);

  if (Geometry::create_point(str, px, py))
    goto mem_error;

  result= str;

exit:
  collector.reset();
  func.reset();
  scan_it.reset();
  DBUG_RETURN(result);

mem_error:
  collector.reset();
  func.reset();
  scan_it.reset();
  null_value= 1;
  DBUG_RETURN(0);
}


#ifndef DBUG_OFF
longlong Item_func_gis_debug::val_int()
{
  /* For now this is just a stub. TODO: implement the internal GIS debuggign */
  return 0;
}
#endif


/**********************************************************************/


class Create_func_area : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_area(thd, arg1);
  }

  static Create_func_area s_singleton;

protected:
  Create_func_area() {}
  virtual ~Create_func_area() {}
};


class Create_func_as_wkb : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_as_wkb(thd, arg1);
  }

  static Create_func_as_wkb s_singleton;

protected:
  Create_func_as_wkb() {}
  virtual ~Create_func_as_wkb() {}
};


class Create_func_as_wkt : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_as_wkt(thd, arg1);
  }

  static Create_func_as_wkt s_singleton;

protected:
  Create_func_as_wkt() {}
  virtual ~Create_func_as_wkt() {}
};



class Create_func_centroid : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_centroid(thd, arg1);
  }

  static Create_func_centroid s_singleton;

protected:
  Create_func_centroid() {}
  virtual ~Create_func_centroid() {}
};


class Create_func_convexhull : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_convexhull(thd, arg1);
  }

  static Create_func_convexhull s_singleton;

protected:
  Create_func_convexhull() {}
  virtual ~Create_func_convexhull() {}
};


class Create_func_pointonsurface : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_pointonsurface(thd, arg1);
  }

  static Create_func_pointonsurface s_singleton;

protected:
  Create_func_pointonsurface() {}
  virtual ~Create_func_pointonsurface() {}
};


class Create_func_mbr_contains : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_mbr_rel(thd, arg1, arg2,
      Item_func::SP_CONTAINS_FUNC);
  }

  static Create_func_mbr_contains s_singleton;

protected:
  Create_func_mbr_contains() {}
  virtual ~Create_func_mbr_contains() {}
};


class Create_func_contains : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_precise_rel(thd, arg1, arg2,
                                                 Item_func::SP_CONTAINS_FUNC);
  }
  static Create_func_contains s_singleton;

protected:
  Create_func_contains() {}
  virtual ~Create_func_contains() {}
};


class Create_func_crosses : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_precise_rel(thd, arg1, arg2,
                                                   Item_func::SP_CROSSES_FUNC);
  }
  static Create_func_crosses s_singleton;

protected:
  Create_func_crosses() {}
  virtual ~Create_func_crosses() {}
};


class Create_func_dimension : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_dimension(thd, arg1);
  }

  static Create_func_dimension s_singleton;

protected:
  Create_func_dimension() {}
  virtual ~Create_func_dimension() {}
};


class Create_func_mbr_disjoint : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_mbr_rel(thd, arg1, arg2,
                                                  Item_func::SP_DISJOINT_FUNC);
  }

  static Create_func_mbr_disjoint s_singleton;

protected:
  Create_func_mbr_disjoint() {}
  virtual ~Create_func_mbr_disjoint() {}
};


class Create_func_disjoint : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_precise_rel(thd, arg1, arg2,
                                                  Item_func::SP_DISJOINT_FUNC);
  }
  static Create_func_disjoint s_singleton;

protected:
  Create_func_disjoint() {}
  virtual ~Create_func_disjoint() {}
};


class Create_func_distance : public Create_func_arg2
{
public:
  Item* create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_distance(thd, arg1, arg2);
  }

  static Create_func_distance s_singleton;

protected:
  Create_func_distance() {}
  virtual ~Create_func_distance() {}
};


class Create_func_distance_sphere: public Create_native_func
{
public:
  Item *create_native(THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
    override;
  static Create_func_distance_sphere s_singleton;

protected:
  Create_func_distance_sphere() {}
  virtual ~Create_func_distance_sphere() {}
};


Item*
Create_func_distance_sphere::create_native(THD *thd, const LEX_CSTRING *name,
                                           List<Item> *item_list)
{
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  if (arg_count < 2)
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return NULL;
  }
  return new (thd->mem_root) Item_func_sphere_distance(thd, *item_list);
}


class Create_func_endpoint : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_spatial_decomp(thd, arg1,
                                                        Item_func::SP_ENDPOINT);
  }

  static Create_func_endpoint s_singleton;

protected:
  Create_func_endpoint() {}
  virtual ~Create_func_endpoint() {}
};


class Create_func_envelope : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_envelope(thd, arg1);
  }

  static Create_func_envelope s_singleton;

protected:
  Create_func_envelope() {}
  virtual ~Create_func_envelope() {}
};

class Create_func_boundary : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_boundary(thd, arg1);
  }

  static Create_func_boundary s_singleton;

protected:
  Create_func_boundary() {}
  virtual ~Create_func_boundary() {}
};


class Create_func_mbr_equals : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_mbr_rel(thd, arg1, arg2,
                                                     Item_func::SP_EQUALS_FUNC);
  }

  static Create_func_mbr_equals s_singleton;

protected:
  Create_func_mbr_equals() {}
  virtual ~Create_func_mbr_equals() {}
};


class Create_func_equals : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
      return new (thd->mem_root) Item_func_spatial_precise_rel(thd, arg1, arg2,
                                                    Item_func::SP_EQUALS_FUNC);
  }

  static Create_func_equals s_singleton;

protected:
  Create_func_equals() {}
  virtual ~Create_func_equals() {}
};


class Create_func_exteriorring : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_spatial_decomp(thd, arg1,
                                                      Item_func::SP_EXTERIORRING);
  }

  static Create_func_exteriorring s_singleton;

protected:
  Create_func_exteriorring() {}
  virtual ~Create_func_exteriorring() {}
};



class Create_func_geometry_from_text : public Create_native_func
{
public:
  Item *create_native(THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
    override;

  static Create_func_geometry_from_text s_singleton;

protected:
  Create_func_geometry_from_text() {}
  virtual ~Create_func_geometry_from_text() {}
};


Item*
Create_func_geometry_from_text::create_native(THD *thd,
                                              const LEX_CSTRING *name,
                                              List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 1:
  {
    Item *param_1= item_list->pop();
    func= new (thd->mem_root) Item_func_geometry_from_text(thd, param_1);
    thd->lex->uncacheable(UNCACHEABLE_RAND);
    break;
  }
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_geometry_from_text(thd, param_1, param_2);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


class Create_func_geometry_from_wkb : public Create_native_func
{
public:
  Item *create_native(THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
    override;

  static Create_func_geometry_from_wkb s_singleton;

protected:
  Create_func_geometry_from_wkb() {}
  virtual ~Create_func_geometry_from_wkb() {}
};


Item*
Create_func_geometry_from_wkb::create_native(THD *thd, const LEX_CSTRING *name,
                                             List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 1:
  {
    Item *param_1= item_list->pop();
    func= new (thd->mem_root) Item_func_geometry_from_wkb(thd, param_1);
    thd->lex->uncacheable(UNCACHEABLE_RAND);
    break;
  }
  case 2:
  {
    Item *param_1= item_list->pop();
    Item *param_2= item_list->pop();
    func= new (thd->mem_root) Item_func_geometry_from_wkb(thd, param_1, param_2);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


class Create_func_geometry_from_json : public Create_native_func
{
public:
  Item *create_native(THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
    override;

  static Create_func_geometry_from_json s_singleton;

protected:
  Create_func_geometry_from_json() {}
  virtual ~Create_func_geometry_from_json() {}
};


Item*
Create_func_geometry_from_json::create_native(THD *thd,
                                              const LEX_CSTRING *name,
                                              List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 1:
  {
    Item *json= item_list->pop();
    func= new (thd->mem_root) Item_func_geometry_from_json(thd, json);
    thd->lex->uncacheable(UNCACHEABLE_RAND);
    break;
  }
  case 2:
  {
    Item *json= item_list->pop();
    Item *options= item_list->pop();
    func= new (thd->mem_root) Item_func_geometry_from_json(thd, json, options);
    break;
  }
  case 3:
  {
    Item *json= item_list->pop();
    Item *options= item_list->pop();
    Item *srid= item_list->pop();
    func= new (thd->mem_root) Item_func_geometry_from_json(thd, json, options,
                                                           srid);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


class Create_func_as_geojson : public Create_native_func
{
public:
  Item *create_native(THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
    override;

  static Create_func_as_geojson s_singleton;

protected:
  Create_func_as_geojson() {}
  virtual ~Create_func_as_geojson() {}
};


Item*
Create_func_as_geojson::create_native(THD *thd, const LEX_CSTRING *name,
                                      List<Item> *item_list)
{
  Item *func= NULL;
  int arg_count= 0;

  if (item_list != NULL)
    arg_count= item_list->elements;

  switch (arg_count) {
  case 1:
  {
    Item *geom= item_list->pop();
    func= new (thd->mem_root) Item_func_as_geojson(thd, geom);
    thd->lex->uncacheable(UNCACHEABLE_RAND);
    break;
  }
  case 2:
  {
    Item *geom= item_list->pop();
    Item *max_dec= item_list->pop();
    func= new (thd->mem_root) Item_func_as_geojson(thd, geom, max_dec);
    break;
  }
  case 3:
  {
    Item *geom= item_list->pop();
    Item *max_dec= item_list->pop();
    Item *options= item_list->pop();
    func= new (thd->mem_root) Item_func_as_geojson(thd, geom, max_dec, options);
    break;
  }
  default:
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    break;
  }
  }

  return func;
}


class Create_func_geometry_type : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_geometry_type(thd, arg1);
  }

  static Create_func_geometry_type s_singleton;

protected:
  Create_func_geometry_type() {}
  virtual ~Create_func_geometry_type() {}
};


class Create_func_geometryn : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_decomp_n(thd, arg1, arg2,
                                                      Item_func::SP_GEOMETRYN);
  }

  static Create_func_geometryn s_singleton;

protected:
  Create_func_geometryn() {}
  virtual ~Create_func_geometryn() {}
};


#if !defined(DBUG_OFF)
class Create_func_gis_debug : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_gis_debug(thd, arg1);
  }

  static Create_func_gis_debug s_singleton;

protected:
  Create_func_gis_debug() {}
  virtual ~Create_func_gis_debug() {}
};
#endif


class Create_func_glength : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_glength(thd, arg1);
  }

  static Create_func_glength s_singleton;

protected:
  Create_func_glength() {}
  virtual ~Create_func_glength() {}
};


class Create_func_interiorringn : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_decomp_n(thd, arg1, arg2,
                                                  Item_func::SP_INTERIORRINGN);
  }

  static Create_func_interiorringn s_singleton;

protected:
  Create_func_interiorringn() {}
  virtual ~Create_func_interiorringn() {}
};


class Create_func_relate : public Create_func_arg3
{
public:
  Item *create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3) override
  {
    return new (thd->mem_root) Item_func_spatial_relate(thd, arg1, arg2, arg3);
  }

  static Create_func_relate s_singleton;

protected:
  Create_func_relate() {}
  virtual ~Create_func_relate() {}
};


class Create_func_mbr_intersects : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_mbr_rel(thd, arg1, arg2,
        Item_func::SP_INTERSECTS_FUNC);
  }

  static Create_func_mbr_intersects s_singleton;

protected:
  Create_func_mbr_intersects() {}
  virtual ~Create_func_mbr_intersects() {}
};


class Create_func_intersects : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_precise_rel(thd, arg1, arg2,
                                               Item_func::SP_INTERSECTS_FUNC);
  }

  static Create_func_intersects s_singleton;

protected:
  Create_func_intersects() {}
  virtual ~Create_func_intersects() {}
};


class Create_func_intersection : public Create_func_arg2
{
public:
  Item* create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_operation(thd, arg1, arg2,
                                 Gcalc_function::op_intersection);
  }

  static Create_func_intersection s_singleton;

protected:
  Create_func_intersection() {}
  virtual ~Create_func_intersection() {}
};


class Create_func_difference : public Create_func_arg2
{
public:
  Item* create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_operation(thd, arg1, arg2,
                                 Gcalc_function::op_difference);
  }

  static Create_func_difference s_singleton;

protected:
  Create_func_difference() {}
  virtual ~Create_func_difference() {}
};


class Create_func_union : public Create_func_arg2
{
public:
  Item* create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_operation(thd, arg1, arg2,
                                 Gcalc_function::op_union);
  }

  static Create_func_union s_singleton;

protected:
  Create_func_union() {}
  virtual ~Create_func_union() {}
};


class Create_func_symdifference : public Create_func_arg2
{
public:
  Item* create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_operation(thd, arg1, arg2,
                                 Gcalc_function::op_symdifference);
  }

  static Create_func_symdifference s_singleton;

protected:
  Create_func_symdifference() {}
  virtual ~Create_func_symdifference() {}
};


class Create_func_buffer : public Create_func_arg2
{
public:
  Item* create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_buffer(thd, arg1, arg2);
  }

  static Create_func_buffer s_singleton;

protected:
  Create_func_buffer() {}
  virtual ~Create_func_buffer() {}
};


class Create_func_isclosed : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_isclosed(thd, arg1);
  }

  static Create_func_isclosed s_singleton;

protected:
  Create_func_isclosed() {}
  virtual ~Create_func_isclosed() {}
};


class Create_func_isring : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_isring(thd, arg1);
  }

  static Create_func_isring s_singleton;

protected:
  Create_func_isring() {}
  virtual ~Create_func_isring() {}
};


class Create_func_isempty : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_isempty(thd, arg1);
  }

  static Create_func_isempty s_singleton;

protected:
  Create_func_isempty() {}
  virtual ~Create_func_isempty() {}
};


class Create_func_issimple : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_issimple(thd, arg1);
  }

  static Create_func_issimple s_singleton;

protected:
  Create_func_issimple() {}
  virtual ~Create_func_issimple() {}
};



class Create_func_numgeometries : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_numgeometries(thd, arg1);
  }

  static Create_func_numgeometries s_singleton;

protected:
  Create_func_numgeometries() {}
  virtual ~Create_func_numgeometries() {}
};


class Create_func_numinteriorring : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_numinteriorring(thd, arg1);
  }

  static Create_func_numinteriorring s_singleton;

protected:
  Create_func_numinteriorring() {}
  virtual ~Create_func_numinteriorring() {}
};


class Create_func_numpoints : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_numpoints(thd, arg1);
  }

  static Create_func_numpoints s_singleton;

protected:
  Create_func_numpoints() {}
  virtual ~Create_func_numpoints() {}
};


class Create_func_mbr_overlaps : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_mbr_rel(thd, arg1, arg2,
        Item_func::SP_OVERLAPS_FUNC);
  }

  static Create_func_mbr_overlaps s_singleton;

protected:
  Create_func_mbr_overlaps() {}
  virtual ~Create_func_mbr_overlaps() {}
};


class Create_func_overlaps : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_precise_rel(thd, arg1, arg2,
                                                  Item_func::SP_OVERLAPS_FUNC);
  }

  static Create_func_overlaps s_singleton;

protected:
  Create_func_overlaps() {}
  virtual ~Create_func_overlaps() {}
};





class Create_func_pointn : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_decomp_n(thd, arg1, arg2,
                                                          Item_func::SP_POINTN);
  }
  static Create_func_pointn s_singleton;

protected:
  Create_func_pointn() {}
  virtual ~Create_func_pointn() {}
};




class Create_func_srid : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_srid(thd, arg1);
  }

  static Create_func_srid s_singleton;

protected:
  Create_func_srid() {}
  virtual ~Create_func_srid() {}
};


class Create_func_startpoint : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_spatial_decomp(thd, arg1,
                                                     Item_func::SP_STARTPOINT);
  }

  static Create_func_startpoint s_singleton;

protected:
  Create_func_startpoint() {}
  virtual ~Create_func_startpoint() {}
};



class Create_func_touches : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_precise_rel(thd, arg1, arg2,
                                                   Item_func::SP_TOUCHES_FUNC);
  }

  static Create_func_touches s_singleton;

protected:
  Create_func_touches() {}
  virtual ~Create_func_touches() {}
};


class Create_func_mbr_within : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_mbr_rel(thd, arg1, arg2,
      Item_func::SP_WITHIN_FUNC);
  }

  static Create_func_mbr_within s_singleton;

protected:
  Create_func_mbr_within() {}
  virtual ~Create_func_mbr_within() {}
};


class Create_func_within : public Create_func_arg2
{
public:
  Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) override
  {
    return new (thd->mem_root) Item_func_spatial_precise_rel(thd, arg1, arg2,
                                                   Item_func::SP_WITHIN_FUNC);
  }

  static Create_func_within s_singleton;

protected:
  Create_func_within() {}
  virtual ~Create_func_within() {}
};


class Create_func_x : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_x(thd, arg1);
  }

  static Create_func_x s_singleton;

protected:
  Create_func_x() {}
  virtual ~Create_func_x() {}
};


class Create_func_y : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_y(thd, arg1);
  }

  static Create_func_y s_singleton;

protected:
  Create_func_y() {}
  virtual ~Create_func_y() {}
};


/*****************************************************************/







/*************************************************************************/

#if !defined(DBUG_OFF)
Create_func_gis_debug Create_func_gis_debug::s_singleton;
#endif

Create_func_area Create_func_area::s_singleton;
Create_func_as_geojson Create_func_as_geojson::s_singleton;
Create_func_as_wkb Create_func_as_wkb::s_singleton;
Create_func_as_wkt Create_func_as_wkt::s_singleton;
Create_func_boundary Create_func_boundary::s_singleton;
Create_func_buffer Create_func_buffer::s_singleton;
Create_func_centroid Create_func_centroid::s_singleton;
Create_func_contains Create_func_contains::s_singleton;
Create_func_convexhull Create_func_convexhull::s_singleton;
Create_func_crosses Create_func_crosses::s_singleton;
Create_func_difference Create_func_difference::s_singleton;
Create_func_dimension Create_func_dimension::s_singleton;
Create_func_disjoint Create_func_disjoint::s_singleton;
Create_func_distance Create_func_distance::s_singleton;
Create_func_distance_sphere Create_func_distance_sphere::s_singleton;
Create_func_endpoint Create_func_endpoint::s_singleton;
Create_func_envelope Create_func_envelope::s_singleton;
Create_func_equals Create_func_equals::s_singleton;
Create_func_exteriorring Create_func_exteriorring::s_singleton;
Create_func_geometry_from_json Create_func_geometry_from_json::s_singleton;
Create_func_geometry_from_text Create_func_geometry_from_text::s_singleton;
Create_func_geometry_from_wkb Create_func_geometry_from_wkb::s_singleton;
Create_func_geometryn Create_func_geometryn::s_singleton;
Create_func_geometry_type Create_func_geometry_type::s_singleton;
Create_func_glength Create_func_glength::s_singleton;
Create_func_interiorringn Create_func_interiorringn::s_singleton;
Create_func_intersection Create_func_intersection::s_singleton;
Create_func_intersects Create_func_intersects::s_singleton;
Create_func_isclosed Create_func_isclosed::s_singleton;
Create_func_isempty Create_func_isempty::s_singleton;
Create_func_isring Create_func_isring::s_singleton;
Create_func_issimple Create_func_issimple::s_singleton;
Create_func_mbr_contains Create_func_mbr_contains::s_singleton;
Create_func_mbr_disjoint Create_func_mbr_disjoint::s_singleton;
Create_func_mbr_equals Create_func_mbr_equals::s_singleton;
Create_func_mbr_intersects Create_func_mbr_intersects::s_singleton;
Create_func_mbr_overlaps Create_func_mbr_overlaps::s_singleton;
Create_func_mbr_within Create_func_mbr_within::s_singleton;
Create_func_numgeometries Create_func_numgeometries::s_singleton;
Create_func_numinteriorring Create_func_numinteriorring::s_singleton;
Create_func_numpoints Create_func_numpoints::s_singleton;
Create_func_overlaps Create_func_overlaps::s_singleton;
Create_func_pointn Create_func_pointn::s_singleton;
Create_func_pointonsurface Create_func_pointonsurface::s_singleton;
Create_func_relate Create_func_relate::s_singleton;
Create_func_srid Create_func_srid::s_singleton;
Create_func_startpoint Create_func_startpoint::s_singleton;
Create_func_symdifference Create_func_symdifference::s_singleton;
Create_func_touches Create_func_touches::s_singleton;
Create_func_union Create_func_union::s_singleton;
Create_func_within Create_func_within::s_singleton;
Create_func_x Create_func_x::s_singleton;
Create_func_y Create_func_y::s_singleton;

/*************************************************************************/


#define GEOM_BUILDER(F) & F::s_singleton


static Native_func_registry func_array_geom[] =
{
#ifndef DBUG_OFF
    { { STRING_WITH_LEN("ST_GIS_DEBUG") }, GEOM_BUILDER(Create_func_gis_debug)},
#endif
  { { STRING_WITH_LEN("AREA") }, GEOM_BUILDER(Create_func_area)},
  { { STRING_WITH_LEN("ASBINARY") }, GEOM_BUILDER(Create_func_as_wkb)},
  { { STRING_WITH_LEN("ASTEXT") }, GEOM_BUILDER(Create_func_as_wkt)},
  { { STRING_WITH_LEN("ASWKB") }, GEOM_BUILDER(Create_func_as_wkb)},
  { { STRING_WITH_LEN("ASWKT") }, GEOM_BUILDER(Create_func_as_wkt)},
  { { STRING_WITH_LEN("BOUNDARY") }, GEOM_BUILDER(Create_func_boundary)},
  { { STRING_WITH_LEN("BUFFER") }, GEOM_BUILDER(Create_func_buffer)},
  { { STRING_WITH_LEN("CENTROID") }, GEOM_BUILDER(Create_func_centroid)},
  { { STRING_WITH_LEN("CONTAINS") }, GEOM_BUILDER(Create_func_contains)},
  { { STRING_WITH_LEN("CONVEXHULL") }, GEOM_BUILDER(Create_func_convexhull)},
  { { STRING_WITH_LEN("CROSSES") }, GEOM_BUILDER(Create_func_crosses)},
  { { STRING_WITH_LEN("DIMENSION") }, GEOM_BUILDER(Create_func_dimension)},
  { { STRING_WITH_LEN("DISJOINT") }, GEOM_BUILDER(Create_func_mbr_disjoint)},
  { { STRING_WITH_LEN("ENDPOINT") }, GEOM_BUILDER(Create_func_endpoint)},
  { { STRING_WITH_LEN("ENVELOPE") }, GEOM_BUILDER(Create_func_envelope)},
  { { STRING_WITH_LEN("EQUALS") }, GEOM_BUILDER(Create_func_equals)},
  { { STRING_WITH_LEN("EXTERIORRING") }, GEOM_BUILDER(Create_func_exteriorring)},
  { { STRING_WITH_LEN("GEOMCOLLFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("GEOMCOLLFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("GEOMETRYCOLLECTIONFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("GEOMETRYCOLLECTIONFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("GEOMETRYFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("GEOMETRYFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("GEOMETRYN") }, GEOM_BUILDER(Create_func_geometryn)},
  { { STRING_WITH_LEN("GEOMETRYTYPE") }, GEOM_BUILDER(Create_func_geometry_type)},
  { { STRING_WITH_LEN("GEOMFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("GEOMFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("GLENGTH") }, GEOM_BUILDER(Create_func_glength)},
  { { STRING_WITH_LEN("INTERIORRINGN") }, GEOM_BUILDER(Create_func_interiorringn)},
  { { STRING_WITH_LEN("INTERSECTS") }, GEOM_BUILDER(Create_func_mbr_intersects)},
  { { STRING_WITH_LEN("ISCLOSED") }, GEOM_BUILDER(Create_func_isclosed)},
  { { STRING_WITH_LEN("ISEMPTY") }, GEOM_BUILDER(Create_func_isempty)},
  { { STRING_WITH_LEN("ISRING") }, GEOM_BUILDER(Create_func_isring)},
  { { STRING_WITH_LEN("ISSIMPLE") }, GEOM_BUILDER(Create_func_issimple)},
  { { STRING_WITH_LEN("LINEFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("LINEFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("LINESTRINGFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("LINESTRINGFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("MBRCONTAINS") }, GEOM_BUILDER(Create_func_mbr_contains)},
  { { STRING_WITH_LEN("MBRDISJOINT") }, GEOM_BUILDER(Create_func_mbr_disjoint)},
  { { STRING_WITH_LEN("MBREQUAL") }, GEOM_BUILDER(Create_func_mbr_equals)},
  { { STRING_WITH_LEN("MBREQUALS") }, GEOM_BUILDER(Create_func_mbr_equals)},
  { { STRING_WITH_LEN("MBRINTERSECTS") }, GEOM_BUILDER(Create_func_mbr_intersects)},
  { { STRING_WITH_LEN("MBROVERLAPS") }, GEOM_BUILDER(Create_func_mbr_overlaps)},
  { { STRING_WITH_LEN("MBRTOUCHES") }, GEOM_BUILDER(Create_func_touches)},
  { { STRING_WITH_LEN("MBRWITHIN") }, GEOM_BUILDER(Create_func_mbr_within)},
  { { STRING_WITH_LEN("MLINEFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("MLINEFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("MPOINTFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("MPOINTFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("MPOLYFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("MPOLYFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("MULTILINESTRINGFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("MULTILINESTRINGFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("MULTIPOINTFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("MULTIPOINTFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("MULTIPOLYGONFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("MULTIPOLYGONFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("NUMGEOMETRIES") }, GEOM_BUILDER(Create_func_numgeometries)},
  { { STRING_WITH_LEN("NUMINTERIORRINGS") }, GEOM_BUILDER(Create_func_numinteriorring)},
  { { STRING_WITH_LEN("NUMPOINTS") }, GEOM_BUILDER(Create_func_numpoints)},
  { { STRING_WITH_LEN("OVERLAPS") }, GEOM_BUILDER(Create_func_mbr_overlaps)},
  { { STRING_WITH_LEN("POINTFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("POINTFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("POINTN") }, GEOM_BUILDER(Create_func_pointn)},
  { { STRING_WITH_LEN("POINTONSURFACE") }, GEOM_BUILDER(Create_func_pointonsurface)},
  { { STRING_WITH_LEN("POLYFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("POLYFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("POLYGONFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("POLYGONFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("SRID") }, GEOM_BUILDER(Create_func_srid)},
  { { STRING_WITH_LEN("ST_AREA") }, GEOM_BUILDER(Create_func_area)},
  { { STRING_WITH_LEN("STARTPOINT") }, GEOM_BUILDER(Create_func_startpoint)},
  { { STRING_WITH_LEN("ST_ASBINARY") }, GEOM_BUILDER(Create_func_as_wkb)},
  { { STRING_WITH_LEN("ST_ASGEOJSON") }, GEOM_BUILDER(Create_func_as_geojson)},
  { { STRING_WITH_LEN("ST_ASTEXT") }, GEOM_BUILDER(Create_func_as_wkt)},
  { { STRING_WITH_LEN("ST_ASWKB") }, GEOM_BUILDER(Create_func_as_wkb)},
  { { STRING_WITH_LEN("ST_ASWKT") }, GEOM_BUILDER(Create_func_as_wkt)},
  { { STRING_WITH_LEN("ST_BOUNDARY") }, GEOM_BUILDER(Create_func_boundary)},
  { { STRING_WITH_LEN("ST_BUFFER") }, GEOM_BUILDER(Create_func_buffer)},
  { { STRING_WITH_LEN("ST_CENTROID") }, GEOM_BUILDER(Create_func_centroid)},
  { { STRING_WITH_LEN("ST_CONTAINS") }, GEOM_BUILDER(Create_func_contains)},
  { { STRING_WITH_LEN("ST_CONVEXHULL") }, GEOM_BUILDER(Create_func_convexhull)},
  { { STRING_WITH_LEN("ST_CROSSES") }, GEOM_BUILDER(Create_func_crosses)},
  { { STRING_WITH_LEN("ST_DIFFERENCE") }, GEOM_BUILDER(Create_func_difference)},
  { { STRING_WITH_LEN("ST_DIMENSION") }, GEOM_BUILDER(Create_func_dimension)},
  { { STRING_WITH_LEN("ST_DISJOINT") }, GEOM_BUILDER(Create_func_disjoint)},
  { { STRING_WITH_LEN("ST_DISTANCE") }, GEOM_BUILDER(Create_func_distance)},
  { { STRING_WITH_LEN("ST_ENDPOINT") }, GEOM_BUILDER(Create_func_endpoint)},
  { { STRING_WITH_LEN("ST_ENVELOPE") }, GEOM_BUILDER(Create_func_envelope)},
  { { STRING_WITH_LEN("ST_EQUALS") }, GEOM_BUILDER(Create_func_equals)},
  { { STRING_WITH_LEN("ST_EQUALS") }, GEOM_BUILDER(Create_func_equals)},
  { { STRING_WITH_LEN("ST_EXTERIORRING") }, GEOM_BUILDER(Create_func_exteriorring)},
  { { STRING_WITH_LEN("ST_GEOMCOLLFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_GEOMCOLLFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_GEOMETRYCOLLECTIONFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_GEOMETRYCOLLECTIONFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_GEOMETRYFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_GEOMETRYFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_GEOMETRYN") }, GEOM_BUILDER(Create_func_geometryn)},
  { { STRING_WITH_LEN("ST_GEOMETRYTYPE") }, GEOM_BUILDER(Create_func_geometry_type)},
  { { STRING_WITH_LEN("ST_GEOMFROMGEOJSON") }, GEOM_BUILDER(Create_func_geometry_from_json)},
  { { STRING_WITH_LEN("ST_GEOMFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_GEOMFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_INTERIORRINGN") }, GEOM_BUILDER(Create_func_interiorringn)},
  { { STRING_WITH_LEN("ST_INTERSECTION") }, GEOM_BUILDER(Create_func_intersection)},
  { { STRING_WITH_LEN("ST_INTERSECTS") }, GEOM_BUILDER(Create_func_intersects)},
  { { STRING_WITH_LEN("ST_ISCLOSED") }, GEOM_BUILDER(Create_func_isclosed)},
  { { STRING_WITH_LEN("ST_ISEMPTY") }, GEOM_BUILDER(Create_func_isempty)},
  { { STRING_WITH_LEN("ST_ISRING") }, GEOM_BUILDER(Create_func_isring)},
  { { STRING_WITH_LEN("ST_ISSIMPLE") }, GEOM_BUILDER(Create_func_issimple)},
  { { STRING_WITH_LEN("ST_LENGTH") }, GEOM_BUILDER(Create_func_glength)},
  { { STRING_WITH_LEN("ST_LINEFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_LINEFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_LINESTRINGFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_LINESTRINGFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_MLINEFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_MLINEFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_MPOINTFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_MPOINTFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_MPOLYFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_MPOLYFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_MULTILINESTRINGFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_MULTILINESTRINGFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_MULTIPOINTFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_MULTIPOINTFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_MULTIPOLYGONFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_MULTIPOLYGONFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_NUMGEOMETRIES") }, GEOM_BUILDER(Create_func_numgeometries)},
  { { STRING_WITH_LEN("ST_NUMINTERIORRINGS") }, GEOM_BUILDER(Create_func_numinteriorring)},
  { { STRING_WITH_LEN("ST_NUMPOINTS") }, GEOM_BUILDER(Create_func_numpoints)},
  { { STRING_WITH_LEN("ST_OVERLAPS") }, GEOM_BUILDER(Create_func_overlaps)},
  { { STRING_WITH_LEN("ST_POINTFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_POINTFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_POINTN") }, GEOM_BUILDER(Create_func_pointn)},
  { { STRING_WITH_LEN("ST_POINTONSURFACE") }, GEOM_BUILDER(Create_func_pointonsurface)},
  { { STRING_WITH_LEN("ST_POLYFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_POLYFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_POLYGONFROMTEXT") }, GEOM_BUILDER(Create_func_geometry_from_text)},
  { { STRING_WITH_LEN("ST_POLYGONFROMWKB") }, GEOM_BUILDER(Create_func_geometry_from_wkb)},
  { { STRING_WITH_LEN("ST_RELATE") }, GEOM_BUILDER(Create_func_relate)},
  { { STRING_WITH_LEN("ST_SRID") }, GEOM_BUILDER(Create_func_srid)},
  { { STRING_WITH_LEN("ST_STARTPOINT") }, GEOM_BUILDER(Create_func_startpoint)},
  { { STRING_WITH_LEN("ST_SYMDIFFERENCE") }, GEOM_BUILDER(Create_func_symdifference)},
  { { STRING_WITH_LEN("ST_TOUCHES") }, GEOM_BUILDER(Create_func_touches)},
  { { STRING_WITH_LEN("ST_UNION") }, GEOM_BUILDER(Create_func_union)},
  { { STRING_WITH_LEN("ST_WITHIN") }, GEOM_BUILDER(Create_func_within)},
  { { STRING_WITH_LEN("ST_X") }, GEOM_BUILDER(Create_func_x)},
  { { STRING_WITH_LEN("ST_Y") }, GEOM_BUILDER(Create_func_y)},
  { { STRING_WITH_LEN("ST_DISTANCE_SPHERE") }, GEOM_BUILDER(Create_func_distance_sphere)},
  { { STRING_WITH_LEN("TOUCHES") }, GEOM_BUILDER(Create_func_touches)},
  { { STRING_WITH_LEN("WITHIN") }, GEOM_BUILDER(Create_func_within)},
  { { STRING_WITH_LEN("X") }, GEOM_BUILDER(Create_func_x)},
  { { STRING_WITH_LEN("Y") }, GEOM_BUILDER(Create_func_y)},
};


Native_func_registry_array
  native_func_registry_array_geom(func_array_geom,
                                  array_elements(func_array_geom));

#endif /*HAVE_SPATIAL*/

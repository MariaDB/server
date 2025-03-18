/*
   Copyright (c) 2002, 2013, Oracle and/or its affiliates.
   Copyright (c) 2011, 2021, MariaDB Corporation.

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

#include "mariadb.h"
#include "sql_priv.h"
#include "spatial.h"
#include "gstream.h"                            // Gis_read_stream
#include "sql_string.h"                         // String
#include <vector>

/* This is from item_func.h. Didn't want to #include the whole file. */
double my_double_round(double value, longlong dec, bool dec_unsigned,
                       bool truncate);

/*
  exponential notation :
  1   sign
  1   number before the decimal point
  1   decimal point
  14  number of significant digits (see String::qs_append(double))
  1   'e' sign
  1   exponent sign
  3   exponent digits
  ==
  22

  "f" notation :
  1   optional 0
  1   sign
  14  number significant digits (see String::qs_append(double) )
  1   decimal point
  ==
  17
*/

#define MAX_DIGITS_IN_DOUBLE MY_GCVT_MAX_FIELD_WIDTH

int MBR::within(const MBR *mbr)
{
  /*
    We have to take into account the 'dimension' of
    the MBR, where the dimension of a single point is 0,
    the dimension of a vertical or horizontal line is 1,
    and finally the dimension of the solid rectangle is 2.
  */
    
  int dim1= dimension();
  int dim2= mbr->dimension();

  DBUG_ASSERT(dim1 >= 0 && dim1 <= 2 && dim2 >= 0 && dim2 <= 2);

  /*
    Either/both of the two operands can degrade to a point or a
    horizontal/vertical line segment, and we have to treat such cases
    separately.
   */
  switch (dim1)
  {
  case 0:
    DBUG_ASSERT(xmin == xmax && ymin == ymax);
    switch (dim2)
    {
    case 0:
      DBUG_ASSERT(mbr->xmin == mbr->xmax && mbr->ymin == mbr->ymax);
      return equals(mbr);
      break;
    case 1:
      DBUG_ASSERT((mbr->xmin == mbr->xmax && mbr->ymin != mbr->ymax) ||
                  (mbr->ymin == mbr->ymax && mbr->xmin != mbr->xmax));
      return ((xmin > mbr->xmin && xmin < mbr->xmax && ymin == mbr->ymin) ||
              (ymin > mbr->ymin && ymin < mbr->ymax && xmin == mbr->xmin));
      break;
    case 2:
      DBUG_ASSERT(mbr->xmin != mbr->xmax && mbr->ymin != mbr->ymax);
      return (xmin > mbr->xmin && xmax < mbr->xmax &&
              ymin > mbr->ymin && ymax < mbr->ymax);
      break;
    }
    break;
  case 1:
    DBUG_ASSERT((xmin == xmax && ymin != ymax) ||
                (ymin == ymax && xmin != xmax));
    switch (dim2)
    {
    case 0:
      DBUG_ASSERT(mbr->xmin == mbr->xmax && mbr->ymin == mbr->ymax);
      return 0;
      break;
    case 1:
      DBUG_ASSERT((mbr->xmin == mbr->xmax && mbr->ymin != mbr->ymax) ||
                  (mbr->ymin == mbr->ymax && mbr->xmin != mbr->xmax));
      return ((xmin == xmax && mbr->xmin == mbr->xmax && mbr->xmin == xmin &&
               mbr->ymin <= ymin && mbr->ymax >= ymax) ||
              (ymin == ymax && mbr->ymin == mbr->ymax && mbr->ymin == ymin &&
               mbr->xmin <= xmin && mbr->xmax >= xmax));
      break;
    case 2:
      DBUG_ASSERT(mbr->xmin != mbr->xmax && mbr->ymin != mbr->ymax);
      return ((xmin == xmax && xmin > mbr->xmin && xmax < mbr->xmax &&
               ymin >= mbr->ymin && ymax <= mbr->ymax) ||
              (ymin == ymax && ymin > mbr->ymin && ymax < mbr->ymax &&
               xmin >= mbr->xmin && xmax <= mbr->xmax));
      break;
    }
    break;
  case 2:
    DBUG_ASSERT(xmin != xmax && ymin != ymax);
    switch (dim2)
    {
    case 0:
    case 1:
      return 0;
      break;
    case 2:
      DBUG_ASSERT(mbr->xmin != mbr->xmax && mbr->ymin != mbr->ymax);
      return ((mbr->xmin <= xmin) && (mbr->ymin <= ymin) &&
              (mbr->xmax >= xmax) && (mbr->ymax >= ymax));
      break;

    }
    break;
  }

  // Never reached.
  DBUG_ASSERT(false);
  return 0;
}


int MBR::coveredby(const MBR *mbr)
{
  int dim1= dimension();
  int dim2= mbr->dimension();

  if (dim1 > dim2)
    return 0;
  else if (dim1 == 0 && dim2 == 0)
    return equals(mbr);

  return ((xmin >= mbr->xmin) && (xmax <= mbr->xmax) &&
          (ymin >= mbr->ymin) && (ymax <= mbr->ymax));
}


/********************** Ramer–Douglas–Peucker algorithm **********************/

static double perpendicular_distance(const st_point_2d& point,
                            const st_point_2d& line_start,
                            const st_point_2d& line_end) {
  double difference_x= line_end.x - line_start.x;
  double difference_y= line_end.y - line_start.y;

  double magnitude = sqrt((difference_x * difference_x) +
                               (difference_y * difference_y));

  if (magnitude > 0.0)
  {
    difference_x /= magnitude;
    difference_y /= magnitude;
  }

  double point_vector_x= point.x - line_start.x;
  double point_vector_y= point.y - line_start.y;

  double point_vector_dot_product= ((difference_x * point_vector_x) +
                                   (difference_y * point_vector_y));

  double adjusted_x= point_vector_x - (point_vector_dot_product * difference_x);
  double adjusted_y= point_vector_y - (point_vector_dot_product * difference_y);

  return sqrt((adjusted_x * adjusted_x) + (adjusted_y * adjusted_y));
}


static void recursive_RDP(const std::vector<st_point_2d>& points,
                          const double max_distance,
                          std::vector<st_point_2d>& out,
                          const uint32 start, const uint32 end)
{
  if (start >= end) return;

  double greatest_distance= 0.0;
  uint32 index= start;

  for (uint32 i = start + 1; i < end; ++i) {
    double dist = perpendicular_distance(points[i], points[start], points[end]);
    if (dist > greatest_distance) {
      index = i;
      greatest_distance = dist;
    }
  }

  if (greatest_distance > max_distance)
  {
    recursive_RDP(points, max_distance, out, start, index);
    recursive_RDP(points, max_distance, out, index, end);
  } else if (start != 0)
    out.push_back(points[start]);
}


/*
  Implements the Ramer–Douglas–Peucker. Given the points that compose a line,
  finds a similar curve with fewer points. The simplified curve consists of a
  subset of the points that defined the original curve.
  https://en.wikipedia.org/wiki/Ramer%E2%80%93Douglas%E2%80%93Peucker_algorithm
*/
static void simplify_RDP(std::vector<st_point_2d>& points,
                         const double max_distance) {
  std::vector<st_point_2d> result;
  result.push_back(points.front());
  recursive_RDP(points, max_distance, result, 0,(uint32) points.size() - 1);
  result.push_back(points.back());
  points = std::move(result);
}


/***************************** Gis_class_info *******************************/

Geometry::Class_info *Geometry::ci_collection[Geometry::wkb_last+1]=
{
  NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static Geometry::Class_info **ci_collection_end=
                                Geometry::ci_collection+Geometry::wkb_last + 1;

Geometry::Class_info::Class_info(const char *name, const char *geojson_name,
                                 int type_id, create_geom_t create_func):
  m_type_id(type_id), m_create_func(create_func)
{
  m_name.str= (char *) name;
  m_name.length= strlen(name);
  m_geojson_name.str= (char *) geojson_name;
  m_geojson_name.length= strlen(geojson_name);

  ci_collection[type_id]= this;
}

static Geometry *create_point(char *buffer)
{
  return new (buffer) Gis_point;
}

static Geometry *create_linestring(char *buffer)
{
  return new (buffer) Gis_line_string;
}

static Geometry *create_polygon(char *buffer)
{
  return new (buffer) Gis_polygon;
}

static Geometry *create_multipoint(char *buffer)
{
  return new (buffer) Gis_multi_point;
}

static Geometry *create_multipolygon(char *buffer)
{
  return new (buffer) Gis_multi_polygon;
}

static Geometry *create_multilinestring(char *buffer)
{
  return new (buffer) Gis_multi_line_string;
}

static Geometry *create_geometrycollection(char *buffer)
{
  return new (buffer) Gis_geometry_collection;
}



static Geometry::Class_info point_class("POINT", "Point",
					Geometry::wkb_point, create_point);

static Geometry::Class_info linestring_class("LINESTRING", "LineString",
					     Geometry::wkb_linestring,
					     create_linestring);
static Geometry::Class_info polygon_class("POLYGON", "Polygon",
					      Geometry::wkb_polygon,
					      create_polygon);
static Geometry::Class_info multipoint_class("MULTIPOINT", "MultiPoint",
						 Geometry::wkb_multipoint,
						 create_multipoint);
static Geometry::Class_info 
multilinestring_class("MULTILINESTRING", "MultiLineString",
		      Geometry::wkb_multilinestring, create_multilinestring);
static Geometry::Class_info multipolygon_class("MULTIPOLYGON", "MultiPolygon",
						   Geometry::wkb_multipolygon,
						   create_multipolygon);
static Geometry::Class_info 
geometrycollection_class("GEOMETRYCOLLECTION", "GeometryCollection",
                         Geometry::wkb_geometrycollection,
			 create_geometrycollection);

static void get_point(double *x, double *y, const char *data)
{
  float8get(*x, data);
  float8get(*y, data + SIZEOF_STORED_DOUBLE);
}

/***************************** Geometry *******************************/

Geometry::Class_info *Geometry::find_class(const char *name, size_t len)
{
  for (Class_info **cur_rt= ci_collection;
       cur_rt < ci_collection_end; cur_rt++)
  {
    if (*cur_rt &&
	((*cur_rt)->m_name.length == len) &&
	(my_charset_latin1.strnncoll((*cur_rt)->m_name.str, len,
		                     name, len) == 0))
      return *cur_rt;
  }
  return 0;
}


Geometry *Geometry::create_by_typeid(Geometry_buffer *buffer, int type_id)
{
  Class_info *ci;
  if (!(ci= find_class(type_id)))
    return NULL;
  return (*ci->m_create_func)(buffer->data);
}


Geometry *Geometry::construct(Geometry_buffer *buffer,
                              const char *data, uint32 data_len)
{
  uint32 geom_type;
  Geometry *result;

  if (data_len < SRID_SIZE + WKB_HEADER_SIZE)   // < 4 + (1 + 4)
    return NULL;
  /* + 1 to skip the byte order (stored in position SRID_SIZE). */
  geom_type= uint4korr(data + SRID_SIZE + 1);
  if (!(result= create_by_typeid(buffer, (int) geom_type)))
    return NULL;
  result->m_data= data+ SRID_SIZE + WKB_HEADER_SIZE;
  result->m_data_end= data + data_len;
  return result;
}


uint Geometry::get_key_image_itMBR(LEX_CSTRING &src, uchar *buff, uint length)
{
  const char *dummy;
  MBR mbr;
  Geometry_buffer buffer;
  Geometry *gobj;
  const uint image_length= SIZEOF_STORED_DOUBLE*4;

  if (src.length < SRID_SIZE)
  {
    bzero(buff, image_length);
    return image_length;
  }
  gobj= Geometry::construct(&buffer, (char*) src.str, (uint32) src.length);
  if (!gobj || gobj->get_mbr(&mbr, &dummy))
    bzero(buff, image_length);
  else
  {
    float8store(buff,    mbr.xmin);
    float8store(buff+8,  mbr.xmax);
    float8store(buff+16, mbr.ymin);
    float8store(buff+24, mbr.ymax);
  }
  return image_length;
}


Geometry *Geometry::create_from_wkt(Geometry_buffer *buffer,
				    Gis_read_stream *trs, String *wkt,
				    bool init_stream)
{
  LEX_STRING name;
  Class_info *ci;
  char next_sym;

  if (trs->get_next_word(&name))
  {
    trs->set_error_msg("Geometry name expected");
    return NULL;
  }
  if (!(ci= find_class(name.str, name.length)) ||
      wkt->reserve(1 + 4, 512))
    return NULL;
  Geometry *result= (*ci->m_create_func)(buffer->data);
  wkt->q_append((char) wkb_ndr);
  wkt->q_append((uint32) result->get_class_info()->m_type_id);
  if (!(next_sym= trs->next_symbol()))
    return NULL;
  if (!(next_sym= trs->next_symbol()))
    return NULL;
  if ((next_sym == '(' && trs->check_next_symbol('(')) ||
      result->init_from_wkt(trs, wkt) ||
      (next_sym == '(' && trs->check_next_symbol(')')))
    return NULL;
  if (init_stream)  
  {
    result->set_data_ptr(wkt->ptr(), wkt->length());
    result->shift_wkb_header();
  }
  return result;
}


int Geometry::as_wkt(String *wkt, const char **end)
{
  uint32 len= (uint) get_class_info()->m_name.length;
  if (wkt->reserve(len + 2, 512))
    return 1;
  wkt->qs_append(get_class_info()->m_name.str, len);
  if (get_class_info() != &geometrycollection_class)
    wkt->qs_append('(');
  if (get_data_as_wkt(wkt, end))
    return 1;
  if (get_class_info() != &geometrycollection_class)
    wkt->qs_append(')');
  return 0;
}


static const uchar type_keyname[]= "type";
static const uint type_keyname_len= 4;
static const uchar coord_keyname[]= "coordinates";
static const uint coord_keyname_len= 11;
static const uchar geometries_keyname[]= "geometries";
static const uint geometries_keyname_len= 10;
static const uchar features_keyname[]= "features";
static const uint features_keyname_len= 8;
static const uchar geometry_keyname[]= "geometry";
static const uint geometry_keyname_len= 8;

static const uint max_keyname_len= 11; /*'coordinates' keyname is the longest.*/

static const uchar feature_type[]= "feature";
static const int feature_type_len= 7;
static const uchar feature_coll_type[]= "featurecollection";
static const int feature_coll_type_len= 17;
static const uchar bbox_keyname[]= "bbox";
static const int bbox_keyname_len= 4;


int Geometry::as_json(String *wkt, uint max_dec_digits, const char **end)
{
  uint32 len= (uint) get_class_info()->m_geojson_name.length;
  if (wkt->reserve(4 + type_keyname_len + 2 + len + 2 + 2 +
                   coord_keyname_len + 4, 512))
    return 1;
  wkt->qs_append('"');
  wkt->qs_append((const char *) type_keyname, type_keyname_len);
  wkt->qs_append("\": \"", 4);
  wkt->qs_append(get_class_info()->m_geojson_name.str, len);
  wkt->qs_append("\", \"", 4);
  if (get_class_info() == &geometrycollection_class)
    wkt->qs_append((const char *) geometries_keyname, geometries_keyname_len);
  else
    wkt->qs_append((const char *) coord_keyname, coord_keyname_len);

  wkt->qs_append("\": ", 3);
  if (get_data_as_json(wkt, max_dec_digits, end))
    return 1;

  return 0;
}


int Geometry::bbox_as_json(String *wkt)
{
  MBR mbr;
  const char *end;
  if (wkt->reserve(5 + bbox_keyname_len + (FLOATING_POINT_DECIMALS+2)*4, 512))
    return 1;
  wkt->qs_append('"');
  wkt->qs_append((const char *) bbox_keyname, bbox_keyname_len);
  wkt->qs_append("\": [", 4);

  if (get_mbr(&mbr, &end))
    return 1;

  wkt->qs_append(mbr.xmin);
  wkt->qs_append(", ", 2);
  wkt->qs_append(mbr.ymin);
  wkt->qs_append(", ", 2);
  wkt->qs_append(mbr.xmax);
  wkt->qs_append(", ", 2);
  wkt->qs_append(mbr.ymax);
  wkt->qs_append(']');

  return 0;
}


static double wkb_get_double(const char *ptr, Geometry::wkbByteOrder bo)
{
  double res;
  if (bo != Geometry::wkb_xdr)
  {
    float8get(res, ptr);
  }
  else
  {
    char inv_array[8];
    inv_array[0]= ptr[7];
    inv_array[1]= ptr[6];
    inv_array[2]= ptr[5];
    inv_array[3]= ptr[4];
    inv_array[4]= ptr[3];
    inv_array[5]= ptr[2];
    inv_array[6]= ptr[1];
    inv_array[7]= ptr[0];
    float8get(res, inv_array);
  }
  return res;
}


static uint32 wkb_get_uint(const char *ptr, Geometry::wkbByteOrder bo)
{
  if (bo != Geometry::wkb_xdr)
    return uint4korr(ptr);
  /* else */
  {
    char inv_array[4];
    inv_array[0]= ptr[3];
    inv_array[1]= ptr[2];
    inv_array[2]= ptr[1];
    inv_array[3]= ptr[0];
    return uint4korr(inv_array);
  }
}


Geometry *Geometry::create_from_wkb(Geometry_buffer *buffer,
                                    const char *wkb, uint32 len, String *res)
{
  uint32 geom_type;
  Geometry *geom;

  if (len < WKB_HEADER_SIZE)
    return NULL;
  geom_type= wkb_get_uint(wkb+1, (wkbByteOrder)wkb[0]);
  if (!(geom= create_by_typeid(buffer, (int) geom_type)) ||
      res->reserve(WKB_HEADER_SIZE, 512))
    return NULL;

  res->q_append((char) wkb_ndr);
  res->q_append(geom_type);

  return geom->init_from_wkb(wkb + WKB_HEADER_SIZE, len - WKB_HEADER_SIZE,
                             (wkbByteOrder) wkb[0], res) ? geom : NULL;
}


Geometry *Geometry::create_from_json(Geometry_buffer *buffer,
                      json_engine_t *je, bool er_on_3D, String *res)
{
  Class_info *ci= NULL;
  const uchar *coord_start= NULL, *geom_start= NULL,
              *features_start= NULL, *geometry_start= NULL;
  Geometry *result;
  uchar key_buf[max_keyname_len];
  uint key_len;
  int fcoll_type_found= 0, feature_type_found= 0;


  if (json_read_value(je))
    goto err_return;
  
  if (je->value_type != JSON_VALUE_OBJECT)
  {
    je->s.error= GEOJ_INCORRECT_GEOJSON;
    goto err_return;
  }

  while (json_scan_next(je) == 0 && je->state != JST_OBJ_END)
  {
    DBUG_ASSERT(je->state == JST_KEY);

    key_len=0;
    while (json_read_keyname_chr(je) == 0)
    {
      if (je->s.c_next > 127 || key_len >= max_keyname_len)
      {
        /* Symbol out of range, or keyname too long. No need to compare.. */
        key_len=0;
        break;
      }
      key_buf[key_len++]= (uchar)je->s.c_next | 0x20; /* make it lowercase. */
    }

    if (unlikely(je->s.error))
      goto err_return;

    if (key_len == type_keyname_len &&
        memcmp(key_buf, type_keyname, type_keyname_len) == 0)
    {
      /*
         Found the "type" key. Let's check it's a string and remember
         the feature's type.
      */
      if (json_read_value(je))
        goto err_return;

      if (je->value_type == JSON_VALUE_STRING)
      {
        if ((ci= find_class((const char *) je->value, je->value_len)))
        {
          if ((coord_start=
                (ci == &geometrycollection_class) ? geom_start : coord_start))
            goto create_geom;
        }
        else if (je->value_len == feature_coll_type_len &&
            my_charset_latin1.strnncoll(je->value, je->value_len,
		                        feature_coll_type, feature_coll_type_len) == 0)
        {
          /*
            'FeatureCollection' type found. Handle the 'Featurecollection'/'features'
            GeoJSON construction.
          */
          if (features_start)
            goto handle_feature_collection;
          fcoll_type_found= 1;
        }
        else if (je->value_len == feature_type_len &&
                 my_charset_latin1.strnncoll(je->value, je->value_len,
		                             feature_type, feature_type_len) == 0)
        {
          if (geometry_start)
            goto handle_geometry_key;
          feature_type_found= 1;
        }
        else /* can't understand the type. */
          break;
      }
      else /* The "type" value can only be string. */
        break;
    }
    else if (key_len == coord_keyname_len &&
             memcmp(key_buf, coord_keyname, coord_keyname_len) == 0)
    {
      /*
        Found the "coordinates" key. Let's check it's an array
        and remember where it starts.
      */
      if (json_read_value(je))
        goto err_return;

      if (je->value_type == JSON_VALUE_ARRAY)
      {
        coord_start= je->value_begin;
        if (ci && ci != &geometrycollection_class)
          goto create_geom;
        if (json_skip_level(je))
          goto err_return;
      }
    }
    else if (key_len == geometries_keyname_len &&
             memcmp(key_buf, geometries_keyname, geometries_keyname_len) == 0)
    {
      /*
        Found the "geometries" key. Let's check it's an array
        and remember where it starts.
      */
      if (json_read_value(je))
        goto err_return;

      if (je->value_type == JSON_VALUE_ARRAY)
      {
        geom_start= je->value_begin;
        if (ci == &geometrycollection_class)
        {
          coord_start= geom_start;
          goto create_geom;
        }
      }
    }
    else if (key_len == features_keyname_len &&
             memcmp(key_buf, features_keyname, features_keyname_len) == 0)
    {
      /*
        'features' key found. Handle the 'Featurecollection'/'features'
        GeoJSON construction.
      */
      if (json_read_value(je))
        goto err_return;
      if (je->value_type == JSON_VALUE_ARRAY)
      {
        features_start= je->value_begin;
        if (fcoll_type_found)
          goto handle_feature_collection;
      }
    }
    else if (key_len == geometry_keyname_len &&
             memcmp(key_buf, geometry_keyname, geometry_keyname_len) == 0)
    {
      if (json_read_value(je))
        goto err_return;
      if (je->value_type == JSON_VALUE_OBJECT)
      {
        geometry_start= je->value_begin;
        if (feature_type_found)
          goto handle_geometry_key;
      }
      else
        goto err_return;
    }
    else
    {
      if (json_skip_key(je))
        goto err_return;
    }
  }

  if (je->s.error == 0)
  {
    /*
      We didn't find all the required keys. That are "type" and "coordinates"
      or "geometries" for GeometryCollection.
    */
    je->s.error= GEOJ_INCORRECT_GEOJSON;
  }
  goto err_return;

handle_feature_collection:
  ci= &geometrycollection_class;
  coord_start= features_start;

create_geom:

  json_scan_start(je, je->s.cs, coord_start, je->s.str_end);

  if (res->reserve(1 + 4, 512))
    goto err_return;

  result= (*ci->m_create_func)(buffer->data);
  res->q_append((char) wkb_ndr);
  res->q_append((uint32) result->get_class_info()->m_type_id);
  if (result->init_from_json(je, er_on_3D, res))
    goto err_return;

  return result;

handle_geometry_key:
  json_scan_start(je, je->s.cs, geometry_start, je->s.str_end);
  return create_from_json(buffer, je, er_on_3D, res);

err_return:
  return NULL;
}


Geometry *Geometry::create_from_opresult(Geometry_buffer *g_buf,
                                   String *res, Gcalc_result_receiver &rr)
{
  uint32 geom_type= rr.get_result_typeid();
  Geometry *obj= create_by_typeid(g_buf, geom_type);

  if (!obj || res->reserve(WKB_HEADER_SIZE, 512))
    return NULL;

  res->q_append((char) wkb_ndr);
  res->q_append(geom_type);
  return obj->init_from_opresult(res, rr.result(), rr.length()) ? obj : NULL;
}


bool Geometry::envelope(String *result) const
{
  MBR mbr;
  const char *end;

  if (get_mbr(&mbr, &end))
    return 1;

  if (!mbr.valid())
  {
    /* Empty geometry */
    if (result->reserve(1 + 4*2))
      return 1;
    result->q_append((char) wkb_ndr);
    result->q_append((uint32) wkb_geometrycollection);
    result->q_append((uint32) 0);
    return 0;
  }
  if (result->reserve(1 + 4 * 3 + SIZEOF_STORED_DOUBLE * 10))
    return 1;

  result->q_append((char) wkb_ndr);
  result->q_append((uint32) wkb_polygon);
  result->q_append((uint32) 1);
  result->q_append((uint32) 5);
  result->q_append(mbr.xmin);
  result->q_append(mbr.ymin);
  result->q_append(mbr.xmax);
  result->q_append(mbr.ymin);
  result->q_append(mbr.xmax);
  result->q_append(mbr.ymax);
  result->q_append(mbr.xmin);
  result->q_append(mbr.ymax);
  result->q_append(mbr.xmin);
  result->q_append(mbr.ymin);

  return 0;
}

int Geometry::is_simple(int *simple) const {
  Gcalc_scan_iterator scan_it;
  Gcalc_heap collector;
  Gcalc_function func;
  Gcalc_operation_transporter trn(&func, &collector);
  const char *c_end;
  MBR mbr;
  *simple= 0;

 if(this->get_mbr(&mbr, &c_end))
    return 1;

  collector.set_extent(mbr.xmin, mbr.xmax, mbr.ymin, mbr.ymax);
  if (this->store_shapes(&trn))
    return 1;

  collector.prepare_operation();
  scan_it.init(&collector);

  while (scan_it.more_points())
  {
    const Gcalc_scan_iterator::event_point *ev, *next_ev;

    if (scan_it.step())
      return 1;

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

    return 0;
  }

  *simple= 1;
  return 0;
}

/*
  Create a point from data.

  SYNPOSIS
    create_point()
    result		Put result here
    data		Data for point is here.

  RETURN
    0	ok
    1	Can't reallocate 'result'
*/
bool Geometry::create_point(String *result, const char *data) const
{
  if (no_data(data, POINT_DATA_SIZE) ||
      result->reserve(1 + 4 + POINT_DATA_SIZE))
    return 1;
  result->q_append((char) wkb_ndr);
  result->q_append((uint32) wkb_point);
  /* Copy two double in same format */
  result->q_append(data, POINT_DATA_SIZE);
  return 0;
}

/*
  Create a point from coordinates.

  SYNPOSIS
    create_point()
    result		Put result here
    x			x coordinate for point
    y			y coordinate for point

  RETURN
    0	ok
    1	Can't reallocate 'result'
*/

bool Geometry::create_point(String *result, double x, double y)
{
  if (result->reserve(1 + 4 + POINT_DATA_SIZE))
    return 1;

  result->q_append((char) wkb_ndr);
  result->q_append((uint32) wkb_point);
  result->q_append(x);
  result->q_append(y);
  return 0;
}

/*
  Append N points from packed format to text

  SYNOPSIS
    append_points()
    txt			Append points here
    n_points		Number of points
    data		Packed data
    offset		Offset between points

  RETURN
    # end of data
*/

const char *Geometry::append_points(String *txt, uint32 n_points,
				    const char *data, uint32 offset) const
{			     
  while (n_points--)
  {
    double x,y;
    data+= offset;
    get_point(&x, &y, data);
    data+= POINT_DATA_SIZE;
    txt->qs_append(x);
    txt->qs_append(' ');
    txt->qs_append(y);
    txt->qs_append(',');
  }
  return data;
}


static void append_json_point(String *txt, uint max_dec, const char *data)
{
  double x,y;
  get_point(&x, &y, data);
  if (max_dec < FLOATING_POINT_DECIMALS)
  {
    x= my_double_round(x, max_dec, FALSE, FALSE);
    y= my_double_round(y, max_dec, FALSE, FALSE);
  }
  txt->qs_append('[');
  txt->qs_append(x);
  txt->qs_append(", ", 2);
  txt->qs_append(y);
  txt->qs_append(']');
}


/*
  Append N points from packed format to json

  SYNOPSIS
    append_json_points()
    txt			Append points here
    n_points		Number of points
    data		Packed data
    offset		Offset between points

  RETURN
    # end of data
*/

static const char *append_json_points(String *txt, uint max_dec,
    uint32 n_points, const char *data, uint32 offset)
{			     
  txt->qs_append('[');
  while (n_points--)
  {
    data+= offset;
    append_json_point(txt, max_dec, data);
    data+= POINT_DATA_SIZE;
    txt->qs_append(", ", 2);
  }
  txt->length(txt->length() - 2);// Remove ending ', '
  txt->qs_append(']');
  return data;
}
/*
  Get most bounding rectangle (mbr) for X points

  SYNOPSIS
    get_mbr_for_points()
    mbr			MBR (store rectangle here)
    points		Number of points
    data		Packed data
    offset		Offset between points

  RETURN
    0	Wrong data
    #	end of data
*/

const char *Geometry::get_mbr_for_points(MBR *mbr, const char *data,
					 uint offset) const
{
  uint32 points;
  /* read number of points */
  if (no_data(data, 4))
    return 0;
  points= uint4korr(data);
  data+= 4;

  if (not_enough_points(data, points, offset))
    return 0;

  /* Calculate MBR for points */
  while (points--)
  {
    data+= offset;
    mbr->add_xy(data, data + SIZEOF_STORED_DOUBLE);
    data+= POINT_DATA_SIZE;
  }
  return data;
}

const char* Geometry::get_points_common(const char* data,
                                        Geometry::PointContainer &points) const
{
  uint32 expected_points;
  if (no_data(data, 4))
    return nullptr;
  expected_points= uint4korr(data);
  data+= 4;

  if (not_enough_points(data, expected_points, 0))
    return nullptr;

  while (expected_points--)
  {
    double x, y;
    float8get(x, data);
    float8get(y, data + SIZEOF_STORED_DOUBLE);
    points.push_back(std::make_pair(x, y));
    data+= POINT_DATA_SIZE;
  }
  return data;
}



/***************************** Point *******************************/

uint32 Gis_point::get_data_size() const
{
  return POINT_DATA_SIZE;
}


bool Gis_point::init_from_wkt(Gis_read_stream *trs, String *wkb)
{
  double x, y;
  if (trs->get_next_number(&x) || trs->get_next_number(&y) ||
      wkb->reserve(POINT_DATA_SIZE, 512))
    return 1;
  wkb->q_append(x);
  wkb->q_append(y);
  return 0;
}


uint Gis_point::init_from_wkb(const char *wkb, uint len,
                              wkbByteOrder bo, String *res)
{
  double x, y;
  if (len < POINT_DATA_SIZE || res->reserve(POINT_DATA_SIZE))
    return 0;
  x= wkb_get_double(wkb, bo);
  y= wkb_get_double(wkb + SIZEOF_STORED_DOUBLE, bo);
  res->q_append(x);
  res->q_append(y);
  return POINT_DATA_SIZE;
}


static int read_point_from_json(json_engine_t *je, bool er_on_3D,
                                double *x, double *y)
{
  int n_coord= 0, err;
  double tmp, *d;
  char *endptr;

  while (json_scan_next(je) == 0 && je->state != JST_ARRAY_END)
  {
    DBUG_ASSERT(je->state == JST_VALUE);
    if (json_read_value(je))
      return 1;

    if (je->value_type != JSON_VALUE_NUMBER)
      goto bad_coordinates;

    d= (n_coord == 0) ? x : ((n_coord == 1) ? y : &tmp);
    *d= je->s.cs->strntod((char *) je->value, je->value_len, &endptr, &err);
    if (err)
      goto bad_coordinates;
    n_coord++;
  }

  if (n_coord <= 2 || !er_on_3D)
    return 0;
  je->s.error= Geometry::GEOJ_DIMENSION_NOT_SUPPORTED;
  return 1;
bad_coordinates:
  je->s.error= Geometry::GEOJ_INCORRECT_GEOJSON;
  return 1;
}


bool Gis_point::init_from_json(json_engine_t *je, bool er_on_3D, String *wkb)
{
  double x, y;
  if (json_read_value(je))
    return TRUE;

  if (je->value_type != JSON_VALUE_ARRAY)
  {
    je->s.error= GEOJ_INCORRECT_GEOJSON;
    return TRUE;
  }

  if (read_point_from_json(je, er_on_3D, &x, &y) ||
      wkb->reserve(POINT_DATA_SIZE))
    return TRUE;

  wkb->q_append(x);
  wkb->q_append(y);
  return FALSE;
}


bool Gis_point::get_data_as_wkt(String *txt, const char **end) const
{
  double x, y;
  if (get_xy(&x, &y))
    return 1;
  if (txt->reserve(MAX_DIGITS_IN_DOUBLE * 2 + 1))
    return 1;
  txt->qs_append(x);
  txt->qs_append(' ');
  txt->qs_append(y);
  *end= m_data+ POINT_DATA_SIZE;
  return 0;
}


bool Gis_point::get_data_as_json(String *txt, uint max_dec_digits,
                                 const char **end) const
{
  if (txt->reserve(MAX_DIGITS_IN_DOUBLE * 2 + 4))
    return 1;
  append_json_point(txt, max_dec_digits, m_data);
  *end= m_data+ POINT_DATA_SIZE;
  return 0;
}


bool Gis_point::get_mbr(MBR *mbr, const char **end) const
{
  double x, y;
  if (get_xy(&x, &y))
    return 1;
  mbr->add_xy(x, y);
  *end= m_data+ POINT_DATA_SIZE;
  return 0;
}


int Gis_point::is_valid(int *valid) const
{
  double x, y;
  if (get_xy(&x, &y))
    return 1;

  *valid= 1;
  return 0;
}


int Gis_point::area(double *ar, const char **end) const
{
  *ar= 0;
  *end= m_data+ POINT_DATA_SIZE;
  return 0;
}


int Gis_point::geom_length(double *len, const char **end) const
{
  *len= 0;
  *end= m_data+ POINT_DATA_SIZE;
  return 0;
}


int Gis_point::store_shapes(Gcalc_shape_transporter *trn) const
{
  double x, y;

  return get_xy(&x, &y) || trn->single_point(x, y);
}


const Geometry::Class_info *Gis_point::get_class_info() const
{
  return &point_class;
}


/**
  Function to calculate haversine.
  Taking as arguments Point and Multipoint geometries.
  Multipoint geometry has to be single point only.
  It is up to caller to ensure valid input.

  @param    g      pointer to the Geometry
  @param    r      sphere radius
  @param    error  pointer describing the error in case of the boundary conditions

  @return distance in case without error, it is calculated distance (non-negative),
                   in case error exist, negative value.
*/
double Gis_point::calculate_haversine(const Geometry *g,
                                      const double sphere_radius,
                                      int *error)
{
  DBUG_ASSERT(sphere_radius > 0);
  double x1r, x2r, y1r, y2r;

  // This check is done only for optimization purposes where we know it will
  // be one and only one point in Multipoint
  if (g->get_class_info()->m_type_id == Geometry::wkb_multipoint)
  {
    const char point_size= 4 + WKB_HEADER_SIZE + POINT_DATA_SIZE+1; //1 for the type
    char point_temp[point_size];
    memset(point_temp+4, Geometry::wkb_point, 1);
    memcpy(point_temp+5, static_cast<const Gis_multi_point *>(g)->get_data_ptr()+5, 4);
    memcpy(point_temp+4+WKB_HEADER_SIZE, g->get_data_ptr()+4+WKB_HEADER_SIZE,
           POINT_DATA_SIZE);
    point_temp[point_size-1]= '\0';
    Geometry_buffer gbuff;
    Geometry *gg= Geometry::construct(&gbuff, point_temp, point_size-1);
    if (!gg || static_cast<Gis_point *>(gg)->get_xy_radian(&x2r, &y2r))
    {
      *error= 2;
      return -1;
    }
  }
  else
  {
    if (static_cast<const Gis_point *>(g)->get_xy_radian(&x2r, &y2r))
    {
      *error= 2;
      return -1;
    }
  }
  if (this->get_xy_radian(&x1r, &y1r))
  {
    *error= 2;
    return -1;
  }
  //
  // Check boundary conditions: longitude[-180,180]
  if (!((x2r >= -M_PI && x2r <= M_PI) && (x1r >= -M_PI && x1r <= M_PI)))
  {
    *error=1;
    return -1;
  }
  // Check boundary conditions: latitude[-90,90]
  if (!((y2r >= -M_PI/2 && y2r <= M_PI/2) && (y1r >= -M_PI/2 && y1r <= M_PI/2)))
  {
    *error=-1;
    return -1;
  }
  double dlat= sin((y2r - y1r)/2)*sin((y2r - y1r)/2);
  double dlong= sin((x2r - x1r)/2)*sin((x2r - x1r)/2);
  return 2*sphere_radius*asin((sqrt(dlat + cos(y1r)*cos(y2r)*dlong)));
}


/**
  Function that calculate spherical distance of Point from Multipoint geometries.
  In case there is single point in Multipoint geometries calculate_haversine()
  can handle such case. Otherwise, new geometry (Point) has to be constructed.

  @param    g pointer to the Geometry
  @param    r sphere radius
  @param    result pointer to the result
  @param    err    pointer to the error obtained from calculate_haversin()

  @return state
  @retval TRUE  failed
  @retval FALSE success
*/
int Gis_point::spherical_distance_multipoints(Geometry *g, const double r,
                                              double *result, int *err)
{  
  uint32 num_of_points2;
    // To find the minimum radius it cannot be greater than Earth radius
  double res= 6370986.0;
  double temp_res= 0.0;
  const uint32 len= 4 + WKB_HEADER_SIZE + POINT_DATA_SIZE + 1;
  char s[len];
  g->num_geometries(&num_of_points2);
  DBUG_ASSERT(num_of_points2 >= 1);
  if (num_of_points2 == 1)
  {
    *result= this->calculate_haversine(g, r, err);
    return 0;
  }
  for (uint32 i=1; i <= num_of_points2; i++)
  {
    Geometry_buffer buff_temp;
    Geometry *temp;
    const char *pt_ptr= g->get_data_ptr()+
      4+WKB_HEADER_SIZE*i + POINT_DATA_SIZE*(i-1);

    // First 4 bytes are handled already, make sure to create a Point
    memset(s + 4, Geometry::wkb_point, 1);
    if (g->no_data(pt_ptr, POINT_DATA_SIZE))
      return 1;

    memcpy(s + 5, g->get_data_ptr() + 5, 4);
    memcpy(s + 4 + WKB_HEADER_SIZE, pt_ptr, POINT_DATA_SIZE);
    s[len-1]= '\0';
    temp= Geometry::construct(&buff_temp, s, len);
    if (!temp)
      return 1;
    temp_res= this->calculate_haversine(temp, r, err);
    if (res > temp_res)
      res= temp_res;
  }
  *result= res;
  return 0;
}
/***************************** LineString *******************************/

uint32 Gis_line_string::get_data_size() const 
{
  uint32 n_points;
  if (no_data(m_data, 4))
    return GET_SIZE_ERROR;

  n_points= uint4korr(m_data);

  if (not_enough_points(m_data + 4, n_points))
    return GET_SIZE_ERROR;

  return 4 + n_points * POINT_DATA_SIZE;
}


bool Gis_line_string::init_from_wkt(Gis_read_stream *trs, String *wkb)
{
  uint32 n_points= 0;
  uint32 np_pos= wkb->length();
  Gis_point p;

  if (wkb->reserve(4, 512))
    return 1;
  wkb->length(wkb->length()+4);			// Reserve space for points  

  for (;;)
  {
    if (p.init_from_wkt(trs, wkb))
      return 1;
    n_points++;
    if (trs->skip_char(','))			// Didn't find ','
      break;
  }
  if (n_points < 1)
  {
    trs->set_error_msg("Too few points in LINESTRING");
    return 1;
  }
  wkb->write_at_position(np_pos, n_points);
  return 0;
}


uint Gis_line_string::init_from_wkb(const char *wkb, uint len,
                                    wkbByteOrder bo, String *res)
{
  uint32 n_points, proper_length;
  const char *wkb_end;
  Gis_point p;

  if (len < 4 || (n_points= wkb_get_uint(wkb, bo)) < 1 ||
      ((len - 4) / POINT_DATA_SIZE) < n_points)
    return 0;
  proper_length= 4 + n_points * POINT_DATA_SIZE;

  if (len < proper_length || res->reserve(proper_length))
    return 0;

  res->q_append(n_points);
  wkb_end= wkb + proper_length;
  for (wkb+= 4; wkb<wkb_end; wkb+= POINT_DATA_SIZE)
  {
    if (!p.init_from_wkb(wkb, POINT_DATA_SIZE, bo, res))
      return 0;
  }

  return proper_length;
}


bool Gis_line_string::init_from_json(json_engine_t *je, bool er_on_3D,
                                     String *wkb)
{
  uint32 n_points= 0;
  uint32 np_pos= wkb->length();
  Gis_point p;

  if (json_read_value(je))
    return TRUE;

  if (je->value_type != JSON_VALUE_ARRAY)
  {
    je->s.error= GEOJ_INCORRECT_GEOJSON;
    return TRUE;
  }

  if (wkb->reserve(4, 512))
    return TRUE;
  wkb->length(wkb->length()+4);	// Reserve space for n_points  

  while (json_scan_next(je) == 0 && je->state != JST_ARRAY_END)
  {
    DBUG_ASSERT(je->state == JST_VALUE);

    if (p.init_from_json(je, er_on_3D, wkb))
      return TRUE;
    n_points++;
  }
  if (n_points < 1)
  {
    je->s.error= Geometry::GEOJ_TOO_FEW_POINTS;
    return TRUE;
  }
  wkb->write_at_position(np_pos, n_points);
  return FALSE;
}


bool Gis_line_string::get_data_as_wkt(String *txt, const char **end) const
{
  uint32 n_points;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_points= uint4korr(data);
  data += 4;

  if (n_points < 1 ||
      not_enough_points(data, n_points) ||
      txt->reserve(((MAX_DIGITS_IN_DOUBLE + 1)*2 + 1) * n_points))
    return 1;

  while (n_points--)
  {
    double x, y;
    get_point(&x, &y, data);
    data+= POINT_DATA_SIZE;
    txt->qs_append(x);
    txt->qs_append(' ');
    txt->qs_append(y);
    txt->qs_append(',');
  }
  txt->length(txt->length() - 1);		// Remove end ','
  *end= data;
  return 0;
}


bool Gis_line_string::get_data_as_json(String *txt, uint max_dec_digits,
                                       const char **end) const
{
  uint32 n_points;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_points= uint4korr(data);
  data += 4;

  if (n_points < 1 ||
      not_enough_points(data, n_points) ||
      txt->reserve((MAX_DIGITS_IN_DOUBLE*2 + 6) * n_points + 2))
    return 1;

  *end= append_json_points(txt, max_dec_digits, n_points, data, 0);

  return 0;
}


bool Gis_line_string::get_mbr(MBR *mbr, const char **end) const
{
  return (*end=get_mbr_for_points(mbr, m_data, 0)) == 0;
}


int Gis_line_string::geom_length(double *len, const char **end) const
{
  uint32 n_points;
  double prev_x, prev_y;
  const char *data= m_data;

  *len= 0;					// In case of errors
  if (no_data(data, 4))
    return 1;
  n_points= uint4korr(data);
  data+= 4;
  if (n_points < 1 || not_enough_points(data, n_points))
    return 1;

  get_point(&prev_x, &prev_y, data);
  data+= POINT_DATA_SIZE;
  while (--n_points)
  {
    double x, y;
    get_point(&x, &y, data);
    data+= POINT_DATA_SIZE;
    *len+= sqrt(pow(prev_x-x,2)+pow(prev_y-y,2));
    prev_x= x;
    prev_y= y;
  }
  *end= data;
  return 0;
}


int Gis_line_string::area(double *ar, const char **end) const
{
  uint32 n_points;
  *ar= 0.0;

  /* read number of points */
  if (no_data(m_data, 4))
    return 1;
  n_points= uint4korr(m_data);
  *end= m_data + 4 + POINT_DATA_SIZE * n_points;
  return 0;
}


int Gis_line_string::is_closed(int *closed) const
{
  uint32 n_points;
  double x1, y1, x2, y2;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_points= uint4korr(data);
  if (n_points == 1)
  {
    *closed=1;
    return 0;
  }
  data+= 4;
  if (n_points == 0 || not_enough_points(data, n_points))
    return 1;

  /* Get first point */
  get_point(&x1, &y1, data);

  /* get last point */
  data+= POINT_DATA_SIZE + (n_points-2)*POINT_DATA_SIZE;
  get_point(&x2, &y2, data);

  *closed= (x1==x2) && (y1==y2);
  return 0;
}


int Gis_line_string::is_valid(int *valid) const
{
  Geometry_buffer buffer;
  Geometry *geometry;
  uint32 num_points;
  *valid= 0;

  if (no_data(m_data, 4))
    return 1;

  num_points= uint4korr(m_data);
  if (not_enough_points(m_data, num_points))
    return 1;

  double x, y, previous_x, previous_y;
  for (uint32 i = 1; i <= num_points; i++)
  {
    String wkb= 0;

    if (wkb.reserve(SRID_SIZE + BYTE_ORDER_SIZE + WKB_HEADER_SIZE))
      return 1;

    wkb.q_append(SRID_PLACEHOLDER);
    this->point_n(i, &wkb);
    if (!(geometry= Geometry::construct(&buffer, wkb.ptr(), wkb.length()))||
        ((Gis_point *) geometry)->get_xy(&x, &y))
      return 1;

    if ((i != 1) && (x != previous_x || y != previous_y))
    {
      *valid= 1;
      return 0;
    }

    previous_x = x;
    previous_y = y;
  }
  return 0;
}


int Gis_line_string::simplify(String *result, double max_distance) const {
  Geometry_buffer buffer;
  Geometry *geometry= NULL;
  std::vector<st_point_2d> points;
  double x, y;

  uint32 n_points= 0;
  if(this->num_points(&n_points))
    return 1;

  for (uint32 i = 1; i <= n_points; i++)
  {
    String wkb= 0;
    if (wkb.reserve(SRID_SIZE + WKB_HEADER_SIZE + POINT_DATA_SIZE))
      return 1;

    wkb.q_append(SRID_PLACEHOLDER);
    this->point_n(i, &wkb);
    if (!(geometry= Geometry::construct(&buffer, wkb.ptr(), wkb.length())))
      return 1;
    if(((Gis_point *) geometry)->get_xy(&x, &y))
      return 1;

    points.push_back(st_point_2d{x, y});
  }

  simplify_RDP(points, max_distance);

  result->length(0);
  result->reserve(SRID_SIZE + WKB_HEADER_SIZE +
                  (POINT_DATA_SIZE * points.size()));

  result->q_append(SRID_PLACEHOLDER);
  result->q_append((char) wkb_ndr);
  result->q_append((uint32) wkb_linestring);
  result->q_append((uint32) points.size());

  for (auto point : points)
  {
    result->q_append((double) point.x);
    result->q_append((double) point.y);
  }

  return 0;
}


int Gis_line_string::num_points(uint32 *n_points) const
{
  *n_points= uint4korr(m_data);
  return 0;
}


int Gis_line_string::start_point(String *result) const
{
  /* +4 is for skipping over number of points */
  return create_point(result, m_data + 4);
}


int Gis_line_string::end_point(String *result) const
{
  uint32 n_points;
  if (no_data(m_data, 4))
    return 1;
  n_points= uint4korr(m_data);
  if (n_points == 0 || not_enough_points(m_data+4, n_points))
    return 1;
  return create_point(result, m_data + 4 + (n_points - 1) * POINT_DATA_SIZE);
}


int Gis_line_string::point_n(uint32 num, String *result) const
{
  uint32 n_points;
  if (no_data(m_data, 4))
    return 1;
  num--;
  n_points= uint4korr(m_data);
  if (num >= n_points || not_enough_points(m_data+4, n_points))
    return 1;

  return create_point(result, m_data + 4 + num*POINT_DATA_SIZE);
}


int Gis_line_string::store_shapes(Gcalc_shape_transporter *trn) const
{
  uint32 n_points;
  double x, y;
  double UNINIT_VAR(prev_x), UNINIT_VAR(prev_y);
  int first_point= 1;
  const char *data= m_data;

  if (no_data(m_data, 4))
    return 1;
  n_points= uint4korr(data);
  data+= 4;
  if (n_points < 1 || not_enough_points(data, n_points))
    return 1;

  trn->start_line();

  while (n_points--)
  {
    get_point(&x, &y, data);
    data+= POINT_DATA_SIZE;
    if (!first_point && x == prev_x && y == prev_y)
      continue;
    if (trn->add_point(x, y))
      return 1;
    first_point= 0;
    prev_x= x;
    prev_y= y;
  }

  return trn->complete_line();
}


/*
  Calculate the internal area using the shoelace formula
  (https://en.wikipedia.org/wiki/Shoelace_formula). If the area is < 0 then
  it is clockwise. If the area is > 0 it is counterclockwise.
  If it is 0 is degenerate.
*/
int Gis_line_string::is_clockwise(int *result) const
{
  uint32 num_points;
  double area= 0;

  if (this->num_points(&num_points))
    return 1;

  for (uint32 i= 1; i <= num_points; i++)
  {
    Geometry_buffer buffer_first, buffer_second;
    Geometry *point_first, *point_second;
    String wkb_first, wkb_second;

    if (wkb_first.reserve(SRID_SIZE + WKB_HEADER_SIZE) ||
        wkb_second.reserve(SRID_SIZE + WKB_HEADER_SIZE))
      return 1;

    wkb_first.q_append(SRID_PLACEHOLDER);
    wkb_second.q_append(SRID_PLACEHOLDER);

    if (this->point_n(i, &wkb_first) ||
        this->point_n((i == num_points) ? 1 : i + 1, &wkb_second))
      return 1;

    if (!(point_first=
           Geometry::construct(&buffer_first, wkb_first.ptr(),
                                wkb_first.length())) ||
        !(point_second=
           Geometry::construct(&buffer_second, wkb_second.ptr(),
                                wkb_second.length())))
      return 1;

    double x1, x2, y1, y2;
    if (((Gis_point *) point_first)->get_xy(&x1, &y1) ||
        ((Gis_point *) point_second)->get_xy(&x2, &y2))
      return 1;

    area+= (x1 * y2) - (x2 * y1);
  }

  *result= (area < 0);
  return 0;
}


const Geometry::Class_info *Gis_line_string::get_class_info() const
{
  return &linestring_class;
}


/***************************** Polygon *******************************/

uint32 Gis_polygon::get_data_size() const 
{
  uint32 n_linear_rings;
  uint32 n_points;
  const char *data= m_data;

  if (no_data(data, 4))
    return GET_SIZE_ERROR;
  n_linear_rings= uint4korr(data);
  data+= 4;

  while (n_linear_rings--)
  {
    if (no_data(data, 4) ||
        not_enough_points(data+4, n_points= uint4korr(data)))
      return GET_SIZE_ERROR;
    data+= 4 + n_points*POINT_DATA_SIZE;
  }
  if (no_data(data, 0))
    return GET_SIZE_ERROR;
  return (uint32) (data - m_data);
}


bool Gis_polygon::init_from_wkt(Gis_read_stream *trs, String *wkb)
{
  uint32 n_linear_rings= 0;
  uint32 lr_pos= wkb->length();
  int closed;

  if (wkb->reserve(4, 512))
    return 1;
  wkb->length(wkb->length()+4);	// Reserve space for n_rings
  for (;;)  
  {
    Gis_line_string ls;
    uint32 ls_pos=wkb->length();
    if (trs->check_next_symbol('(') ||
	ls.init_from_wkt(trs, wkb) ||
	trs->check_next_symbol(')'))
      return 1;

    ls.set_data_ptr(wkb->ptr() + ls_pos, wkb->length() - ls_pos);
    if (ls.is_closed(&closed) || !closed)
    {
      trs->set_error_msg("POLYGON's linear ring isn't closed");
      return 1;
    }
    n_linear_rings++;
    if (trs->skip_char(','))			// Didn't find ','
      break;
  }
  wkb->write_at_position(lr_pos, n_linear_rings);
  return 0;
}


uint Gis_polygon::init_from_opresult(String *bin,
                                     const char *opres, uint res_len)
{
  const char *opres_orig= opres;
  uint32 position= bin->length();
  uint32 poly_shapes= 0;

  if (bin->reserve(4, 512))
    return 0;
  bin->q_append(poly_shapes);

  while (opres_orig + res_len > opres)
  {
    uint32 n_points, proper_length;
    const char *op_end, *p1_position;
    Gis_point p;
    Gcalc_function::shape_type st;

    st= (Gcalc_function::shape_type) uint4korr(opres);
    if (poly_shapes && st != Gcalc_function::shape_hole)
      break;
    poly_shapes++;
    n_points= uint4korr(opres + 4) + 1; /* skip shape type id */
    proper_length= 4 + n_points * POINT_DATA_SIZE;

    if (bin->reserve(proper_length, 512))
      return 0;

    bin->q_append(n_points);
    op_end= opres + 8 + (n_points-1) * 8 * 2;
    p1_position= (opres+= 8);
    for (; opres<op_end; opres+= POINT_DATA_SIZE)
    {
      if (!p.init_from_wkb(opres, POINT_DATA_SIZE, wkb_ndr, bin))
        return 0;
    }
    if (!p.init_from_wkb(p1_position, POINT_DATA_SIZE, wkb_ndr, bin))
      return 0;
  }

  bin->write_at_position(position, poly_shapes);

  return (uint) (opres - opres_orig);
}


uint Gis_polygon::init_from_wkb(const char *wkb, uint len, wkbByteOrder bo,
                                String *res)
{
  uint32 n_linear_rings;
  const char *wkb_orig= wkb;

  if (len < 4)
    return 0;

  if (!(n_linear_rings= wkb_get_uint(wkb, bo)))
    return 0;

  if (res->reserve(4, 512))
    return 0;
  wkb+= 4;
  len-= 4;
  res->q_append(n_linear_rings);

  while (n_linear_rings--)
  {
    Gis_line_string ls;
    uint32 ls_pos= res->length();
    int ls_len;
    int closed;

    if (!(ls_len= ls.init_from_wkb(wkb, len, bo, res)))
      return 0;

    ls.set_data_ptr(res->ptr() + ls_pos, res->length() - ls_pos);

    if (ls.is_closed(&closed) || !closed)
      return 0;
    wkb+= ls_len;
  }

  return (uint) (wkb - wkb_orig);
}


bool Gis_polygon::init_from_json(json_engine_t *je, bool er_on_3D, String *wkb)
{
  uint32 n_linear_rings= 0;
  uint32 lr_pos= wkb->length();
  int closed;

  if (json_read_value(je))
    return TRUE;

  if (je->value_type != JSON_VALUE_ARRAY)
  {
    je->s.error= GEOJ_INCORRECT_GEOJSON;
    return TRUE;
  }

  if (wkb->reserve(4, 512))
    return TRUE;
  wkb->length(wkb->length()+4);	// Reserve space for n_rings

  while (json_scan_next(je) == 0 && je->state != JST_ARRAY_END)
  {
    Gis_line_string ls;
    DBUG_ASSERT(je->state == JST_VALUE);

    uint32 ls_pos=wkb->length();
    if (ls.init_from_json(je, er_on_3D, wkb))
      return TRUE;
    ls.set_data_ptr(wkb->ptr() + ls_pos, wkb->length() - ls_pos);
    if (ls.is_closed(&closed) || !closed)
    {
      je->s.error= GEOJ_POLYGON_NOT_CLOSED;
      return TRUE;
    }
    n_linear_rings++;
  }

  if (je->s.error)
    return TRUE;

  if (n_linear_rings == 0)
  {
    je->s.error= Geometry::GEOJ_EMPTY_COORDINATES;
    return TRUE;
  }
  wkb->write_at_position(lr_pos, n_linear_rings);
  return FALSE;
}


bool Gis_polygon::get_data_as_wkt(String *txt, const char **end) const
{
  uint32 n_linear_rings;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;

  n_linear_rings= uint4korr(data);
  data+= 4;

  while (n_linear_rings--)
  {
    uint32 n_points;
    if (no_data(data, 4))
      return 1;
    n_points= uint4korr(data);
    data+= 4;
    if (not_enough_points(data, n_points) ||
	txt->reserve(2 + ((MAX_DIGITS_IN_DOUBLE + 1) * 2 + 1) * n_points))
      return 1;
    txt->qs_append('(');
    data= append_points(txt, n_points, data, 0);
    (*txt) [txt->length() - 1]= ')';		// Replace end ','
    txt->qs_append(',');
  }
  txt->length(txt->length() - 1);		// Remove end ','
  *end= data;
  return 0;
}


bool Gis_polygon::get_data_as_json(String *txt, uint max_dec_digits,
                                   const char **end) const
{
  uint32 n_linear_rings;
  const char *data= m_data;

  if (no_data(data, 4) || txt->reserve(1, 512))
    return 1;

  n_linear_rings= uint4korr(data);
  data+= 4;

  txt->qs_append('[');
  while (n_linear_rings--)
  {
    uint32 n_points;
    if (no_data(data, 4))
      return 1;
    n_points= uint4korr(data);
    data+= 4;
    if (not_enough_points(data, n_points) ||
	txt->reserve(4 + (MAX_DIGITS_IN_DOUBLE * 2 + 6) * n_points))
      return 1;
    data= append_json_points(txt, max_dec_digits, n_points, data, 0);
    txt->qs_append(", ", 2);
  }
  txt->length(txt->length() - 2);// Remove ending ', '
  txt->qs_append(']');
  *end= data;
  return 0;
}


bool Gis_polygon::get_mbr(MBR *mbr, const char **end) const
{
  uint32 n_linear_rings;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_linear_rings= uint4korr(data);
  data+= 4;

  while (n_linear_rings--)
  {
    if (!(data= get_mbr_for_points(mbr, data, 0)))
      return 1;
  }
  *end= data;
  return 0;
}

bool Gis_polygon::get_points(Geometry::PointContainer &points) const
{
  uint32 n_linear_rings;
  const char *data= m_data;

  if (no_data(data, 4))
    return true;
  n_linear_rings= uint4korr(data);
  data+= 4;

  while (data && n_linear_rings--)
    data= get_points_common(data, points);
  return !data;
}


class Gcalc_poly_transporter : public Gcalc_shape_transporter
{
protected:
  gcalc_shape_info m_si;
  int m_points_in_ring;
  int m_error;
public:
  Gcalc_poly_transporter(Gcalc_heap *heap) :
    Gcalc_shape_transporter(heap), m_si(0), m_error(0) {}

  int get_error() const { return m_error; }
  int single_point(double x, double y) override { return 0; }
  int start_line() override { return 0; }
  int complete_line() override { return 0; }

  int start_poly() override
  {
    int_start_poly();
    return 0;
  }

  int complete_poly() override
  {
    int_complete_poly();
    return 0;
  }
  int start_ring() override
  {
    int_start_ring();
    m_points_in_ring= m_heap->get_n_points();
    return 0;
  }
  int complete_ring() override
  {
    int_complete_ring();
    m_si++;
    if (m_heap->get_n_points() - m_points_in_ring < 3)
      m_error= 1;
    return 0;
  }
  int add_point(double x, double y) override
  {
    return int_add_point(m_si, x, y);
  }

  int start_collection(int n_objects) override { return 0; }
  int empty_shape() override { return 0; }
};


int Gis_polygon::is_valid(int *valid) const
{
  Gcalc_scan_iterator scan_it;
  Gcalc_heap collector;
  Gcalc_poly_transporter trn(&collector);
  MBR mbr;
  uint32 num_rings;
  const char *c_end;
  char *border_count, *touches_count, *internals;
  int result= 0;

  *valid= 0;

  if (this->num_interior_ring(&num_rings))
    return 1;

  num_rings++;

  if(this->get_mbr(&mbr, &c_end))
    return 1;

  collector.set_extent(mbr.xmin, mbr.xmax, mbr.ymin, mbr.ymax);

  if (this->store_shapes(&trn))
    return 1;

  if (trn.get_error())
    goto exit;

  collector.prepare_operation();
  scan_it.init(&collector);

  border_count= (char *) my_alloca(num_rings);
  bzero(border_count, num_rings);
  touches_count= (char *) my_alloca(num_rings);
  internals= (char *) my_alloca(num_rings);

  while (scan_it.more_points())
  {
    const Gcalc_scan_iterator::event_point *events;

    if (scan_it.step())
    {
      result= 1;
      goto exit;
    }

    events= scan_it.get_events();

    Gcalc_point_iterator pit(&scan_it);
    int outer_border= 0;

    bzero(internals, num_rings);
    /* Walk to the event, marking polygons we met */
    for (; pit.point() != scan_it.get_event_position(); ++pit)
    {
      gcalc_shape_info si= pit.point()->get_shape();
      internals[si]^= 1;
      if (si != 0) /* interior ring */
      {
        if (!internals[0])
        {
          /* Internal ring outside the outer. */
          goto exit;
        }
        for (uint n=1; n<num_rings; n++)
        {
          if (n != si && internals[n]) /* Internal ring inside another internal */
            goto exit;
        }
      }
    }

    if (events->simple_event())
      continue;

    bzero(touches_count, num_rings);

    /* Check the status of the event point */
    for (; events; events= events->get_next())
    {
      gcalc_shape_info si= events->get_shape();
      if (events->event == scev_thread ||
          events->event == scev_end || /* should never happen. */
          events->event == scev_single_point ||
          events->event == scev_intersection)
      {
        /* These types of events never happen in valid polygon. */
        goto exit;
      }

      touches_count[si]++;
      if (events->event == scev_two_threads || events->event == scev_two_ends)
      {
        if (touches_count[si] > 2)
          goto exit;
      }
      else
      {
        if (touches_count[si] > 1)
          goto exit;
      }

      if (si == 0) /* outer ring */
        outer_border= 1;
      else
      {
        if (!outer_border && !internals[0])
        {
          /* Inner ring outside the outer ring. */
          goto exit;
        }
        if (outer_border)
        {
          if (border_count[si]++ > 1)
          {
            /*
              We can't have more than one point of the
              internal ring on the border of the outer ring.
            */
            goto exit;
          }
        }
      }
    }
  }

  *valid= 1;

exit:
  collector.reset();
  scan_it.reset();
  my_afree(border_count);
  my_afree(touches_count);
  my_afree(internals);

  return result;

}


int Gis_polygon::area(double *ar, const char **end_of_data) const
{
  uint32 n_linear_rings;
  double result= -1.0;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_linear_rings= uint4korr(data);
  data+= 4;

  while (n_linear_rings--)
  {
    double prev_x, prev_y;
    double lr_area= 0;
    uint32 n_points;

    if (no_data(data, 4))
      return 1;
    n_points= uint4korr(data);
    if (n_points == 0 ||
        not_enough_points(data, n_points))
      return 1;
    get_point(&prev_x, &prev_y, data+4);
    data+= (4+POINT_DATA_SIZE);

    while (--n_points)				// One point is already read
    {
      double x, y;
      get_point(&x, &y, data);
      data+= POINT_DATA_SIZE;
      lr_area+= (prev_x + x)* (prev_y - y);
      prev_x= x;
      prev_y= y;
    }
    lr_area= fabs(lr_area)/2;
    if (result == -1.0)
      result= lr_area;
    else
      result-= lr_area;
  }
  *ar= fabs(result);
  *end_of_data= data;
  return 0;
}

int Gis_polygon::simplify(String *result, double max_distance) const
{
  uint32 num_interior_ring= 0, num_invalid_ring= 0, num_points;
  Geometry_buffer buffer;
  Geometry *geometry= NULL;
  String exterior_ring= 0;

  exterior_ring.q_append(SRID_PLACEHOLDER);
  if (this->num_interior_ring(&num_interior_ring) ||
      this->exterior_ring(&exterior_ring) ||
      !(geometry= Geometry::construct(&buffer, exterior_ring.ptr(),
      exterior_ring.length())))
    return 1;

  if (geometry->simplify(&exterior_ring, max_distance))
    return 1;

  if (!(geometry= Geometry::construct(&buffer, exterior_ring.ptr(),
                          exterior_ring.length())))
    return 1;

  if (geometry->num_points(&num_points) || num_points <= 3)
    return 1;

  result->length(0);
  result->reserve(SRID_SIZE + WKB_HEADER_SIZE);
  result->q_append(SRID_PLACEHOLDER);
  result->q_append((char) wkb_ndr);
  result->q_append((uint32) wkb_polygon);
  result->q_append((uint32) 1 + num_interior_ring);
  result->append(exterior_ring.ptr() + SRID_SIZE + WKB_HEADER_SIZE,
                 (exterior_ring.length() - SRID_SIZE -
                  WKB_HEADER_SIZE));

  for (uint32 i = 1; i <= num_interior_ring; i++)
  {
    String interior_ring= 0;
    interior_ring.q_append((uint) 0);
    if (this->interior_ring_n(i, &interior_ring) ||
        !(geometry= Geometry::construct(&buffer, interior_ring.ptr(),
          interior_ring.length())))
    {
      num_invalid_ring++;
      continue;
    }

    if(geometry->simplify(&interior_ring, max_distance))
      return 1;

    if (!(geometry= Geometry::construct(&buffer, interior_ring.ptr(),
                          interior_ring.length())))
    {
      num_invalid_ring++;
      continue;
    }

    if (geometry->num_points(&num_points))
      return 1;

    if (num_points <= 3)
    {
      num_invalid_ring++;
      continue;
    }

    result->append(interior_ring.ptr() + SRID_SIZE + WKB_HEADER_SIZE,
                   (interior_ring.length() - SRID_SIZE - WKB_HEADER_SIZE));
  }

  result->write_at_position(SRID_SIZE + WKB_HEADER_SIZE,
                            ((uint32) 1 + num_interior_ring -
                             num_invalid_ring));
  return 0;
}

int Gis_polygon::exterior_ring(String *result) const
{
  uint32 n_points, length;
  const char *data= m_data + 4; // skip n_linerings

  if (no_data(data, 4))
    return 1;
  n_points= uint4korr(data);
  data+= 4;
  length= n_points * POINT_DATA_SIZE;
  if (not_enough_points(data, n_points) || result->reserve(1+4+4+ length))
    return 1;

  result->q_append((char) wkb_ndr);
  result->q_append((uint32) wkb_linestring);
  result->q_append(n_points);
  result->q_append(data, n_points * POINT_DATA_SIZE);
  return 0;
}


int Gis_polygon::num_interior_ring(uint32 *n_int_rings) const
{
  if (no_data(m_data, 4))
    return 1;
  *n_int_rings= uint4korr(m_data)-1;
  return 0;
}


int Gis_polygon::interior_ring_n(uint32 num, String *result) const
{
  const char *data= m_data;
  uint32 n_linear_rings;
  uint32 n_points;
  uint32 points_size;

  if (no_data(data, 4))
    return 1;
  n_linear_rings= uint4korr(data);
  data+= 4;

  if (num >= n_linear_rings || num < 1)
    return 1;

  while (num--)
  {
    if (no_data(data, 4))
      return 1;
    data+= 4 + uint4korr(data) * POINT_DATA_SIZE;
  }
  if (no_data(data, 4))
    return 1;
  n_points= uint4korr(data);
  points_size= n_points * POINT_DATA_SIZE;
  data+= 4;
  if (not_enough_points(data, n_points) || result->reserve(1+4+4+ points_size))
    return 1;

  result->q_append((char) wkb_ndr);
  result->q_append((uint32) wkb_linestring);
  result->q_append(n_points);
  result->q_append(data, points_size); 

  return 0;
}


int Gis_polygon::centroid_xy(double *x, double *y) const
{
  uint32 n_linear_rings;
  double UNINIT_VAR(res_area);
  double UNINIT_VAR(res_cx), UNINIT_VAR(res_cy);
  const char *data= m_data;
  bool first_loop= 1;

  if (no_data(data, 4) ||
      (n_linear_rings= uint4korr(data)) == 0)
    return 1;
  data+= 4;

  while (n_linear_rings--)
  {
    uint32 n_points, org_n_points;
    double prev_x, prev_y;
    double cur_area= 0;
    double cur_cx= 0, cur_cy= 0;
    double sum_cx= 0, sum_cy= 0;

    if (no_data(data, 4))
      return 1;
    org_n_points= n_points= uint4korr(data);
    data+= 4;
    if (n_points == 0 || not_enough_points(data, n_points))
      return 1;
    get_point(&prev_x, &prev_y, data);
    data+= POINT_DATA_SIZE;

    while (--n_points)				// One point is already read
    {
      double tmp_x, tmp_y;
      double loc_area;
      get_point(&tmp_x, &tmp_y, data);
      data+= POINT_DATA_SIZE;
      loc_area= prev_x * tmp_y - tmp_x * prev_y;
      cur_area+= loc_area;
      cur_cx+= tmp_x;
      cur_cy+= tmp_y;
      sum_cx+= (prev_x + tmp_x) * loc_area;
      sum_cy+= (prev_y + tmp_y) * loc_area;

      prev_x= tmp_x;
      prev_y= tmp_y;
    }

    if (fabs(cur_area) > 1e-10)
    {
      cur_cx= sum_cx / cur_area / 3.0;
      cur_cy= sum_cy / cur_area / 3.0;
    }
    else
    {
      cur_cx= cur_cx / (org_n_points - 1);
      cur_cy= cur_cy / (org_n_points - 1);
    }

    cur_area= fabs(cur_area);

    if (!first_loop)
    {
      double d_area= fabs(res_area - cur_area);
      res_cx= (res_area * res_cx - cur_area * cur_cx) / d_area;
      res_cy= (res_area * res_cy - cur_area * cur_cy) / d_area;
    }
    else
    {
      first_loop= 0;
      res_area= cur_area;
      res_cx= cur_cx;
      res_cy= cur_cy;
    }
  }

  *x= res_cx;
  *y= res_cy;
  return 0;
}


int Gis_polygon::centroid(String *result) const
{
  double x, y;
  if (centroid_xy(&x, &y))
    return 1;
  return create_point(result, x, y);
}


int Gis_polygon::store_shapes(Gcalc_shape_transporter *trn) const
{
  uint32 n_linear_rings;
  const char *data= m_data;
  double first_x, first_y;
  double prev_x, prev_y;
  int was_equal_first= 0;

  if (trn->start_poly())
    return 1;

  if (no_data(data, 4))
    return 1;
  n_linear_rings= uint4korr(data);
  data+= 4;

  while (n_linear_rings--)
  {
    uint32 n_points;

    if (no_data(data, 4))
      return 1;
    n_points= uint4korr(data);
    data+= 4;
    if (!n_points || not_enough_points(data, n_points))
      return 1;

    trn->start_ring();
    get_point(&first_x, &first_y, data);
    data+= POINT_DATA_SIZE;

    prev_x= first_x;
    prev_y= first_y;
    if (trn->add_point(first_x, first_y))
      return 1;

    if (--n_points == 0)
      goto single_point_ring;

    while (--n_points)
    {
      double x, y;
      get_point(&x, &y, data);
      data+= POINT_DATA_SIZE;
      if (x == prev_x && y == prev_y)
        continue;
      prev_x= x;
      prev_y= y;
      if (was_equal_first)
      {
        if (trn->add_point(first_x, first_y))
          return 1;
        was_equal_first= 0;
      }
      if (x == first_x && y == first_y)
      {
        was_equal_first= 1;
        continue;
      }
      if (trn->add_point(x, y))
        return 1;
    }
    data+= POINT_DATA_SIZE;

single_point_ring:
    trn->complete_ring();
  }

  trn->complete_poly();
  return 0;
}


int Gis_polygon::make_clockwise(String *result) const
{
  String ring_wkb= 0;
  uint32 num_interior_ring;
  Geometry *ring;
  Geometry_buffer buffer;
  int is_clockwise;
  uint32 ring_points;

  if(ring_wkb.reserve(SRID_SIZE + WKB_HEADER_SIZE) ||
     result->reserve(SRID_SIZE + WKB_HEADER_SIZE))
    return 1;

  if (this->num_interior_ring(&num_interior_ring) ||
      this->exterior_ring(&ring_wkb))
    return 1;

  result->length(0);
  result->append((char) wkb_ndr);
  result->q_append((uint32) wkb_polygon);
  result->q_append((uint32) num_interior_ring + 1);
  result->append(ring_wkb.ptr() + WKB_HEADER_SIZE,
                 ring_wkb.length() - WKB_HEADER_SIZE);

  for(uint32 i= 1; i <= num_interior_ring; i++)
  {
    ring_wkb.length(0);
    ring_wkb.q_append(SRID_PLACEHOLDER);
    if (this->interior_ring_n(i, &ring_wkb))
      return 1;

    if (!(ring= Geometry::construct(&buffer, ring_wkb.ptr(),
                                    ring_wkb.length())))
      return 1;

    if (ring->is_clockwise(&is_clockwise))
      return 1;

    if (is_clockwise)
    {
      result->append(ring_wkb.ptr() + WKB_HEADER_SIZE + SRID_SIZE,
                     ring_wkb.length() - (WKB_HEADER_SIZE + SRID_SIZE));
      continue;
    }

    if (ring->num_points(&ring_points))
      return 1;
    result->q_append((uint32) ring_points);

    for (uint32 i= ring_points; i > 0; i--)
    {
      String point= 0;
      ring->point_n(i, &point);
      result->append(point.ptr() + WKB_HEADER_SIZE,
                     point.length() - WKB_HEADER_SIZE);
    }
  }

  return 0;
}


const Geometry::Class_info *Gis_polygon::get_class_info() const
{
  return &polygon_class;
}


/***************************** MultiPoint *******************************/

uint32 Gis_multi_point::get_data_size() const 
{
  uint32 n_points;

  if (no_data(m_data, 4) ||
      not_enough_points(m_data+4, (n_points= uint4korr(m_data)),
        WKB_HEADER_SIZE))
     return GET_SIZE_ERROR;
  return  4 + n_points * (POINT_DATA_SIZE + WKB_HEADER_SIZE);
}


bool Gis_multi_point::init_from_wkt(Gis_read_stream *trs, String *wkb)
{
  uint32 n_points= 0;
  uint32 np_pos= wkb->length();
  Gis_point p;

  if (wkb->reserve(4, 512))
    return 1;
  wkb->length(wkb->length()+4);			// Reserve space for points

  for (;;)
  {
    if (wkb->reserve(1 + 4, 512))
      return 1;
    wkb->q_append((char) wkb_ndr);
    wkb->q_append((uint32) wkb_point);
    if (p.init_from_wkt(trs, wkb))
      return 1;
    n_points++;
    if (trs->skip_char(','))			// Didn't find ','
      break;
  }
  wkb->write_at_position(np_pos, n_points);	// Store number of found points
  return 0;
}


uint Gis_multi_point::init_from_opresult(String *bin,
                                         const char *opres, uint res_len)
{
  uint bin_size, n_points;
  Gis_point p;
  const char *opres_end;

  n_points= res_len/(4+8*2);
  bin_size= n_points * (WKB_HEADER_SIZE + POINT_DATA_SIZE) + 4;
 
  if (bin->reserve(bin_size, 512))
    return 0;
    
  bin->q_append(n_points);
  opres_end= opres + res_len;
  for (; opres < opres_end; opres+= (4 + 8*2))
  {
    bin->q_append((char)wkb_ndr);
    bin->q_append((uint32)wkb_point);
    if (!p.init_from_wkb(opres + 4, POINT_DATA_SIZE, wkb_ndr, bin))
      return 0;
  }
  return res_len;
}


uint Gis_multi_point::init_from_wkb(const char *wkb, uint len, wkbByteOrder bo,
                                    String *res)
{
  uint32 n_points;
  uint proper_size;
  Gis_point p;
  const char *wkb_end;

  if (len < 4 ||
      (n_points= wkb_get_uint(wkb, bo)) > max_n_points)
    return 0;
  proper_size= 4 + n_points * (WKB_HEADER_SIZE + POINT_DATA_SIZE);
 
  if (len < proper_size || res->reserve(proper_size))
    return 0;
    
  res->q_append(n_points);
  wkb_end= wkb + proper_size;
  for (wkb+=4; wkb < wkb_end; wkb+= (WKB_HEADER_SIZE + POINT_DATA_SIZE))
  {
    res->q_append((char)wkb_ndr);
    res->q_append((uint32)wkb_point);
    if (!p.init_from_wkb(wkb + WKB_HEADER_SIZE,
                         POINT_DATA_SIZE, (wkbByteOrder) wkb[0], res))
      return 0;
  }
  return proper_size;
}


bool Gis_multi_point::init_from_json(json_engine_t *je, bool er_on_3D,
                                     String *wkb)
{
  uint32 n_points= 0;
  uint32 np_pos= wkb->length();
  Gis_point p;

  if (json_read_value(je))
    return TRUE;

  if (je->value_type != JSON_VALUE_ARRAY)
  {
    je->s.error= GEOJ_INCORRECT_GEOJSON;
    return TRUE;
  }

  if (wkb->reserve(4, 512))
    return TRUE;
  wkb->length(wkb->length()+4);	// Reserve space for n_points  

  while (json_scan_next(je) == 0 && je->state != JST_ARRAY_END)
  {
    DBUG_ASSERT(je->state == JST_VALUE);

    if (wkb->reserve(1 + 4, 512))
      return TRUE;
    wkb->q_append((char) wkb_ndr);
    wkb->q_append((uint32) wkb_point);

    if (p.init_from_json(je, er_on_3D, wkb))
      return TRUE;
    n_points++;
  }

  if (je->s.error)
    return TRUE;

  if (n_points == 0)
  {
    je->s.error= Geometry::GEOJ_EMPTY_COORDINATES;
    return TRUE;
  }

  wkb->write_at_position(np_pos, n_points);
  return FALSE;
}


bool Gis_multi_point::get_data_as_wkt(String *txt, const char **end) const
{
  uint32 n_points;
  if (no_data(m_data, 4))
    return 1;

  n_points= uint4korr(m_data);
  if (n_points > max_n_points ||
      not_enough_points(m_data+4, n_points, WKB_HEADER_SIZE) ||
      txt->reserve(((MAX_DIGITS_IN_DOUBLE + 1) * 2 + 1) * n_points))
    return 1;
  *end= append_points(txt, n_points, m_data+4, WKB_HEADER_SIZE);
  txt->length(txt->length()-1);			// Remove end ','
  return 0;
}


bool Gis_multi_point::get_data_as_json(String *txt, uint max_dec_digits,
                                       const char **end) const
{
  uint32 n_points;
  if (no_data(m_data, 4))
    return 1;

  n_points= uint4korr(m_data);
  if (n_points > max_n_points ||
      not_enough_points(m_data+4, n_points, WKB_HEADER_SIZE) ||
      txt->reserve((MAX_DIGITS_IN_DOUBLE * 2 + 6) * n_points + 2))
    return 1;
  *end= append_json_points(txt, max_dec_digits, n_points, m_data+4,
                           WKB_HEADER_SIZE);
  return 0;
}


int Gis_multi_point::is_valid(int *valid) const
{
  uint32 num_points;
  if (no_data(m_data, 4))
    return 1;

  num_points= uint4korr(m_data);
  if (not_enough_points(m_data, num_points))
    return 1;

  *valid= 1;
  return 0;
}


bool Gis_multi_point::get_mbr(MBR *mbr, const char **end) const
{
  return (*end= get_mbr_for_points(mbr, m_data, WKB_HEADER_SIZE)) == 0;
}


int Gis_multi_point::num_geometries(uint32 *num) const
{
  *num= uint4korr(m_data);
  return 0;
}


int Gis_multi_point::geometry_n(uint32 num, String *result) const
{
  const char *data= m_data;
  uint32 n_points;

  if (no_data(data, 4))
    return 1;
  n_points= uint4korr(data);
  data+= 4+ (num - 1) * (WKB_HEADER_SIZE + POINT_DATA_SIZE);

  if (num > n_points || num < 1 ||
      no_data(data, WKB_HEADER_SIZE + POINT_DATA_SIZE) ||
      result->reserve(WKB_HEADER_SIZE + POINT_DATA_SIZE))
    return 1;

  result->q_append(data, WKB_HEADER_SIZE + POINT_DATA_SIZE);
  return 0;
}


int Gis_multi_point::store_shapes(Gcalc_shape_transporter *trn) const
{
  uint32 n_points;
  Gis_point pt;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_points= uint4korr(data);
  data+= 4;

  if (trn->start_collection(n_points))
    return 1;

  while (n_points--)
  {
    if (no_data(data, WKB_HEADER_SIZE))
      return 1;
    data+= WKB_HEADER_SIZE;
    pt.set_data_ptr(data, (uint32) (m_data_end - data));
    if (pt.store_shapes(trn))
      return 1;
    data+= pt.get_data_size();
  }
  return 0;
}


const Geometry::Class_info *Gis_multi_point::get_class_info() const
{
  return &multipoint_class;
}


/**
  Function that calculate spherical distance of Multipoints geometries.
  In case there is single point in Multipoint geometries calculate_haversine()
  can handle such case. Otherwise, new geometry (Point) has to be constructed.

  @param    g pointer to the Geometry
  @param    r sphere radius
  @param    result pointer to the result
  @param    err    pointer to the error obtained from calculate_haversin()

  @return state
  @retval TRUE  failed
  @retval FALSE success
*/
int Gis_multi_point::spherical_distance_multipoints(Geometry *g, const double r,
                                                    double *result, int *err)
{
  const uint32 len= 4 + WKB_HEADER_SIZE + POINT_DATA_SIZE + 1;
  // Check how many points are stored in Multipoints
  uint32 num_of_points1, num_of_points2;
  // To find the minimum radius it cannot be greater than Earth radius
  double res= 6370986.0;

  /* From Item_func_sphere_distance::spherical_distance_points,
     we are sure that there will be multiple points and we have to construct
     Point geometry and return the smallest result.
  */
  num_geometries(&num_of_points1);
  DBUG_ASSERT(num_of_points1 >= 1);
  g->num_geometries(&num_of_points2);
  DBUG_ASSERT(num_of_points2 >= 1);

  for (uint32 i=1; i <= num_of_points1; i++)
  {
    Geometry_buffer buff_temp;
    Geometry *temp;
    double temp_res= 0.0;
    char s[len];
    const char *pt_ptr= get_data_ptr()+
      4+WKB_HEADER_SIZE*i + POINT_DATA_SIZE*(i-1);
    // First 4 bytes are handled already, make sure to create a Point
    memset(s + 4, Geometry::wkb_point, 1);
    if (no_data(pt_ptr, POINT_DATA_SIZE))
      return 1;
    memcpy(s + 5, this->get_data_ptr() + 5, 4);
    memcpy(s + 4 + WKB_HEADER_SIZE, pt_ptr, POINT_DATA_SIZE);
    s[len-1]= '\0';
    temp= Geometry::construct(&buff_temp, s, len);
    if (!temp)
      return 1;
    // Optimization for single Multipoint
    if (num_of_points2 == 1)
    {
      *result= static_cast<Gis_point *>(temp)->calculate_haversine(g, r, err);
      return 0;
    }
    for (uint32 j=1; j<= num_of_points2; j++)
    {
      Geometry_buffer buff_temp2;
      Geometry *temp2;
      char s2[len];
      const char *pt_ptr= g->get_data_ptr()+
        4+WKB_HEADER_SIZE*j + POINT_DATA_SIZE*(j-1);
      // First 4 bytes are handled already, make sure to create a Point
      memset(s2 + 4, Geometry::wkb_point, 1);
      if (g->no_data(pt_ptr, POINT_DATA_SIZE))
        return 1;
      memcpy(s2 + 5, g->get_data_ptr() + 5, 4);
      memcpy(s2 + 4 + WKB_HEADER_SIZE, pt_ptr, POINT_DATA_SIZE);
      s2[len-1]= '\0';
      temp2= Geometry::construct(&buff_temp2, s2, len);
      if (!temp2)
        return 1;
      temp_res= static_cast<Gis_point *>(temp)->calculate_haversine(temp2, r, err);
      if (res > temp_res)
        res= temp_res;
    }
  }
  *result= res;
  return 0;
}


/***************************** MultiLineString *******************************/

uint32 Gis_multi_line_string::get_data_size() const 
{
  uint32 n_line_strings;
  uint32 n_points;
  const char *data= m_data;

  if (no_data(data, 4))
    return GET_SIZE_ERROR;
  n_line_strings= uint4korr(data);
  data+= 4;

  while (n_line_strings--)
  {
    if (no_data(data, WKB_HEADER_SIZE + 4) ||
        not_enough_points(data + WKB_HEADER_SIZE+4,
                          (n_points= uint4korr(data + WKB_HEADER_SIZE))))
      return GET_SIZE_ERROR;
    data+= (WKB_HEADER_SIZE + 4 + n_points*POINT_DATA_SIZE);
  }
  if (no_data(data, 0))
    return GET_SIZE_ERROR;
  return (uint32) (data - m_data);
}


bool Gis_multi_line_string::init_from_wkt(Gis_read_stream *trs, String *wkb)
{
  uint32 n_line_strings= 0;
  uint32 ls_pos= wkb->length();

  if (wkb->reserve(4, 512))
    return 1;
  wkb->length(wkb->length()+4);			// Reserve space for points

  for (;;)
  {
    Gis_line_string ls;

    if (wkb->reserve(1 + 4, 512))
      return 1;
    wkb->q_append((char) wkb_ndr);
    wkb->q_append((uint32) wkb_linestring);

    if (trs->check_next_symbol('(') ||
	      ls.init_from_wkt(trs, wkb) ||
	      trs->check_next_symbol(')'))
      return 1;
    n_line_strings++;
    if (trs->skip_char(','))			// Didn't find ','
      break;
  }
  wkb->write_at_position(ls_pos, n_line_strings);
  return 0;
}


uint Gis_multi_line_string::init_from_opresult(String *bin,
                                               const char *opres, uint res_len)
{
  const char *opres_orig= opres;
  int ns_pos= bin->length();
  uint n_linestring= 0;

  if (bin->reserve(4, 512))
    return 0;
  bin->q_append(n_linestring);
  
  while (res_len)
  {
    Gis_line_string ls;
    int ls_len;

    if (bin->reserve(WKB_HEADER_SIZE, 512))
      return 0;

    bin->q_append((char) wkb_ndr);
    bin->q_append((uint32) wkb_linestring);

    if (!(ls_len= ls.init_from_opresult(bin, opres, res_len)))
      return 0;
    opres+= ls_len;
    res_len-= ls_len;
    n_linestring++;
  }
  bin->write_at_position(ns_pos, n_linestring);
  return (uint) (opres - opres_orig);
}


uint Gis_multi_line_string::init_from_wkb(const char *wkb, uint len,
                                          wkbByteOrder bo, String *res)
{
  uint32 n_line_strings;
  const char *wkb_orig= wkb;

  if (len < 4 ||
      (n_line_strings= wkb_get_uint(wkb, bo))< 1)
    return 0;

  if (res->reserve(4, 512))
    return 0;
  res->q_append(n_line_strings);
  
  wkb+= 4;
  while (n_line_strings--)
  {
    Gis_line_string ls;
    int ls_len;

    if ((len < WKB_HEADER_SIZE) ||
        res->reserve(WKB_HEADER_SIZE, 512))
      return 0;

    res->q_append((char) wkb_ndr);
    res->q_append((uint32) wkb_linestring);

    if (!(ls_len= ls.init_from_wkb(wkb + WKB_HEADER_SIZE, len,
                                   (wkbByteOrder) wkb[0], res)))
      return 0;
    ls_len+= WKB_HEADER_SIZE;
    wkb+= ls_len;
    len-= ls_len;
  }
  return (uint) (wkb - wkb_orig);
}


bool Gis_multi_line_string::init_from_json(json_engine_t *je, bool er_on_3D,
                                           String *wkb)
{
  uint32 n_line_strings= 0;
  uint32 ls_pos= wkb->length();

  if (json_read_value(je))
    return TRUE;

  if (je->value_type != JSON_VALUE_ARRAY)
  {
    je->s.error= GEOJ_INCORRECT_GEOJSON;
    return TRUE;
  }

  if (wkb->reserve(4, 512))
    return TRUE;
  wkb->length(wkb->length()+4);	// Reserve space for n_rings

  while (json_scan_next(je) == 0 && je->state != JST_ARRAY_END)
  {
    Gis_line_string ls;
    DBUG_ASSERT(je->state == JST_VALUE);

    if (wkb->reserve(1 + 4, 512))
      return TRUE;
    wkb->q_append((char) wkb_ndr);
    wkb->q_append((uint32) wkb_linestring);

    if (ls.init_from_json(je, er_on_3D, wkb))
      return TRUE;

    n_line_strings++;
  }

  if (je->s.error)
    return TRUE;

  if (n_line_strings == 0)
  {
    je->s.error= Geometry::GEOJ_EMPTY_COORDINATES;
    return TRUE;
  }

  wkb->write_at_position(ls_pos, n_line_strings);
  return FALSE;
}


bool Gis_multi_line_string::get_data_as_wkt(String *txt, 
					     const char **end) const
{
  uint32 n_line_strings;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_line_strings= uint4korr(data);
  data+= 4;

  while (n_line_strings--)
  {
    uint32 n_points;
    if (no_data(data, (WKB_HEADER_SIZE + 4)))
      return 1;
    n_points= uint4korr(data + WKB_HEADER_SIZE);
    data+= WKB_HEADER_SIZE + 4;
    if (not_enough_points(data, n_points) ||
	txt->reserve(2 + ((MAX_DIGITS_IN_DOUBLE + 1) * 2 + 1) * n_points))
      return 1;
    txt->qs_append('(');
    data= append_points(txt, n_points, data, 0);
    (*txt) [txt->length() - 1]= ')';
    txt->qs_append(',');
  }
  txt->length(txt->length() - 1);
  *end= data;
  return 0;
}


bool Gis_multi_line_string::get_data_as_json(String *txt, uint max_dec_digits,
                                             const char **end) const
{
  uint32 n_line_strings;
  const char *data= m_data;

  if (no_data(data, 4) || txt->reserve(1, 512))
    return 1;
  n_line_strings= uint4korr(data);
  data+= 4;

  txt->qs_append('[');
  while (n_line_strings--)
  {
    uint32 n_points;
    if (no_data(data, (WKB_HEADER_SIZE + 4)))
      return 1;
    n_points= uint4korr(data + WKB_HEADER_SIZE);
    data+= WKB_HEADER_SIZE + 4;
    if (not_enough_points(data, n_points) ||
	txt->reserve(2 + (MAX_DIGITS_IN_DOUBLE * 2 + 6) * n_points))
      return 1;
    data= append_json_points(txt, max_dec_digits, n_points, data, 0);
    txt->qs_append(", ", 2);
  }
  txt->length(txt->length() - 2);
  txt->qs_append(']');
  *end= data;
  return 0;
}


int Gis_multi_line_string::is_valid(int *valid) const
{
  uint32 num_linestring;
  Geometry_buffer buffer;
  Geometry *geometry= NULL;
  *valid= 0;

  if (no_data(m_data, 4))
    return 1;
  num_linestring= uint4korr(m_data);

  for (uint32 i = 1; i <= num_linestring; i++)
  {
    String wkb = 0;

    wkb.q_append(SRID_PLACEHOLDER);
    if (this->geometry_n(i, &wkb) ||
        !(geometry= Geometry::construct(&buffer, wkb.ptr(), wkb.length())))
      return 1;

    int line_valid;
    if(geometry->is_valid(&line_valid))
      return 1;

    if (!line_valid)
      return 0;
  }

  *valid= 1;
  return 0;
}


bool Gis_multi_line_string::get_mbr(MBR *mbr, const char **end) const
{
  uint32 n_line_strings;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_line_strings= uint4korr(data);
  data+= 4;

  while (n_line_strings--)
  {
    data+= WKB_HEADER_SIZE;
    if (!(data= get_mbr_for_points(mbr, data, 0)))
      return 1;
  }
  *end= data;
  return 0;
}


int Gis_multi_line_string::num_geometries(uint32 *num) const
{
  *num= uint4korr(m_data);
  return 0;
}


int Gis_multi_line_string::geometry_n(uint32 num, String *result) const
{
  uint32 n_line_strings, n_points, length;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_line_strings= uint4korr(data);
  data+= 4;

  if ((num > n_line_strings) || (num < 1))
    return 1;
 
  for (;;)
  {
    if (no_data(data, WKB_HEADER_SIZE + 4))
      return 1;
    n_points= uint4korr(data + WKB_HEADER_SIZE);
    length= WKB_HEADER_SIZE + 4+ POINT_DATA_SIZE * n_points;
    if (not_enough_points(data+WKB_HEADER_SIZE+4, n_points))
      return 1;
    if (!--num)
      break;
    data+= length;
  }
  return result->append(data, length, (uint32) 0);
}


int Gis_multi_line_string::geom_length(double *len, const char **end) const
{
  uint32 n_line_strings;
  const char *data= m_data;
  const char *line_end;

  if (no_data(data, 4))
    return 1;
  n_line_strings= uint4korr(data);
  data+= 4;

  *len=0;
  while (n_line_strings--)
  {
    double ls_len;
    Gis_line_string ls;
    data+= WKB_HEADER_SIZE;
    ls.set_data_ptr(data, (uint32) (m_data_end - data));
    if (ls.geom_length(&ls_len, &line_end))
      return 1;
    *len+= ls_len;
    /*
      We know here that ls was ok, so we can call the trivial function
      Gis_line_string::get_data_size without error checking
    */
    data+= ls.get_data_size();
  }
  *end= data;
  return 0;
}


int Gis_multi_line_string::is_closed(int *closed) const
{
  uint32 n_line_strings;
  const char *data= m_data;

  if (no_data(data, 4 + WKB_HEADER_SIZE))
    return 1;
  n_line_strings= uint4korr(data);
  data+= 4 + WKB_HEADER_SIZE;

  while (n_line_strings--)
  {
    Gis_line_string ls;
    if (no_data(data, 0))
      return 1;
    ls.set_data_ptr(data, (uint32) (m_data_end - data));
    if (ls.is_closed(closed))
      return 1;
    if (!*closed)
      return 0;
    /*
      We know here that ls was ok, so we can call the trivial function
      Gis_line_string::get_data_size without error checking
    */
    data+= ls.get_data_size() + WKB_HEADER_SIZE;
  }
  return 0;
}


int Gis_multi_line_string::simplify(String *result, double max_distance) const
{
  uint32 num_lines= 0;
  Geometry_buffer buffer;
  Geometry *geometry= NULL;

  if (this->num_geometries(&num_lines))
    return 1;

  result->length(0);
  result->reserve(SRID_SIZE + WKB_HEADER_SIZE);
  result->q_append(SRID_PLACEHOLDER);
  result->q_append((char) wkb_ndr);
  result->q_append((uint32) wkb_multilinestring);
  result->q_append((uint32) num_lines);

  for (uint32 i = 1; i <= num_lines; i++)
  {
    String wkb= 0;
    wkb.q_append((uint) 0);
    this->geometry_n(i, &wkb);
    if (!(geometry= Geometry::construct(&buffer, wkb.ptr(), wkb.length())))
      return 1;
    geometry->simplify(&wkb, max_distance);
    result->append(wkb.ptr() + SRID_SIZE, wkb.length() - SRID_SIZE);
  }

  return 0;
}


int Gis_multi_line_string::store_shapes(Gcalc_shape_transporter *trn) const
{
  uint32 n_lines;
  Gis_line_string ls;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_lines= uint4korr(data);
  data+= 4;

  if (trn->start_collection(n_lines))
    return 1;

  while (n_lines--)
  {
    if (no_data(data, WKB_HEADER_SIZE))
      return 1;
    data+= WKB_HEADER_SIZE;
    ls.set_data_ptr(data, (uint32) (m_data_end - data));
    if (ls.store_shapes(trn))
      return 1;
    data+= ls.get_data_size();
  }
  return 0;
}


const Geometry::Class_info *Gis_multi_line_string::get_class_info() const
{
  return &multilinestring_class;
}


/***************************** MultiPolygon *******************************/

uint32 Gis_multi_polygon::get_data_size() const 
{
  uint32 n_polygons;
  uint32 n_points;
  const char *data= m_data;

  if (no_data(data, 4))
    return GET_SIZE_ERROR;
  n_polygons= uint4korr(data);
  data+= 4;

  while (n_polygons--)
  {
    uint32 n_linear_rings;
    if (no_data(data, 4 + WKB_HEADER_SIZE))
      return GET_SIZE_ERROR;

    n_linear_rings= uint4korr(data + WKB_HEADER_SIZE);
    data+= 4 + WKB_HEADER_SIZE;

    while (n_linear_rings--)
    {
      if (no_data(data, 4) ||
          not_enough_points(data+4, (n_points= uint4korr(data))))
	return GET_SIZE_ERROR;
      data+= 4 + n_points * POINT_DATA_SIZE;
    }
  }
  if (no_data(data, 0))
    return GET_SIZE_ERROR;
  return (uint32) (data - m_data);
}


bool Gis_multi_polygon::init_from_wkt(Gis_read_stream *trs, String *wkb)
{
  uint32 n_polygons= 0;
  int np_pos= wkb->length();
  Gis_polygon p;

  if (wkb->reserve(4, 512))
    return 1;
  wkb->length(wkb->length()+4);			// Reserve space for points

  for (;;)  
  {
    if (wkb->reserve(1 + 4, 512))
      return 1;
    wkb->q_append((char) wkb_ndr);
    wkb->q_append((uint32) wkb_polygon);

    if (trs->check_next_symbol('(') ||
	p.init_from_wkt(trs, wkb) ||
	trs->check_next_symbol(')'))
      return 1;
    n_polygons++;
    if (trs->skip_char(','))			// Didn't find ','
      break;
  }
  wkb->write_at_position(np_pos, n_polygons);
  return 0;
}


uint Gis_multi_polygon::init_from_wkb(const char *wkb, uint len,
                                      wkbByteOrder bo, String *res)
{
  uint32 n_poly;
  const char *wkb_orig= wkb;

  if (len < 4)
    return 0;
  n_poly= wkb_get_uint(wkb, bo);

  if (res->reserve(4, 512))
    return 0;
  res->q_append(n_poly);
  
  wkb+=4;
  while (n_poly--)
  {
    Gis_polygon p;
    int p_len;

    if (len < WKB_HEADER_SIZE ||
        res->reserve(WKB_HEADER_SIZE, 512))
      return 0;
    res->q_append((char) wkb_ndr);
    res->q_append((uint32) wkb_polygon);

    if (!(p_len= p.init_from_wkb(wkb + WKB_HEADER_SIZE, len,
                                 (wkbByteOrder) wkb[0], res)))
      return 0;
    p_len+= WKB_HEADER_SIZE;
    wkb+= p_len;
    len-= p_len;
  }
  return (uint) (wkb - wkb_orig);
}


uint Gis_multi_polygon::init_from_opresult(String *bin,
                                           const char *opres, uint res_len)
{
  Gis_polygon p;
  const char *opres_orig= opres;
  uint p_len;
  uint32 n_poly= 0;
  uint32 np_pos= bin->length();

  if (bin->reserve(4, 512))
    return 0;
    
  bin->q_append(n_poly);
  while (res_len)
  {
    if (bin->reserve(1 + 4, 512))
      return 0;
    bin->q_append((char)wkb_ndr);
    bin->q_append((uint32)wkb_polygon);
    if (!(p_len= p.init_from_opresult(bin, opres, res_len)))
      return 0;
    opres+= p_len;
    res_len-= p_len;
    n_poly++;
  }
  bin->write_at_position(np_pos, n_poly);
  return (uint)(opres - opres_orig);
}


bool Gis_multi_polygon::init_from_json(json_engine_t *je, bool er_on_3D,
                                       String *wkb)
{
  uint32 n_polygons= 0;
  int np_pos= wkb->length();
  Gis_polygon p;

  if (json_read_value(je))
    return TRUE;

  if (je->value_type != JSON_VALUE_ARRAY)
  {
    je->s.error= GEOJ_INCORRECT_GEOJSON;
    return TRUE;
  }

  if (wkb->reserve(4, 512))
    return TRUE;
  wkb->length(wkb->length()+4);	// Reserve space for n_rings

  while (json_scan_next(je) == 0 && je->state != JST_ARRAY_END)
  {
    DBUG_ASSERT(je->state == JST_VALUE);

    if (wkb->reserve(1 + 4, 512))
      return TRUE;
    wkb->q_append((char) wkb_ndr);
    wkb->q_append((uint32) wkb_polygon);

    if (p.init_from_json(je, er_on_3D, wkb))
      return TRUE;

    n_polygons++;
  }

  if (je->s.error)
    return TRUE;

  if (n_polygons == 0)
  {
    je->s.error= Geometry::GEOJ_EMPTY_COORDINATES;
    return TRUE;
  }
  wkb->write_at_position(np_pos, n_polygons);
  return FALSE;
}


bool Gis_multi_polygon::get_data_as_wkt(String *txt, const char **end) const
{
  uint32 n_polygons;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_polygons= uint4korr(data);
  data+= 4;

  while (n_polygons--)
  {
    uint32 n_linear_rings;
    if (no_data(data, 4 + WKB_HEADER_SIZE) ||
	txt->reserve(1, 512))
      return 1;
    n_linear_rings= uint4korr(data+WKB_HEADER_SIZE);
    data+= 4 + WKB_HEADER_SIZE;
    txt->q_append('(');

    while (n_linear_rings--)
    {
      if (no_data(data, 4))
        return 1;
      uint32 n_points= uint4korr(data);
      data+= 4;
      if (not_enough_points(data, n_points) ||
	  txt->reserve(2 + ((MAX_DIGITS_IN_DOUBLE + 1) * 2 + 1) * n_points,
		       512))
	return 1;
      txt->qs_append('(');
      data= append_points(txt, n_points, data, 0);
      (*txt) [txt->length() - 1]= ')';
      txt->qs_append(',');
    }
    (*txt) [txt->length() - 1]= ')';
    txt->qs_append(',');
  }
  txt->length(txt->length() - 1);
  *end= data;
  return 0;
}


bool Gis_multi_polygon::get_data_as_json(String *txt, uint max_dec_digits,
                                         const char **end) const
{
  uint32 n_polygons;
  const char *data= m_data;

  if (no_data(data, 4) || txt->reserve(1, 512))
    return 1;
  n_polygons= uint4korr(data);
  data+= 4;

  txt->q_append('[');
  while (n_polygons--)
  {
    uint32 n_linear_rings;
    if (no_data(data, 4 + WKB_HEADER_SIZE) ||
	txt->reserve(1, 512))
      return 1;
    n_linear_rings= uint4korr(data+WKB_HEADER_SIZE);
    data+= 4 + WKB_HEADER_SIZE;
    txt->q_append('[');

    while (n_linear_rings--)
    {
      if (no_data(data, 4))
        return 1;
      uint32 n_points= uint4korr(data);
      data+= 4;
      if (not_enough_points(data, n_points) ||
	  txt->reserve(2 + (MAX_DIGITS_IN_DOUBLE * 2 + 6) * n_points,
		       512))
	return 1;
      data= append_json_points(txt, max_dec_digits, n_points, data, 0);
      txt->qs_append(", ", 2);
    }
    txt->length(txt->length() - 2);
    txt->qs_append("], ", 3);
  }
  txt->length(txt->length() - 2);
  txt->q_append(']');
  *end= data;
  return 0;
}


class Gcalc_multipoly_transporter : public Gcalc_shape_transporter
{
protected:
  gcalc_shape_info m_si;
public:
  Gcalc_multipoly_transporter(Gcalc_heap *heap) :
    Gcalc_shape_transporter(heap), m_si(0) {}

  int single_point(double x, double y) override { return 0; }
  int start_line() override { return 0; }
  int complete_line() override { return 0; }

  int start_poly() override
  {
    int_start_poly();
    return 0;
  }

  int complete_poly() override
  {
    int_complete_poly();
    m_si++;
    return 0;
  }
  int start_ring() override
  {
    int_start_ring();
    return 0;
  }
  int complete_ring() override
  {
    int_complete_ring();
    return 0;
  }
  int add_point(double x, double y) override
  {
    return int_add_point(m_si, x, y);
  }

  int start_collection(int n_objects) override
  {
    return 0;
  }

  int empty_shape() override { return 0; }
};


int Gis_multi_polygon::is_valid(int *valid) const
{
  int result= 0;
  Gcalc_scan_iterator scan_it;
  Gcalc_heap collector;
  Gcalc_multipoly_transporter trn(&collector);
  MBR mbr;
  uint32 num_geometries;
  const char *c_end;
  char *internals;

  if (this->num_geometries(&num_geometries))
    return 1;

  if (shapes_valid(valid))
    return 1;

  if (*valid == 0)
    return 0;

  *valid= 0;

  if (num_geometries < 1)
    return 0;
  
  if(this->get_mbr(&mbr, &c_end))
    return 1;


  collector.set_extent(mbr.xmin, mbr.xmax, mbr.ymin, mbr.ymax);

  if (this->store_shapes(&trn))
    return 1;

  
  collector.prepare_operation();
  scan_it.init(&collector);
  internals= (char *) my_alloca(num_geometries);

  while (scan_it.more_points())
  {
    const Gcalc_scan_iterator::event_point *events, *next_ev;

    if (scan_it.step())
    {
      result= 1;
      goto exit;
    }

    events= scan_it.get_events();

    Gcalc_point_iterator pit(&scan_it);

    bzero(internals, num_geometries);
    /* Walk to the event, marking polygons we met */
    for (; pit.point() != scan_it.get_event_position(); ++pit)
    {
      gcalc_shape_info si= pit.point()->get_shape();
      internals[si]^= 1;
    }

    if (events->simple_event())
      continue;

    /* Check the status of the event point */
    for (; events; events= events->get_next())
    {
      gcalc_shape_info si= events->get_shape();
      if (events->event == scev_thread ||
          events->event == scev_end || /* should never happen. */
          events->event == scev_single_point ||
          events->event == scev_intersection)
      {
        /* These types of events never happen in valid multipolygon. */
        goto exit;
      }

      if ((internals[si]^= 1))
      {
        for (uint n=0; n<num_geometries; n++)
        {
          if (n != si && internals[n])
          {
            /* Polygons overlap */
            goto exit;
          }
        }
      }

      if ((next_ev= events->get_next()))
      {
        if (next_ev->event != scev_two_ends &&
            events->event != scev_two_ends &&
            events->cmp_dx_dy(events->dx, events->dy,
                              next_ev->dx, next_ev->dy) == 0)
        {
          /* Only can touch at points, not lines. */
          goto exit;
        }
      }
    }
  }

  *valid= 1;

exit:
  collector.reset();
  scan_it.reset();

  return result;
}


bool Gis_multi_polygon::get_mbr(MBR *mbr, const char **end) const
{
  uint32 n_polygons;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_polygons= uint4korr(data);
  data+= 4;

  while (n_polygons--)
  {
    uint32 n_linear_rings;
    if (no_data(data, 4+WKB_HEADER_SIZE))
      return 1;
    n_linear_rings= uint4korr(data + WKB_HEADER_SIZE);
    data+= WKB_HEADER_SIZE + 4;

    while (n_linear_rings--)
    {
      if (!(data= get_mbr_for_points(mbr, data, 0)))
	return 1;
    }
  }
  *end= data;
  return 0;
}


int Gis_multi_polygon::num_geometries(uint32 *num) const
{
  *num= uint4korr(m_data);
  return 0;
}


int Gis_multi_polygon::geometry_n(uint32 num, String *result) const
{
  uint32 n_polygons;
  const char *data= m_data, *start_of_polygon;

  if (no_data(data, 4))
    return 1;
  n_polygons= uint4korr(data);
  data+= 4;

  if (num > n_polygons || num < 1)
    return -1;

  do
  {
    uint32 n_linear_rings;
    start_of_polygon= data;

    if (no_data(data, WKB_HEADER_SIZE + 4))
      return 1;
    n_linear_rings= uint4korr(data + WKB_HEADER_SIZE);
    data+= WKB_HEADER_SIZE + 4;

    while (n_linear_rings--)
    {
      uint32 n_points;
      if (no_data(data, 4))
	return 1;
      n_points= uint4korr(data);
      if (not_enough_points(data + 4, n_points))
        return 1;
      data+= 4 + POINT_DATA_SIZE * n_points;
    }
  } while (--num);
  if (no_data(data, 0))				// We must check last segment
    return 1;
  return result->append(start_of_polygon, (uint32) (data - start_of_polygon),
			(uint32) 0);
}


int Gis_multi_polygon::area(double *ar,  const char **end_of_data) const
{
  uint32 n_polygons;
  const char *data= m_data;
  double result= 0;

  if (no_data(data, 4))
    return 1;
  n_polygons= uint4korr(data);
  data+= 4;

  while (n_polygons--)
  {
    double p_area;
    Gis_polygon p;

    data+= WKB_HEADER_SIZE;
    p.set_data_ptr(data, (uint32) (m_data_end - data));
    if (p.area(&p_area, &data))
      return 1;
    result+= p_area;
  }
  *ar= result;
  *end_of_data= data;
  return 0;
}

int Gis_multi_polygon::simplify(String *result, double max_distance) const
{
  uint32 num_polygon= 0, num_invalid_polygon= 0;
  Geometry_buffer buffer;
  Geometry *geometry= NULL;

  if (this->num_geometries(&num_polygon))
    return 1;

  result->length(0);
  result->reserve(SRID_SIZE + WKB_HEADER_SIZE);
  result->q_append(SRID_PLACEHOLDER);
  result->q_append((char) wkb_ndr);
  result->q_append((uint32) wkb_multipolygon);
  result->q_append((uint32) num_polygon);

  for (uint32 i = 1; i <= num_polygon; i++)
  {
    String polygon= 0, simplified_polygon= 0;
    polygon.q_append((uint) 0);
    if (this->geometry_n(i, &polygon) ||
        !(geometry= Geometry::construct(&buffer, polygon.ptr(),
          polygon.length())))
      return 1;

    if(geometry->simplify(&simplified_polygon, max_distance))
    {
      num_invalid_polygon++;
      continue;
    }

    result->append(simplified_polygon.ptr() + SRID_SIZE,
                   simplified_polygon.length() - SRID_SIZE);
  }

  if (num_polygon == num_invalid_polygon)
    return 1;

  result->write_at_position(SRID_SIZE + WKB_HEADER_SIZE,
                            ((uint32) num_polygon - num_invalid_polygon));
  return 0;
}

int Gis_multi_polygon::centroid(String *result) const
{
  uint32 n_polygons;
  Gis_polygon p;
  double res_area= 0.0, res_cx= 0.0, res_cy= 0.0;
  double cur_area, cur_cx, cur_cy;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_polygons= uint4korr(data);
  data+= 4;

  while (n_polygons--)
  {
    data+= WKB_HEADER_SIZE;
    p.set_data_ptr(data, (uint32) (m_data_end - data));
    if (p.area(&cur_area, &data) ||
	p.centroid_xy(&cur_cx, &cur_cy))
      return 1;

    res_area+= cur_area;
    res_cx+= cur_area * cur_cx;
    res_cy+= cur_area * cur_cy;
  }
   
  res_cx/= res_area;
  res_cy/= res_area;

  return create_point(result, res_cx, res_cy);
}


int Gis_multi_polygon::store_shapes(Gcalc_shape_transporter *trn) const
{
  uint32 n_polygons;
  Gis_polygon p;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_polygons= uint4korr(data);
  data+= 4;

  if (trn->start_collection(n_polygons))
    return 1;

  while (n_polygons--)
  {
    if (no_data(data, WKB_HEADER_SIZE))
      return 1;
    data+= WKB_HEADER_SIZE;
    p.set_data_ptr(data, (uint32) (m_data_end - data));
    if (p.store_shapes(trn))
      return 1;
    data+= p.get_data_size();
  }
  return 0;
}


int Gis_multi_polygon::shapes_valid(int *valid) const
{
  uint32 n_polygons;
  Gis_polygon p;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_polygons= uint4korr(data);
  data+= 4;

  *valid= 0;

  while (n_polygons--)
  {
    if (no_data(data, WKB_HEADER_SIZE))
      return 1;
    data+= WKB_HEADER_SIZE;
    p.set_data_ptr(data, (uint32) (m_data_end - data));
    if (p.is_valid(valid))
      return 1;

    if (*valid == 0)
      break;

    data+= p.get_data_size();
  }

  return 0;
}


int Gis_multi_polygon::make_clockwise(String *result) const
{
  Geometry_buffer buffer;
  uint32 num_polygons;
  Geometry *polygon;

  if(this->num_geometries(&num_polygons) ||
     result->reserve(SRID_SIZE + WKB_HEADER_SIZE))
    return 1;

  result->q_append((char) wkb_ndr);
  result->q_append((uint32) wkb_multipolygon);
  result->q_append((uint32) num_polygons);
  for (uint32 i= 1; i <= num_polygons; i++)
  {
    String wkb= 0, clockwise_wkb= 0;
    if (wkb.reserve(SRID_SIZE + BYTE_ORDER_SIZE + WKB_HEADER_SIZE))
      return 0;

    wkb.q_append(SRID_PLACEHOLDER);
    if (this->geometry_n(i, &wkb) ||
        !(polygon= Geometry::construct(&buffer, wkb.ptr(), wkb.length())))
      return 1;

    if (polygon->make_clockwise(&clockwise_wkb))
      return 1;

    // Reserve space for the byte order and GIS type.
    if (result->reserve(sizeof(char) + sizeof(uint32)))
      return 1;
    result->q_append((char) wkb_ndr);
    result->q_append((uint32) wkb_polygon);
    result->append(clockwise_wkb.ptr() + WKB_HEADER_SIZE,
                   clockwise_wkb.length() - WKB_HEADER_SIZE);
  }

  return 0;
}


const Geometry::Class_info *Gis_multi_polygon::get_class_info() const
{
  return &multipolygon_class;
}


/************************* GeometryCollection ****************************/

uint32 Gis_geometry_collection::get_data_size() const 
{
  uint32 n_objects;
  const char *data= m_data;
  Geometry_buffer buffer;
  Geometry *geom;

  if (no_data(data, 4))
    return GET_SIZE_ERROR;
  n_objects= uint4korr(data);
  data+= 4;

  while (n_objects--)
  {
    uint32 wkb_type,object_size;

    if (no_data(data, WKB_HEADER_SIZE))
      return GET_SIZE_ERROR;
    wkb_type= uint4korr(data + 1);
    data+= WKB_HEADER_SIZE;

    if (!(geom= create_by_typeid(&buffer, wkb_type)))
      return GET_SIZE_ERROR;
    geom->set_data_ptr(data, (uint) (m_data_end - data));
    if ((object_size= geom->get_data_size()) == GET_SIZE_ERROR)
      return GET_SIZE_ERROR;
    data+= object_size;
  }
  return (uint32) (data - m_data);
}


bool Gis_geometry_collection::init_from_wkt(Gis_read_stream *trs, String *wkb)
{
  uint32 n_objects= 0;
  uint32 no_pos= wkb->length();
  Geometry_buffer buffer;
  Geometry *g;
  char next_sym;

  if (wkb->reserve(4, 512))
    return 1;
  wkb->length(wkb->length()+4);			// Reserve space for points

  if (!(next_sym= trs->next_symbol()))
    return 1;

  if (next_sym != ')')
  {
    LEX_STRING next_word;
    if (trs->lookup_next_word(&next_word))
      return 1;

    if (next_word.length != 5 ||
	     (my_charset_latin1.strnncoll("empty", 5, next_word.str, 5) != 0))
    {
      for (;;)
      {
        if (!(g= create_from_wkt(&buffer, trs, wkb)))
          return 1;

        if (g->get_class_info()->m_type_id == wkb_geometrycollection)
        {
          trs->set_error_msg("Unexpected GEOMETRYCOLLECTION");
          return 1;
        }
        n_objects++;
        if (trs->skip_char(','))			// Didn't find ','
          break;
      }
    }
  }

  wkb->write_at_position(no_pos, n_objects);
  return 0;
}


uint Gis_geometry_collection::init_from_opresult(String *bin,
                                                 const char *opres,
                                                 uint res_len)
{
  const char *opres_orig= opres;
  Geometry_buffer buffer;
  Geometry *geom;
  int g_len;
  uint32 wkb_type;
  int no_pos= bin->length();
  uint32 n_objects= 0;

  if (bin->reserve(4, 512))
    return 0;
  bin->q_append(n_objects);
  
  if (res_len == 0)
  {
    /* Special case of GEOMETRYCOLLECTION EMPTY. */
    opres+= 1;
    goto empty_geom;
  }
  
  while (res_len)
  {
    switch ((Gcalc_function::shape_type) uint4korr(opres))
    {
      case Gcalc_function::shape_point:   wkb_type= wkb_point; break;
      case Gcalc_function::shape_line:    wkb_type= wkb_linestring; break;
      case Gcalc_function::shape_polygon: wkb_type= wkb_polygon; break;
      default: wkb_type= 0; DBUG_ASSERT(FALSE);
    };

    if (bin->reserve(WKB_HEADER_SIZE, 512))
      return 0;

    bin->q_append((char) wkb_ndr);
    bin->q_append(wkb_type);

    if (!(geom= create_by_typeid(&buffer, wkb_type)) ||
        !(g_len= geom->init_from_opresult(bin, opres, res_len)))
      return 0;
    opres+= g_len;
    res_len-= g_len;
    n_objects++;
  }
empty_geom:
  bin->write_at_position(no_pos, n_objects);
  return (uint) (opres - opres_orig);
}


uint Gis_geometry_collection::init_from_wkb(const char *wkb, uint len,
                                            wkbByteOrder bo, String *res)
{
  uint32 n_geom;
  const char *wkb_orig= wkb;

  if (len < 4)
    return 0;
  n_geom= wkb_get_uint(wkb, bo);

  if (res->reserve(4, 512))
    return 0;
  res->q_append(n_geom);
  
  wkb+= 4;
  while (n_geom--)
  {
    Geometry_buffer buffer;
    Geometry *geom;
    int g_len;
    uint32 wkb_type;

    if (len < WKB_HEADER_SIZE ||
        res->reserve(WKB_HEADER_SIZE, 512))
      return 0;

    res->q_append((char) wkb_ndr);
    wkb_type= wkb_get_uint(wkb+1, (wkbByteOrder) wkb[0]);
    res->q_append(wkb_type);

    if (!(geom= create_by_typeid(&buffer, wkb_type)) ||
        !(g_len= geom->init_from_wkb(wkb + WKB_HEADER_SIZE, len,
                                     (wkbByteOrder)  wkb[0], res)))
      return 0;
    g_len+= WKB_HEADER_SIZE;
    wkb+= g_len;
    len-= g_len;
  }
  return (uint) (wkb - wkb_orig);
}


bool Gis_geometry_collection::init_from_json(json_engine_t *je, bool er_on_3D,
                                             String *wkb)
{
  uint32 n_objects= 0;
  uint32 no_pos= wkb->length();
  Geometry_buffer buffer;
  Geometry *g;

  if (json_read_value(je))
    return TRUE;

  if (je->value_type != JSON_VALUE_ARRAY)
  {
    je->s.error= GEOJ_INCORRECT_GEOJSON;
    return TRUE;
  }

  if (wkb->reserve(4, 512))
    return TRUE;
  wkb->length(wkb->length()+4);	// Reserve space for n_objects

  while (json_scan_next(je) == 0 && je->state != JST_ARRAY_END)
  {
    json_engine_t sav_je= *je;

    DBUG_ASSERT(je->state == JST_VALUE);

    if (!(g= create_from_json(&buffer, je, er_on_3D, wkb)))
      return TRUE;

    *je= sav_je;
    if (json_skip_array_item(je))
      return TRUE;

    n_objects++;
  }

  wkb->write_at_position(no_pos, n_objects);
  return FALSE;
}


bool Gis_geometry_collection::get_data_as_wkt(String *txt,
					     const char **end) const
{
  uint32 n_objects;
  Geometry_buffer buffer;
  Geometry *geom;
  const char *data= m_data;

  if (no_data(data, 4))
    return 1;
  n_objects= uint4korr(data);
  data+= 4;

  if (n_objects == 0)
  {
    txt->append(STRING_WITH_LEN(" EMPTY"), 512);
    goto exit;
  }

  txt->qs_append('(');
  while (n_objects--)
  {
    uint32 wkb_type;

    if (no_data(data, WKB_HEADER_SIZE))
      return 1;
    wkb_type= uint4korr(data + 1);
    data+= WKB_HEADER_SIZE;

    if (!(geom= create_by_typeid(&buffer, wkb_type)))
      return 1;
    geom->set_data_ptr(data, (uint) (m_data_end - data));
    if (geom->as_wkt(txt, &data))
      return 1;
    if (n_objects && txt->append(STRING_WITH_LEN(","), 512))
      return 1;
  }
  txt->qs_append(')');
exit:
  *end= data;
  return 0;
}


bool Gis_geometry_collection::get_data_as_json(String *txt, uint max_dec_digits,
                                               const char **end) const
{
  uint32 n_objects;
  Geometry_buffer buffer;
  Geometry *geom;
  const char *data= m_data;

  if (no_data(data, 4) || txt->reserve(1, 512))
    return 1;
  n_objects= uint4korr(data);
  data+= 4;

  txt->qs_append('[');
  while (n_objects--)
  {
    uint32 wkb_type;

    if (no_data(data, WKB_HEADER_SIZE))
      return 1;
    wkb_type= uint4korr(data + 1);
    data+= WKB_HEADER_SIZE;

    if (!(geom= create_by_typeid(&buffer, wkb_type)))
      return 1;
    geom->set_data_ptr(data, (uint) (m_data_end - data));
    if (txt->append('{') ||
        geom->as_json(txt, max_dec_digits, &data) ||
        txt->append(STRING_WITH_LEN("}, "), 512))
      return 1;
  }
  txt->length(txt->length() - 2);
  if (txt->append(']'))
    return 1;

  *end= data;
  return 0;
}


int Gis_geometry_collection::is_valid(int *valid) const
{
  Geometry_buffer buffer;
  uint32 num_geometries;
  Geometry *geometry;
  *valid= 0;

  if (this->num_geometries(&num_geometries))
    return 1;

  for (uint32 i= 1; i <= num_geometries; i++)
  {
    String wkb= 0;

    if (wkb.reserve(SRID_SIZE + BYTE_ORDER_SIZE + WKB_HEADER_SIZE))
      return 1;

    wkb.q_append(SRID_PLACEHOLDER);
    if(this->geometry_n(i, &wkb) ||
       !(geometry= Geometry::construct(&buffer, wkb.ptr(), wkb.length())))
      return 1;

    int internal_valid;
    if (geometry->is_valid(&internal_valid))
      return 1;

    if (!internal_valid)
      return 0;
  }

  *valid= 1;
  return 0;
}


bool Gis_geometry_collection::get_mbr(MBR *mbr, const char **end) const
{
  uint32 n_objects;
  const char *data= m_data;
  Geometry_buffer buffer;
  Geometry *geom;

  if (no_data(data, 4))
    return 1;
  n_objects= uint4korr(data);
  data+= 4;
  if (n_objects == 0)
    goto exit;

  while (n_objects--)
  {
    uint32 wkb_type;

    if (no_data(data, WKB_HEADER_SIZE))
      return 1;
    wkb_type= uint4korr(data + 1);
    data+= WKB_HEADER_SIZE;

    if (!(geom= create_by_typeid(&buffer, wkb_type)))
      return 1;
    geom->set_data_ptr(data, (uint32) (m_data_end - data));
    if (geom->get_mbr(mbr, &data))
      return 1;
  }
exit:
  *end= data;
  return 0;
}


int Gis_geometry_collection::area(double *ar,  const char **end) const
{
  uint32 n_objects;
  const char *data= m_data;
  Geometry_buffer buffer;
  Geometry *geom;
  double result;

  if (no_data(data, 4))
    return 1;
  n_objects= uint4korr(data);
  data+= 4;

  result= 0.0;
  if (n_objects == 0)
    goto exit;

  while (n_objects--)
  {
    uint32 wkb_type;

    if (no_data(data, WKB_HEADER_SIZE))
      return 1;
    wkb_type= uint4korr(data + 1);
    data+= WKB_HEADER_SIZE;

    if (!(geom= create_by_typeid(&buffer, wkb_type)))
      return 1;
    geom->set_data_ptr(data, (uint32) (m_data_end - data));
    if (geom->area(ar, &data))
      return 1;
    result+= *ar;
  }
exit:
  *end= data;
  *ar= result;
  return 0;
}


int Gis_geometry_collection::simplify(String *result,
                                      double max_distance) const
{
  uint32 num_geometries= 0, num_invalid_geometries= 0;
  Geometry_buffer buffer;
  Geometry *geometry= NULL;

  if (this->num_geometries(&num_geometries))
    return 1;

  result->length(0);
  result->reserve(SRID_SIZE + BYTE_ORDER_SIZE + WKB_HEADER_SIZE);
  result->q_append(SRID_PLACEHOLDER);
  result->q_append((char) wkb_ndr);
  result->q_append((uint32) wkb_geometrycollection);
  result->q_append((uint32) num_geometries);

  for (uint32 i = 1; i <= num_geometries; i++)
  {
    String wkb= 0, simplified_wkb= 0;

    wkb.q_append((uint) 0);
    if (this->geometry_n(i, &wkb) ||
        !(geometry= Geometry::construct(&buffer, wkb.ptr(), wkb.length())))
      return 1;

    if (geometry->get_class_info()->m_type_id == Geometry::wkb_point ||
        geometry->get_class_info()->m_type_id == Geometry::wkb_multipoint)
    {
      result->append(wkb.ptr() + SRID_SIZE, wkb.length() - SRID_SIZE);
      continue;
    }

    if(geometry->simplify(&simplified_wkb, max_distance))
    {
      num_invalid_geometries++;
      continue;
    }

    result->append(simplified_wkb.ptr() + SRID_SIZE,
                   simplified_wkb.length() - SRID_SIZE);
  }

  if (num_geometries == num_invalid_geometries)
    return 1;

  result->write_at_position(SRID_SIZE + WKB_HEADER_SIZE,
                            (uint32) num_geometries - num_invalid_geometries);
  return 0;
}


int Gis_geometry_collection::geom_length(double *len, const char **end) const
{
  uint32 n_objects;
  const char *data= m_data;
  Geometry_buffer buffer;
  Geometry *geom;
  double result;

  if (no_data(data, 4))
    return 1;
  n_objects= uint4korr(data);
  data+= 4;
  result= 0.0;

  if (n_objects == 0)
    goto exit;

  while (n_objects--)
  {
    uint32 wkb_type;

    if (no_data(data, WKB_HEADER_SIZE))
      return 1;
    wkb_type= uint4korr(data + 1);
    data+= WKB_HEADER_SIZE;

    if (!(geom= create_by_typeid(&buffer, wkb_type)))
      return 1;
    geom->set_data_ptr(data, (uint32) (m_data_end - data));
    if (geom->geom_length(len, &data))
      return 1;
    result+= *len;
  }

exit:
  *end= data;
  *len= result;
  return 0;
}


int Gis_geometry_collection::num_geometries(uint32 *num) const
{
  if (no_data(m_data, 4))
    return 1;
  *num= uint4korr(m_data);
  return 0;
}


int Gis_geometry_collection::geometry_n(uint32 num, String *result) const
{
  uint32 n_objects, wkb_type, length;
  const char *data= m_data;
  Geometry_buffer buffer;
  Geometry *geom;

  if (no_data(data, 4))
    return 1;
  n_objects= uint4korr(data);
  data+= 4;
  if (num > n_objects || num < 1)
    return 1;

  do
  {
    if (no_data(data, WKB_HEADER_SIZE))
      return 1;
    wkb_type= uint4korr(data + 1);
    data+= WKB_HEADER_SIZE;

    if (!(geom= create_by_typeid(&buffer, wkb_type)))
      return 1;
    geom->set_data_ptr(data, (uint) (m_data_end - data));
    if ((length= geom->get_data_size()) == GET_SIZE_ERROR)
      return 1;
    data+= length;
  } while (--num);

  /* Copy found object to result */
  if (result->reserve(1 + 4 + length))
    return 1;
  result->q_append((char) wkb_ndr);
  result->q_append((uint32) wkb_type);
  result->q_append(data-length, length);	// data-length = start_of_data
  return 0;
}


/*
  Return dimension for object

  SYNOPSIS
    dimension()
    res_dim		Result dimension
    end			End of object will be stored here. May be 0 for
			simple objects!
  RETURN
    0	ok
    1	error
*/

bool Gis_geometry_collection::dimension(uint32 *res_dim, const char **end) const
{
  uint32 n_objects;
  const char *data= m_data;
  Geometry_buffer buffer;
  Geometry *geom;

  if (no_data(data, 4))
    return 1;
  n_objects= uint4korr(data);
  data+= 4;

  *res_dim= 0;
  while (n_objects--)
  {
    uint32 wkb_type, length, dim;
    const char *end_data;

    if (no_data(data, WKB_HEADER_SIZE))
      return 1;
    wkb_type= uint4korr(data + 1);
    data+= WKB_HEADER_SIZE;
    if (!(geom= create_by_typeid(&buffer, wkb_type)))
      return 1;
    geom->set_data_ptr(data, (uint32) (m_data_end - data));
    if (geom->dimension(&dim, &end_data))
      return 1;
    set_if_bigger(*res_dim, dim);
    if (end_data)				// Complex object
      data= end_data;
    else if ((length= geom->get_data_size()) == GET_SIZE_ERROR)
      return 1;
    else
      data+= length;
  }
  *end= data;
  return 0;
}


int Gis_geometry_collection::store_shapes(Gcalc_shape_transporter *trn) const
{
  uint32 n_objects;
  const char *data= m_data;
  Geometry_buffer buffer;
  Geometry *geom;

  if (no_data(data, 4))
    return 1;
  n_objects= uint4korr(data);
  data+= 4;

  if (!n_objects)
  {
    trn->empty_shape();
    return 0;
  }

  if (trn->start_collection(n_objects))
    return 1;

  while (n_objects--)
  {
    uint32 wkb_type;

    if (no_data(data, WKB_HEADER_SIZE))
      return 1;
    wkb_type= uint4korr(data + 1);
    data+= WKB_HEADER_SIZE;
    if (!(geom= create_by_typeid(&buffer, wkb_type)))
      return 1;
    geom->set_data_ptr(data, (uint32) (m_data_end - data));
    if (geom->store_shapes(trn))
      return 1;

    data+= geom->get_data_size();
  }
  return 0;
}


int Gis_geometry_collection::make_clockwise(String *result) const
{
  Geometry_buffer buffer;
  uint32 num_geometries;
  Geometry *geometry;

  if(this->num_geometries(&num_geometries) ||
     result->reserve(SRID_SIZE + WKB_HEADER_SIZE))
    return 1;

  result->q_append((char) wkb_ndr);
  result->q_append((uint32) wkb_geometrycollection);
  result->q_append((uint32) num_geometries);
  for (uint32 i= 1; i <= num_geometries; i++)
  {
    String wkb= 0, clockwise_wkb= 0;
    if (wkb.reserve(SRID_SIZE + BYTE_ORDER_SIZE + WKB_HEADER_SIZE))
      return 0;

    wkb.q_append(SRID_PLACEHOLDER);
    if (this->geometry_n(i, &wkb) ||
        !(geometry= Geometry::construct(&buffer, wkb.ptr(), wkb.length())))
      return 1;

    result->reserve(sizeof(char) + sizeof(uint32));
    result->q_append((char) wkb_ndr);
    result->q_append((uint32) geometry->get_class_info()->m_type_id);
    if (geometry->get_class_info()->m_type_id == Geometry::wkb_polygon ||
        geometry->get_class_info()->m_type_id == Geometry::wkb_multipolygon ||
        geometry->get_class_info()->m_type_id ==
          Geometry::wkb_geometrycollection)
    {
      if(geometry->make_clockwise(&clockwise_wkb))
        return 1;
      result->append(clockwise_wkb.ptr() + WKB_HEADER_SIZE,
                    clockwise_wkb.length() - WKB_HEADER_SIZE);
    }
    else
    {
      result->append(wkb.ptr() + SRID_SIZE + WKB_HEADER_SIZE,
                     wkb.length() - (SRID_SIZE + WKB_HEADER_SIZE));
    }
  }

  return 0;
}


const Geometry::Class_info *Gis_geometry_collection::get_class_info() const
{
  return &geometrycollection_class;
}

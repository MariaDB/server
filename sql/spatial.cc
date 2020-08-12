/*
   Copyright (c) 2002, 2013, Oracle and/or its affiliates.
   Copyright (c) 2011, 2020, MariaDB Corporation.

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

/* This is from item_func.h. Didn't want to #include the whole file. */
double my_double_round(double value, longlong dec, bool dec_unsigned,
                       bool truncate);

#ifdef HAVE_SPATIAL

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
    the dimesion of an vertical or horizontal line is 1,
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
      }
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
    wkb->q_append((char) wkb_ndr); wkb->q_append((uint32) wkb_linestring);

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
    ls_len+= WKB_HEADER_SIZE;;
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


const Geometry::Class_info *Gis_geometry_collection::get_class_info() const
{
  return &geometrycollection_class;
}

#endif /*HAVE_SPATIAL*/

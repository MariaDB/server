/*****************************************************************************
Copyright (c) 2014, 2015, Oracle and/or its affiliates. All rights reserved.
Copyright (c) 2019, 2020, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software Foundation,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
*****************************************************************************/

/**************************************************//**
@file gis0geo.h
The r-tree define from MyISAM
*******************************************************/

#ifndef _gis0geo_h
#define _gis0geo_h

#include "my_global.h"
#include "string.h"

#define SPTYPE HA_KEYTYPE_DOUBLE
#define SPLEN  8

/* Since the mbr could be a point or a linestring, in this case, area of
mbr is 0. So, we define this macro for calculating the area increasing
when we need to enlarge the mbr. */
#define LINE_MBR_WEIGHTS	0.001

/* Types of "well-known binary representation" (wkb) format. */
enum wkbType
{
  wkbPoint = 1,
  wkbLineString = 2,
  wkbPolygon = 3,
  wkbMultiPoint = 4,
  wkbMultiLineString = 5,
  wkbMultiPolygon = 6,
  wkbGeometryCollection = 7
};

/* Byte order of "well-known binary representation" (wkb) format. */
enum wkbByteOrder
{
  wkbXDR = 0,    /* Big Endian    */
  wkbNDR = 1     /* Little Endian */
};

/*************************************************************//**
Calculate minimal bounding rectangle (mbr) of the spatial object
stored in "well-known binary representation" (wkb) format.
@return 0 if ok */
int
rtree_mbr_from_wkb(
/*===============*/
	const uchar*	wkb,		/*!< in: pointer to wkb. */
	uint	size,		/*!< in: size of wkb. */
	uint	n_dims,		/*!< in: dimensions. */
	double*	mbr);		/*!< in/out: mbr. */

/* Rtree split node structure. */
struct rtr_split_node_t
{
	double	square;		/* square of the mbr.*/
	int	n_node;		/* which group in.*/
	uchar*	key;		/* key. */
	double* coords;		/* mbr. */
};

/*************************************************************//**
Inline function for reserving coords */
inline
static
double*
reserve_coords(double	**d_buffer,	/*!< in/out: buffer. */
	       int	n_dim)		/*!< in: dimensions. */
/*===========*/
{
  double *coords = *d_buffer;
  (*d_buffer) += n_dim * 2;
  return coords;
}

/*************************************************************//**
Split rtree nodes.
Return which group the first rec is in.  */
int
split_rtree_node(
/*=============*/
	rtr_split_node_t*	node,		/*!< in: split nodes.*/
	int			n_entries,	/*!< in: entries number.*/
	int			all_size,	/*!< in: total key's size.*/
	int			key_size,	/*!< in: key's size.*/
	int			min_size,	/*!< in: minimal group size.*/
	int			size1,		/*!< in: size of group.*/
	int			size2,		/*!< in: initial group sizes */
	double**		d_buffer,	/*!< in/out: buffer.*/
	int			n_dim,		/*!< in: dimensions. */
	uchar*			first_rec);	/*!< in: the first rec. */

/** Compare two minimum bounding rectangles.
@param mode   comparison operator
   MBR_INTERSECT(a,b)  a overlaps b
   MBR_CONTAIN(a,b)    a contains b
   MBR_DISJOINT(a,b)   a disjoint b
   MBR_WITHIN(a,b)     a within   b
   MBR_EQUAL(a,b)      All coordinates of MBRs are equal
   MBR_DATA(a,b)       Data reference is the same
@param b first MBR
@param a second MBR
@retval 0 if the predicate holds
@retval 1 if the precidate does not hold */
int rtree_key_cmp(page_cur_mode_t mode, const void *b, const void *a);
#endif

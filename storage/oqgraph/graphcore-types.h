/* Copyright (C) 2007-2013 Arjen G Lentz & Antony T Curtis for Open Query

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/* ======================================================================
   Open Query Graph Computation Engine, based on a concept by Arjen Lentz
   v3 implementation by Antony Curtis, Arjen Lentz, Andrew McDonnell
   For more information, documentation, support, enhancement engineering,
   see http://openquery.com/graph or contact graph@openquery.com
   ======================================================================
*/

#ifndef oq_graphcore_types_h_
#define oq_graphcore_types_h_

namespace open_query
{

  typedef unsigned long long VertexID;
  typedef double EdgeWeight;

}


// Forward defs from mariadb itself!
class Field;
typedef struct TABLE TABLE;
class THD;

#endif

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

#include "oqgraph_shim.h"

bool oqgraph3::edge_iterator::seek()
{
  if (!_graph->_cursor ||
      _graph->_rnd_pos > _offset ||
      _graph->_cursor != _graph->_rnd_cursor.operator->())
  {
    _graph->_rnd_pos= 0;
    _graph->_rnd_cursor= new cursor(_graph);
    if (_graph->_rnd_cursor->seek_to(boost::none, boost::none))
      _graph->_rnd_pos= size_t(-1);
  }
  while (_graph->_rnd_pos < _offset)
  {
    if (_graph->_rnd_cursor->seek_next())
    {
      _offset = size_t(-1);
      return true;
    }
    _graph->_rnd_pos++;
  }
  return false;
}

oqgraph3::edge_iterator::value_type oqgraph3::edge_iterator::operator*()
{
  seek();
  return *_graph->_rnd_cursor;
}

bool oqgraph3::edge_iterator::operator==(const self& x)
{
  if (_offset == size_t(-1) && x._offset != size_t(-1))
    return const_cast<edge_iterator&>(x).seek();
  if (_offset != size_t(-1) && x._offset == size_t(-1))
    return seek();

  return _offset == x._offset;
}

bool oqgraph3::edge_iterator::operator!=(const self& x)
{
  if (_offset == size_t(-1) && x._offset != size_t(-1))
    return !const_cast<edge_iterator&>(x).seek();
  if (_offset != size_t(-1) && x._offset == size_t(-1))
    return !seek();

  return _offset != x._offset;
}

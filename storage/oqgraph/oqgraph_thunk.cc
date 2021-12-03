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

#define MYSQL_SERVER
#include <my_global.h>
#include "oqgraph_thunk.h"

#include <boost/tuple/tuple.hpp>

#include "unireg.h"
#include "sql_base.h"
#include "table.h"
#include "field.h"
#include "key.h"

#if MYSQL_VERSION_ID	< 100000
// Allow compatibility with build for 5.5.32
#define user_defined_key_parts key_parts
#endif

static int debugid = 0;

oqgraph3::vertex_id oqgraph3::edge_info::origid() const
{ return _cursor->get_origid(); }

oqgraph3::vertex_id oqgraph3::edge_info::destid() const
{ return _cursor->get_destid(); }

oqgraph3::weight_t oqgraph3::edge_info::weight() const
{ return _cursor->get_weight(); }

bool oqgraph3::cursor_ptr::operator==(const cursor_ptr& x) const
{
  if (get() == x.get())
    return true;
  return (*this)->record_position() == x->_position;
}

bool oqgraph3::cursor_ptr::operator!=(const cursor_ptr& x) const
{
  if (get() == x.get())
    return false;
  return (*this)->record_position() != x->_position;
}

oqgraph3::cursor::cursor(const graph_ptr& graph)
  : _ref_count(0)
  , _graph(graph)
  , _index(-1)
  , _parts(0)
  , _key()
  , _position()
  , _debugid(++debugid)
{ }

oqgraph3::cursor::cursor(const cursor& src)
  : _ref_count(0)
  , _graph(src._graph)
  , _index(src._index)
  , _parts(src._parts)
  , _key(src._key)
  , _position(src.record_position())
  , _debugid(++debugid)
{ }

oqgraph3::cursor::~cursor()
{
  if (this == _graph->_cursor)
  {
    if (_graph->_cursor->_index >= 0)
      _graph->_table->file->ha_index_end();
    else
      _graph->_table->file->ha_rnd_end();
    _graph->_cursor= 0;
    _graph->_stale= false;
  }
}

const std::string& oqgraph3::cursor::record_position() const
{
  if (_graph->_stale && _graph->_cursor)
  {
    TABLE& table= *_graph->_table;
    table.file->position(table.record[0]);
    _graph->_cursor->_position.assign(
        (const char*) table.file->ref, table.file->ref_length);

    if (_graph->_cursor->_index >= 0)
    {
      key_copy((uchar*) _graph->_cursor->_key.data(), table.record[0],
          table.key_info + _index, table.key_info[_index].key_length, true);
    }

    _graph->_stale= false;
  }
  return _position;
}

void oqgraph3::cursor::clear_position()
{
  _position.clear();
  if (this == _graph->_cursor)
  {
    _graph->_cursor= 0;
    _graph->_stale= false;
  }
}

void oqgraph3::cursor::save_position()
{
  record_position();

  if (this == _graph->_cursor)
  {
    TABLE& table= *_graph->_table;

    if (_graph->_cursor->_index >= 0)
      table.file->ha_index_end();
    else
      table.file->ha_rnd_end();
    _graph->_cursor= 0;
    _graph->_stale= false;
  }
}

int oqgraph3::cursor::restore_position()
{
  TABLE& table= *_graph->_table;

  if (!_position.size())
    return ENOENT;

  if (this == _graph->_cursor)
    return 0;

  if (_graph->_cursor)
    _graph->_cursor->save_position();

  if (_origid || _destid)
  {
    if (int rc= table.file->ha_index_init(_index, 1))
      return rc;

    restore_record(&table, s->default_values);

    if (_origid)
    {
      bitmap_set_bit(table.write_set, _graph->_source->field_index);
      _graph->_source->store(*_origid, 1);
      bitmap_clear_bit(table.write_set, _graph->_source->field_index);
    }

    if (_destid)
    {
      bitmap_set_bit(table.write_set, _graph->_target->field_index);
      _graph->_target->store(*_destid, 1);
      bitmap_clear_bit(table.write_set, _graph->_target->field_index);
    }

    if (int rc= table.file->ha_index_init(_index, 1))
      return rc;

    if (int rc= table.file->ha_index_read_map(
                    table.record[0], (const uchar*) _key.data(),
                    (key_part_map)(1 << _parts) - 1,
                    table.key_info[_index].user_defined_key_parts == _parts ?
                        HA_READ_KEY_EXACT : HA_READ_KEY_OR_NEXT))
    {
      table.file->ha_index_end();
      return rc;
    }

    table.file->position(table.record[0]);

    while (memcmp(table.file->ref, _position.data(), table.file->ref_length))
    {
      if (int rc= table.file->ha_index_next(table.record[0]))
      {
        table.file->ha_index_end();
        return rc;
      }

      if ((_origid && vertex_id(_graph->_source->val_int()) != *_origid) ||
          (_destid && vertex_id(_graph->_target->val_int()) != *_destid))
      {
        table.file->ha_index_end();
        return ENOENT;
      }
      table.file->position(table.record[0]);
    }

  }
  else
  {
    if (int rc= table.file->ha_rnd_init(1))
      return rc;

    if (int rc= table.file->ha_rnd_pos(
            table.record[0], (uchar*) _position.data()))
    {
      table.file->ha_rnd_end();
      return rc;
    }
  }

  _graph->_cursor= this;
  _graph->_stale= false;

  return 0;
}

oqgraph3::vertex_id oqgraph3::cursor::get_origid()
{ 
  if (_origid)
    return *_origid;

  if (this != _graph->_cursor)
  {
    if (restore_position())
      return -1;
  }
  return static_cast<vertex_id>(_graph->_source->val_int());
}

oqgraph3::vertex_id oqgraph3::cursor::get_destid()
{
  if (_destid)
    return *_destid;

  if (this != _graph->_cursor)
  {
    if (restore_position())
      return -1;
  }
  return static_cast<vertex_id>(_graph->_target->val_int());
}


oqgraph3::weight_t oqgraph3::cursor::get_weight()
{
  if (!_graph->_weight)
    return 1.0;

  if (this != _graph->_cursor)
  {
    if (restore_position())
      return -1;
  }
  return static_cast<vertex_id>(_graph->_weight->val_int());
}

int oqgraph3::cursor::seek_next()
{
  if (this != _graph->_cursor)
  {
    if (int rc= restore_position())
      return rc;
  }

  TABLE& table= *_graph->_table;

  if (_index < 0)
  {
    // We need to skip over any deleted records we encounter beyond the start of the table. Bug 796647b
    int rc;
    while ( ((rc= table.file->ha_rnd_next(table.record[0]))) != 0) {
      if (rc == HA_ERR_RECORD_DELETED)
        continue;
      table.file->ha_rnd_end();
      return clear_position(rc);
    }
    return 0;
  }

  if (int rc= table.file->ha_index_next(table.record[0]))
  {
    table.file->ha_index_end();
    return clear_position(rc);
  }

  _graph->_stale= true;

  if ((_origid && vertex_id(_graph->_source->val_int()) != *_origid) ||
      (_destid && vertex_id(_graph->_target->val_int()) != *_destid))
  {
    table.file->ha_index_end();
    return clear_position(ENOENT);
  }

  return 0;
}

int oqgraph3::cursor::seek_prev()
{
  if (this != _graph->_cursor)
  {
    if (int rc= restore_position())
      return rc;
  }

  TABLE& table= *_graph->_table;

  if (_index < 0)
  {
    return -1; // not supported
  }

  if (int rc= table.file->ha_index_prev(table.record[0]))
  {
    table.file->ha_index_end();
    return clear_position(rc);
  }

  _graph->_stale= true;

  if ((_origid && vertex_id(_graph->_source->val_int()) != *_origid) ||
      (_destid && vertex_id(_graph->_target->val_int()) != *_destid))
  {
    table.file->ha_index_end();
    return clear_position(ENOENT);
  }

  return 0;
}


int oqgraph3::cursor::seek_to(
    boost::optional<vertex_id> origid,
    boost::optional<vertex_id> destid)
{
  if (_graph->_cursor && this != _graph->_cursor)
    _graph->_cursor->save_position();

  TABLE& table= *_graph->_table;
  _index= -1;
  _origid= origid;
  _destid= destid;

  if (origid || destid)
  {
    Field *source= _graph->_source;
    Field *target= _graph->_target;

    uint source_fieldpos= _graph->_source->offset(table.record[0]);
    uint target_fieldpos= _graph->_target->offset(table.record[0]);

    if (!destid)
    {
      int i= 0;
      for( ::KEY *key_info= table.key_info,
                 *key_end= key_info + table.s->keys;
          key_info < key_end; ++key_info, ++i)
      {
        if (key_info->key_part[0].offset != source_fieldpos)
          continue;

        if (table.file->ha_index_init(i, 1))
          continue;

        restore_record(&table, s->default_values);

        bitmap_set_bit(table.write_set, source->field_index);
        source->store(*_origid, 1);
        bitmap_clear_bit(table.write_set, source->field_index);

        uchar* buff= (uchar*) my_alloca(source->pack_length());
        source->get_key_image(buff, source->pack_length(), Field::itRAW);
        _key.clear();
        _key.append((char*) buff, source->pack_length());
        _key.resize(key_info->key_length, '\0');
        my_afree(buff);

        _parts= 1;
        _index= i;
        break;
      }
    }
    else if (!origid)
    {
      int i= 0;
      for( ::KEY *key_info= table.key_info,
                 *key_end= key_info + table.s->keys;
          key_info < key_end; ++key_info, ++i)
      {
        if (key_info->key_part[0].offset != target_fieldpos)
          continue;

        if (table.file->ha_index_init(i, 1))
          continue;

        restore_record(&table, s->default_values);

        bitmap_set_bit(table.write_set, target->field_index);
        target->store(*_destid, 1);
        bitmap_clear_bit(table.write_set, target->field_index);

        uchar* buff= (uchar*) my_alloca(target->pack_length());
        target->get_key_image(buff, target->pack_length(), Field::itRAW);
        _key.clear();
        _key.append((char*) buff, target->pack_length());
        _key.resize(key_info->key_length, '\0');
        my_afree(buff);

        _parts= 1;
        _index= i;
        break;
      }
    }
    else
    {
      int i= 0;
      for( ::KEY *key_info= table.key_info,
                 *key_end= key_info + table.s->keys;
          key_info < key_end; ++key_info, ++i)
      {
        if (key_info->user_defined_key_parts < 2)
          continue;
        if (!((key_info->key_part[0].offset == target_fieldpos &&
               key_info->key_part[1].offset == source_fieldpos) ||
              (key_info->key_part[1].offset == target_fieldpos &&
               key_info->key_part[0].offset == source_fieldpos)))
          continue;

        if (table.file->ha_index_init(i, 1))
          continue;

        restore_record(&table, s->default_values);

        bitmap_set_bit(table.write_set, source->field_index);
        source->store(*_origid, 1);
        bitmap_clear_bit(table.write_set, source->field_index);

        bitmap_set_bit(table.write_set, target->field_index);
        target->store(*_destid, 1);
        bitmap_clear_bit(table.write_set, target->field_index);

        Field* first=
            key_info->key_part[0].offset == source_fieldpos ?
                source : target;
        Field* second=
            key_info->key_part[0].offset == target_fieldpos ?
                target : source;

        uchar* buff= (uchar*) my_alloca(
            source->pack_length() + target->pack_length());
        first->get_key_image(buff, first->pack_length(), Field::itRAW);
        second->get_key_image(buff + first->pack_length(), second->pack_length(), Field::itRAW);
        _key.clear();
        _key.append((char*) buff, source->pack_length() + target->pack_length());
        _key.resize(key_info->key_length, '\0');
        my_afree(buff);

        _parts= 2;
        _index= i;
        break;
      }
    }
    if (_index < 0)
    {
      // no suitable index found
      return clear_position(ENXIO);
    }

    if (int rc= table.file->ha_index_read_map(
            table.record[0], (uchar*) _key.data(),
            (key_part_map) ((1U << _parts) - 1),
            table.key_info[_index].user_defined_key_parts == _parts ?
                HA_READ_KEY_EXACT : HA_READ_KEY_OR_NEXT))
    {
      table.file->ha_index_end();
      return clear_position(rc);
    }

    if ((_origid && vertex_id(_graph->_source->val_int()) != *_origid) ||
        (_destid && vertex_id(_graph->_target->val_int()) != *_destid))
    {
      table.file->ha_index_end();
      return clear_position(ENOENT);
    }
  }
  else
  {
    int rc;
    if ((rc= table.file->ha_rnd_init(true)))
      return clear_position(rc);

    // We need to skip over any deleted records we encounter at the start of the table. Bug 796647c
    while ( ((rc= table.file->ha_rnd_next(table.record[0]))) != 0) {
      if (rc == HA_ERR_RECORD_DELETED)
        continue;
      table.file->ha_rnd_end();
      return clear_position(rc);
    }
  }

  _graph->_cursor= this;
  _graph->_stale= true;
  return 0;
}

bool oqgraph3::cursor::operator==(const cursor& x) const
{
  return record_position() == x._position;
}

bool oqgraph3::cursor::operator!=(const cursor& x) const
{
  return record_position() != x._position;
}

::THD* oqgraph3::graph::get_table_thd() { return _table->in_use; }
void oqgraph3::graph::set_table_thd(::THD* thd) { _table->in_use = thd; }

oqgraph3::graph::graph(
    ::TABLE* table,
    ::Field* source,
    ::Field* target,
    ::Field* weight)
  : _ref_count(0)
  , _cursor(0)
  , _stale(false)
  , _rnd_pos((size_t)-1)
  , _table(table)
  , _source(source)
  , _target(target)
  , _weight(weight)
{
  bitmap_set_bit(table->read_set, source->field_index);
  bitmap_set_bit(table->read_set, target->field_index);
  if (weight)
    bitmap_set_bit(table->read_set, weight->field_index);

  table->file->column_bitmaps_signal();
}

oqgraph3::graph::~graph()
{ }

oqgraph3::edges_size_type oqgraph3::graph::num_edges() const
{
  return _table->file->stats.records;
}


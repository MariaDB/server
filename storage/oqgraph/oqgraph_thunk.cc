/* Copyright (C) 2009-2011 Arjen G Lentz & Antony T Curtis for Open Query

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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/* ======================================================================
   Open Query Graph Computation Engine, based on a concept by Arjen Lentz
   Mk.III implementation by Antony Curtis & Arjen Lentz
   For more information, documentation, support, enhancement engineering,
   and non-GPL licensing, see http://openquery.com/graph
   or contact graph@openquery.com
   For packaged binaries, see http://ourdelta.org
   ======================================================================
*/

#include "oqgraph_thunk.h"

#include <boost/tuple/tuple.hpp>

#define MYSQL_SERVER
#include "mysql_priv.h"

oqgraph3::vertex_info::~vertex_info()
{ }

void oqgraph3::vertex_info::release()
{
  if (!--(_ref_count))
  {
    oqgraph3::graph& cache= *_cache;
    oqgraph3::graph::vertex_cache_type::const_iterator it;

    if (!_on_gc)
    {
      cache._gc_vertices.push(this);
      _on_gc= true;
    }

    while (!cache._gc_running && cache._gc_vertices.size() > 64)
    {
      cache._gc_running= true;
      if (cache._gc_vertices.front()->_ref_count ||
          (it= cache._cache_vertices.find(*cache._gc_vertices.front()))
              == cache._cache_vertices.end())
      {
        cache._gc_vertices.front()->_on_gc= false;
      }
      else
      {
        cache._cache_vertices.quick_erase(it);
      }
      cache._gc_vertices.pop();
      cache._gc_running= false;
    }
  }
}

oqgraph3::edge_key::~edge_key()
{ }

void oqgraph3::edge_key::release() const
{
  if (!--(_ref_count))
  {
    oqgraph3::graph& cache= *_cache;
    oqgraph3::graph::edge_cache_type::const_iterator it;

    if (!_on_gc)
    {
      cache._gc_edges.push(const_cast<edge_key*>(this));
      _on_gc= true;
    }

    while (!cache._gc_running && cache._gc_edges.size() > 512)
    {
      cache._gc_running= true;
      if (cache._gc_edges.front()->_ref_count ||
          (it= cache._cache_edges.find(*cache._gc_edges.front()))
              == cache._cache_edges.end())
      {
        cache._gc_edges.front()->_on_gc= false;
      }
      else
      {
        cache._cache_edges.quick_erase(it);
      }
      cache._gc_edges.pop();
      cache._gc_running= false;
    }
  }
}

oqgraph3::graph::graph(
    ::TABLE* table,
    ::Field* source,
    ::Field* target,
    ::Field* weight)
  : _gc_running(false)
  , _ref_count(0)
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

oqgraph3::row_cursor& oqgraph3::row_cursor::operator++()
{
  printf("%s:%d\n", __func__, __LINE__);
  if (!_current)
    return *this;

  graph::edge_cache_type::iterator
      current= _cache->_cache_edges.find(*_current);

  if (current->second._next)
  {
    graph::edge_cache_type::iterator next=
        _cache->_cache_edges.find(edge_key(*current->second._next));
    if (_cache->_cache_edges.end() != next)
    {
      _current.reset(&next->first);
      return *this;
    }
  }

  TABLE& table= *_cache->_table;

  table.file->ha_index_init(0, 1);
  
  printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_read_map");
  if (table.file->ha_index_read_map(
          table.record[0],
          reinterpret_cast<const uchar*>(current->first._ref.data()),
          (key_part_map)((1 << table.key_info->key_parts) - 1),
          HA_READ_AFTER_KEY))
  {
    table.file->ha_index_end();
    _current.reset(0);
    return *this;
  }

  update_virtual_fields(table.in_use, &table);
 
  table.file->ha_index_end();

  edge_key tmp(table.key_info->key_length, _cache);
  
  key_copy(
      reinterpret_cast<uchar*>(const_cast<char*>(tmp._ref.data())),
      table.record[0],
      table.key_info,
      tmp._ref.size(), true);

  graph::edge_cache_type::iterator
      found= _cache->_cache_edges.find(tmp);
  if (_cache->_cache_edges.end() != found)
  {
    // we already had a row, link them together
    found->second._prev= &current->first._ref;
    current->second._next= &found->first._ref;
    _current.reset(&found->first);
    return *this;
  }
  
  _current.reset(&_cache->_cache_edges.insert(
      std::make_pair(
          tmp,
          row_info(
              _cache->_source->val_int(),
              _cache->_target->val_int(),
              _cache->_weight ? _cache->_weight->val_real() : 1.0,
              &_current->_ref,
              0
          )
      )).first->first);
  return *this;
}

oqgraph3::row_cursor& oqgraph3::row_cursor::first()
{
  printf("%s:%d\n", __func__, __LINE__);
  TABLE& table= *_cache->_table;
  
  table.file->ha_index_init(0, 1);

  printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_first");
  if (!table.file->ha_index_first(table.record[0]))
  {
    update_virtual_fields(table.in_use, &table);

    edge_key tmp(table.key_info->key_length, _cache);

    key_copy(
        reinterpret_cast<uchar*>(const_cast<char*>(tmp._ref.data())),
        table.record[0],
        table.key_info,
        tmp._ref.size(), true);

    graph::edge_cache_type::iterator
        found= _cache->_cache_edges.find(tmp);
    if (found != _cache->_cache_edges.end())
    {
      // we already had a row
      _current.reset(&found->first);
    }
    else
    {
      _current.reset(&_cache->_cache_edges.insert(
          std::make_pair(
              tmp,
              row_info(
                  _cache->_source->val_int(),
                  _cache->_target->val_int(),
                  _cache->_weight ? _cache->_weight->val_real() : 1.0,
                  0,
                  0
              )
          )).first->first);
    }
  }

  table.file->ha_index_end();

  return *this;
}

oqgraph3::row_cursor& oqgraph3::row_cursor::last()
{
  printf("%s:%d\n", __func__, __LINE__);
  TABLE& table= *_cache->_table;
  
  table.file->ha_index_init(0, 1);

  printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_last");
  if (!table.file->ha_index_last(table.record[0]))
  {
    update_virtual_fields(table.in_use, &table);

    edge_key tmp(table.key_info->key_length, _cache);

    key_copy(
        reinterpret_cast<uchar*>(const_cast<char*>(tmp._ref.data())),
        table.record[0],
        table.key_info,
        tmp._ref.size(), true);

    graph::edge_cache_type::iterator
        found= _cache->_cache_edges.find(tmp);
    if (found != _cache->_cache_edges.end())
    {
      // we already had a row
      _current.reset(&found->first);
    }
    else
    {
      _current.reset(&_cache->_cache_edges.insert(
          std::make_pair(
              tmp,
              row_info(
                  _cache->_source->val_int(),
                  _cache->_target->val_int(),
                  _cache->_weight ? _cache->_weight->val_real() : 1.0,
                  0,
                  0
              )
          )).first->first);
    }
  }

  table.file->ha_index_end();

  return *this;
}

oqgraph3::row_cursor& oqgraph3::row_cursor::operator--()
{
  printf("%s:%d\n", __func__, __LINE__);
  if (!_current)
    return last();

  graph::edge_cache_type::iterator
      current= _cache->_cache_edges.find(*_current);

  if (current->second._prev)
  {
    graph::edge_cache_type::iterator prev=
        _cache->_cache_edges.find(edge_key(*current->second._prev));
    if (_cache->_cache_edges.end() != prev)
    {
      _current.reset(&prev->first);
      return *this;
    }
  }

  TABLE& table= *_cache->_table;
  table.file->ha_index_init(0, 1);
  
  printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_read_map");
  if (table.file->ha_index_read_map(
          table.record[0],
          reinterpret_cast<const uchar*>(current->first._ref.data()),
          (key_part_map)((1 << table.key_info->key_parts) - 1),
          HA_READ_BEFORE_KEY))
  {
    table.file->ha_index_end();
    _current.reset();
    return *this;
  }

  update_virtual_fields(table.in_use, &table);

  table.file->ha_index_end();
 
  edge_key tmp(table.key_info->key_length, _cache);
  
  key_copy(
      reinterpret_cast<uchar*>(const_cast<char*>(tmp._ref.data())),
      table.record[0],
      table.key_info,
      tmp._ref.size(), true);

  graph::edge_cache_type::iterator
      found= _cache->_cache_edges.find(tmp);
  if (_cache->_cache_edges.end() != found)
  {
    // we already had a row, link them together
    found->second._next= &current->first._ref;
    current->second._prev= &found->first._ref;
    _current.reset(&found->first);
    return *this;
  }
  
  _current.reset(&_cache->_cache_edges.insert(
      std::make_pair(
          tmp,
          row_info(
              _cache->_source->val_int(),
              _cache->_target->val_int(),
              _cache->_weight ? _cache->_weight->val_real() : 1.0,
              0,
              &_current->_ref
          )
      )).first->first);
  return *this;
}

oqgraph3::row_cursor& oqgraph3::row_cursor::operator+=(difference_type delta)
{
  printf("%s:%d\n", __func__, __LINE__);
  if (!delta || !_current)
    return *this;

  if (delta < 0)
    return *this -= (-delta);

  graph::edge_cache_type::iterator
      current= _cache->_cache_edges.find(*_current);

  bool index_started= false;
  TABLE& table= *_cache->_table;
  
  while (delta-- > 0)
  {
    if (_cache->_cache_edges.end() != current)
    {
      if (current->second._next)
      {
        graph::edge_cache_type::iterator
            next= _cache->_cache_edges.find(edge_key(*current->second._next));

        if (_cache->_cache_edges.end() != next)
        {
          current= next;
          continue;
        }
        
        if (!index_started)
        {
          table.file->ha_index_init(0, 1);
          index_started= true;
        }
        
        printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_read_map");
        if (table.file->ha_index_read_map(
                table.record[0],
                reinterpret_cast<const uchar*>(current->second._next->data()),
                (key_part_map)((1 << table.key_info->key_parts) - 1),
                HA_READ_KEY_OR_NEXT))
        {
          table.file->ha_index_end();
          _current.reset();
          return *this;
        }
      }
      else
      {
        if (!index_started)
        {
          table.file->ha_index_init(0, 1);
          index_started= true;
        }

        printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_read_map");
        if (table.file->ha_index_read_map(
                table.record[0],
                reinterpret_cast<const uchar*>(current->first._ref.data()),
                (key_part_map)((1 << table.key_info->key_parts) - 1),
                HA_READ_AFTER_KEY))
        {
          table.file->ha_index_end();
          _current.reset();
          return *this;
        }
      }

      edge_key tmp(table.key_info->key_length);

      key_copy(
          reinterpret_cast<uchar*>(const_cast<char*>(tmp._ref.data())),
          table.record[0],
          table.key_info,
          tmp._ref.size(), true);

      graph::edge_cache_type::iterator
          found= _cache->_cache_edges.find(tmp);
      if (found != _cache->_cache_edges.end())
      {
        // we already had a row, link them together
        found->second._next= &current->first._ref;
        current->second._prev= &found->first._ref;
      }
      current= found;
    }
    else
    {
      if (!index_started)
        return *this;

      printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_next");
      if (table.file->ha_index_next(table.record[0]))
      {
        table.file->ha_index_end();
        _current.reset();
        return *this;
      }

      edge_key tmp(table.key_info->key_length);

      key_copy(
          reinterpret_cast<uchar*>(const_cast<char*>(tmp._ref.data())),
          table.record[0],
          table.key_info,
          tmp._ref.size(), true);

      current= _cache->_cache_edges.find(tmp);
    }
  }

  if (index_started)
  {
    table.file->ha_index_end();
  }

  return *this;
}

oqgraph3::row_cursor& oqgraph3::row_cursor::operator-=(difference_type delta)
{
  printf("%s:%d\n", __func__, __LINE__);
  if (!delta || !_current)
    return *this;

  if (delta < 0)
    return *this += (-delta);

  graph::edge_cache_type::iterator
      current= _cache->_cache_edges.find(*_current);

  bool index_started= false;
  TABLE& table= *_cache->_table;
  
  while (delta-- > 0)
  {
    if (_cache->_cache_edges.end() != current)
    {
      if (current->second._next)
      {
        graph::edge_cache_type::iterator next=
            _cache->_cache_edges.find(edge_key(*current->second._next));

        if (_cache->_cache_edges.end() != next)
        {
          current= next;
          continue;
        }
        
        if (!index_started)
        {
          table.file->ha_index_init(0, 1);
          index_started= true;
        }
        
        printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_read_map");
        if (table.file->ha_index_read_map(
                table.record[0],
                reinterpret_cast<const uchar*>(current->second._next->data()),
                (key_part_map)((1 << table.key_info->key_parts) - 1),
                HA_READ_KEY_OR_PREV))
        {
          table.file->ha_index_end();
          return first();
        }
      }
      else
      {
        if (!index_started)
        {
          table.file->ha_index_init(0, 1);
          index_started= true;
        }

        printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_read_map");
        if (table.file->ha_index_read_map(
                table.record[0],
                reinterpret_cast<const uchar*>(current->first._ref.data()),
                (key_part_map)((1 << table.key_info->key_parts) - 1),
                HA_READ_BEFORE_KEY))
        {
          table.file->ha_index_end();
          return first();
        }
      }

      edge_key tmp(table.key_info->key_length);

      key_copy(
          reinterpret_cast<uchar*>(const_cast<char*>(tmp._ref.data())),
          table.record[0],
          table.key_info,
          tmp._ref.size(), true);

      graph::edge_cache_type::iterator
          found= _cache->_cache_edges.find(tmp);
      if (_cache->_cache_edges.end() != found)
      {
        // we already had a row, link them together
        found->second._next= &current->first._ref;
        current->second._prev= &found->first._ref;
      }
      current= found;
    }
    else
    {
      if (!index_started)
        return first();

      printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_prev");
      if (table.file->ha_index_prev(table.record[0]))
      {
        table.file->ha_index_end();
        return first();
      }

      edge_key tmp(table.key_info->key_length);

      key_copy(
          reinterpret_cast<uchar*>(const_cast<char*>(tmp._ref.data())),
          table.record[0],
          table.key_info,
          tmp._ref.size(), true);

      current= _cache->_cache_edges.find(tmp);
    }
  }

  if (index_started)
  {
    table.file->ha_index_end();
  }

  return *this;
}

oqgraph3::vertex_descriptor oqgraph3::graph::vertex(vertex_id id)
{
  printf("%s:%d\n", __func__, __LINE__);
  vertex_cache_type::const_iterator
      found= _cache_vertices.find(vertex_info(id));

  if (_cache_vertices.end() != found)
    return vertex_descriptor(const_cast<vertex_info*>(found.operator->()));
  
  return vertex_descriptor(const_cast<vertex_info*>(
      _cache_vertices.insert(vertex_info(id, this)).first.operator->()));
}

oqgraph3::edge_descriptor oqgraph3::graph::edge(const edge_key& key)
{
  printf("%s:%d\n", __func__, __LINE__);
  edge_cache_type::const_iterator
      found= _cache_edges.find(key);

  if (_cache_edges.end() != found)
    return edge_descriptor(&found->first);

  TABLE& table= *_table;

  table.file->ha_index_init(0, 0);

  printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_read_map");
  if (table.file->ha_index_read_map(
          table.record[0],
          reinterpret_cast<const uchar*>(key._ref.data()),
          (key_part_map)((1 << table.key_info->key_parts) - 1),
          HA_READ_KEY_EXACT))
  {
    table.file->ha_index_end();
    return edge_descriptor();
  }

  update_virtual_fields(table.in_use, &table);

  table.file->ha_index_end();

  return edge_descriptor(&_cache_edges.insert(
      std::make_pair(
          edge_key(key, this),
          row_info(
              _source->val_int(),
              _target->val_int(),
              _weight ? _weight->val_real() : 1.0,
              0,
              0
          )
      )).first->first);
}

oqgraph3::edge_descriptor oqgraph3::graph::edge(
    const vertex_descriptor& source,
    const vertex_descriptor& target)
{
  printf("%s:%d\n", __func__, __LINE__);
  vertex_cache_type::const_iterator xsource= _cache_vertices.find(*source);
  
  if (_cache_vertices.end() != xsource && xsource->_out_edges)
  {
    const vertex_info::edge_list_type& edges= *xsource->_out_edges;
    for (vertex_info::edge_list_type::const_iterator
             it= edges.begin(), end= edges.end(); end != it; ++it)
    {
      edge_cache_type::const_iterator
          found= _cache_edges.find(edge_key(*it));

      if (_cache_edges.end() != found &&
          target->id == found->second.second)
      {
        return edge_descriptor(&found->first);
      }
    }
    return edge_descriptor();
  }

  vertex_cache_type::const_iterator xtarget= _cache_vertices.find(*target);

  if (_cache_vertices.end() != xtarget && xtarget->_in_edges)
  {
    const vertex_info::edge_list_type& edges= *xtarget->_in_edges;
    for (vertex_info::edge_list_type::const_iterator
             it= edges.begin(), end= edges.end(); end != it; ++it)
    {
      edge_cache_type::const_iterator
          found= _cache_edges.find(edge_key(*it));

      if (_cache_edges.end() != found &&
          source->id == found->second.first)
      {
        return edge_descriptor(&found->first);
      }
    }
    return edge_descriptor();
  }
  
  // If we have an index which has both key parts, we can use that
  // to quickly retrieve the edge descriptor.
  
  TABLE& table= *_table;
  
  uint source_fieldpos= _source->offset(table.record[0]);
  uint target_fieldpos= _target->offset(table.record[0]);
  int i= 0;
  for( ::KEY *key_info= table.s->key_info,
             *key_end= key_info + table.s->keys;
      key_info < key_end; ++key_info, ++i)
  {
    if (key_info->key_parts < 2)
      continue;
    if ((key_info->key_part[0].offset == source_fieldpos &&
         key_info->key_part[1].offset == target_fieldpos) ||
        (key_info->key_part[0].offset == target_fieldpos &&
         key_info->key_part[1].offset == source_fieldpos))
    {
      // we can use this key
      restore_record(&table, s->default_values);

      bitmap_set_bit(table.write_set, _source->field_index);
      bitmap_set_bit(table.write_set, _target->field_index);
      _source->store(source->id, 1);
      _target->store(target->id, 1);
      bitmap_clear_bit(table.write_set, _source->field_index);
      bitmap_clear_bit(table.write_set, _target->field_index);
      
      uint key_len= key_info->key_length;
      uchar* key_prefix= (uchar*) my_alloca(key_len);

      table.file->ha_index_init(i, 0);
      
      key_copy(key_prefix, table.record[0], key_info, key_len, 1);

      printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_read_map");
      if (!table.file->ha_index_read_map(
              table.record[0], key_prefix, (key_part_map)3, 
              key_info->key_parts == 2 ?
                  HA_READ_KEY_EXACT : HA_READ_KEY_OR_NEXT) &&
          _source->val_int() == source->id &&
          _target->val_int() == target->id)
      {
        // We have found the edge,
        
        update_virtual_fields(table.in_use, &table);

        table.file->ha_index_end();
        my_afree(key_prefix);

        edge_key tmp(table.key_info->key_length, this);

        key_copy(
            reinterpret_cast<uchar*>(const_cast<char*>(tmp._ref.data())),
            table.record[0],
            table.key_info,
            tmp._ref.size(), true);

        graph::edge_cache_type::iterator
            found= _cache_edges.find(tmp);
        if (_cache_edges.end() != found)
        {
          // we already had the edge
          return edge_descriptor(&found->first);
        }

        return edge_descriptor(&_cache_edges.insert(
            std::make_pair(
                tmp,
                row_info(
                    _source->val_int(),
                    _target->val_int(),
                    _weight ? _weight->val_real() : 1.0,
                    0,
                    0
                )
            )).first->first);
      }
    }
  }
  
  const vertex_info::edge_list_type& edges= vertex(source->id)->out_edges();
  for (vertex_info::edge_list_type::const_iterator
           it= edges.begin(), end= edges
           .end(); end != it; ++it)
  {
    edge_cache_type::const_iterator
        found= _cache_edges.find(edge_key(*it));

    if (_cache_edges.end() != found &&
        target->id == found->second.second)
    {
      return edge_descriptor(&found->first);
    }
  }

  return edge_descriptor();
}

oqgraph3::edges_size_type oqgraph3::graph::num_edges() const
{
  return _table->file->stats.records;
}

const oqgraph3::vertex_info::edge_list_type&
oqgraph3::vertex_info::out_edges()
{
  printf("%s:%d id=%lld\n", __func__, __LINE__, id);
  if (!_out_edges)
  {
    _out_edges = edge_list_type();
    TABLE& table= *_cache->_table;

    uint source_fieldpos= _cache->_source->offset(table.record[0]);
    int i= 0;
    for( ::KEY *key_info= table.s->key_info,
               *key_end= key_info + table.s->keys;
        key_info < key_end; ++key_info, ++i)
    {
      if (key_info->key_part[0].offset == source_fieldpos)
      {
        // we can use this key
        restore_record(&table, s->default_values);

        bitmap_set_bit(table.write_set, _cache->_source->field_index);
        _cache->_source->store(id, 1);
        bitmap_clear_bit(table.write_set, _cache->_source->field_index);

        uint key_len= key_info->key_length;
        uchar* key= (uchar*) my_alloca(key_len);

        table.file->ha_index_init(i, 1);

        key_copy(key, table.record[0], key_info, key_len, true);
        
        printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_read_map");
        if (!table.file->ha_index_read_map(
                table.record[0], key, (key_part_map)1,
                key_info->key_parts == 1 ?
                    HA_READ_KEY_EXACT : HA_READ_KEY_OR_NEXT))
        {
          // We have found an edge,
          do
          {
            update_virtual_fields(table.in_use, &table);
            
            edge_key tmp(table.key_info->key_length, _cache);

            key_copy(
                reinterpret_cast<uchar*>(const_cast<char*>(tmp._ref.data())),
                table.record[0],
                table.key_info,
                tmp._ref.size(), true);

            graph::edge_cache_type::iterator
                found= _cache->_cache_edges.find(tmp);
            if (_cache->_cache_edges.end() == found)
            {
              found= _cache->_cache_edges.insert(
                  std::make_pair(
                      tmp,
                      row_info(
                          _cache->_source->val_int(),
                          _cache->_target->val_int(),
                          _cache->_weight ? _cache->_weight->val_real() : 1.0,
                          0,
                          0
                      )
                  )).first;
            }
            _out_edges->push_back(found->first._ref);

            printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_next");
            if (table.file->ha_index_next(table.record[0]))
            {
              break;
            }
          }
          while (_cache->_source->val_int() == id);

          table.file->ha_index_end();
          my_afree(key);
          break;
        }
        table.file->ha_index_end();
      }
    }
  }
  return *_out_edges;
}

const oqgraph3::vertex_info::edge_list_type&
oqgraph3::vertex_info::in_edges()
{
  printf("%s:%d\n", __func__, __LINE__);
  if (!_in_edges)
  {
    _in_edges = edge_list_type();
    TABLE& table= *_cache->_table;

    uint target_fieldpos= _cache->_target->offset(table.record[0]);
    int i= 0;
    for( ::KEY *key_info= table.s->key_info,
               *key_end= key_info + table.s->keys;
        key_info < key_end; ++key_info, ++i)
    {
      if (key_info->key_part[0].offset == target_fieldpos)
      {
        // we can use this key
        restore_record(&table, s->default_values);

        bitmap_set_bit(table.write_set, _cache->_target->field_index);
        _cache->_target->store(id, 1);
        bitmap_clear_bit(table.write_set, _cache->_target->field_index);

        uint key_len= key_info->key_length;
        uchar* key= (uchar*) my_alloca(key_len);

        table.file->ha_index_init(i, 1);

        key_copy(key, table.record[0], key_info, key_len, true);

        printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_read_map");
        if (!table.file->ha_index_read_map(
                table.record[0], key, (key_part_map)1,
                key_info->key_parts == 1 ?
                    HA_READ_KEY_EXACT : HA_READ_KEY_OR_NEXT))
        {
          // We have found an edge,
          do
          {
            update_virtual_fields(table.in_use, &table);
            
            edge_key tmp(table.key_info->key_length, _cache);

            key_copy(
                reinterpret_cast<uchar*>(const_cast<char*>(tmp._ref.data())),
                table.record[0],
                table.key_info,
                tmp._ref.size(), true);

            graph::edge_cache_type::iterator
                found= _cache->_cache_edges.find(tmp);
            if (_cache->_cache_edges.end() == found)
            {
              found= _cache->_cache_edges.insert(
                  std::make_pair(
                      tmp,
                      row_info(
                          _cache->_source->val_int(),
                          _cache->_target->val_int(),
                          _cache->_weight ? _cache->_weight->val_real() : 1.0,
                          0,
                          0
                      )
                  )).first;
            }
            _in_edges->push_back(found->first._ref);
            
            printf("%s:%d - %s\n", __func__, __LINE__, "ha_index_next");
            if (table.file->ha_index_next(table.record[0]))
              break;
          }
          while (_cache->_target->val_int() == id);

          table.file->ha_index_end();
          my_afree(key);
          break;
        }
        table.file->ha_index_end();
      }
    }
  }
  return *_in_edges;
}

std::size_t oqgraph3::vertex_info::degree()
{
  printf("%s:%d\n", __func__, __LINE__);
  return out_edges().size() + in_edges().size();
}

oqgraph3::degree_size_type oqgraph3::vertex_descriptor::in_degree() const
{
  printf("%s:%d\n", __func__, __LINE__);
  return (*this)->in_edges().size();
}

oqgraph3::degree_size_type oqgraph3::vertex_descriptor::out_degree() const
{
  printf("%s:%d\n", __func__, __LINE__);
  return (*this)->out_edges().size();
}

oqgraph3::vertex_descriptor oqgraph3::edge_descriptor::source() const
{
  printf("%s:%d\n", __func__, __LINE__);
  return (*this)->_cache->vertex(
      oqgraph3::row_cursor(*this, (*this)->_cache)->first);
}

oqgraph3::vertex_descriptor oqgraph3::edge_descriptor::target() const
{
  printf("%s:%d\n", __func__, __LINE__);
  return (*this)->_cache->vertex(
      oqgraph3::row_cursor(*this, (*this)->_cache)->second);
}


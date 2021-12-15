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

#pragma once

#include <list>
#include <queue>
#include <string>
#include <utility>

#include "graphcore-config.h"

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>
#include <boost/pending/queue.hpp>
#include <boost/ptr_container/ptr_deque.hpp>

#include "graphcore-types.h"

namespace oqgraph3
{
  typedef open_query::VertexID vertex_id;
  typedef open_query::EdgeWeight weight_t;

  typedef size_t vertices_size_type;
  typedef size_t edges_size_type;
  typedef size_t degree_size_type;

  struct graph;
  struct cursor;

  typedef boost::intrusive_ptr<graph> graph_ptr;

  struct cursor_ptr : public boost::intrusive_ptr<cursor>
  {
    cursor_ptr() : boost::intrusive_ptr<cursor>() { }
    cursor_ptr(cursor* pos) : boost::intrusive_ptr<cursor>(pos) { }

    operator const std::string&() const;
    bool operator==(const cursor_ptr&) const;
    bool operator!=(const cursor_ptr&) const;
  };

  struct edge_info
  {
    cursor_ptr _cursor;

    edge_info() : _cursor(0) { }
    explicit edge_info(const cursor_ptr& pos) : _cursor(pos) { }
    edge_info& operator=(const cursor_ptr& pos) { _cursor= pos; return *this; }

    vertex_id origid() const;
    vertex_id destid() const;
    weight_t weight() const;

    bool operator==(const edge_info&) const;
    bool operator!=(const edge_info&) const;
  };

  struct cursor
  {
    mutable int _ref_count;
    graph_ptr _graph;

    int _index;
    unsigned _parts;
    std::string _key;
    std::string _position;

    int _debugid;

    boost::optional<vertex_id> _origid;
    boost::optional<vertex_id> _destid;

    cursor(const graph_ptr& graph);
    cursor(const cursor& src);

    ~cursor();

    operator bool() const
    { return !_position.empty(); }

    operator edge_info() const
    { return edge_info(const_cast<cursor*>(this)); }

    vertex_id get_origid();
    vertex_id get_destid();
    weight_t get_weight();

    int seek_to(
        boost::optional<vertex_id> origid,
        boost::optional<vertex_id> destid);

    int seek_next();
    int seek_prev();

    void save_position();
    int restore_position();
    const std::string& record_position() const;
    void clear_position();
    int clear_position(int rc) { clear_position(); return rc; }

    bool operator==(const cursor& x) const;
    bool operator!=(const cursor& x) const;

    friend void intrusive_ptr_add_ref(cursor* ptr)
    { ++ptr->_ref_count; }
    friend void intrusive_ptr_release(cursor* ptr)
    { if (!--(ptr->_ref_count)) delete ptr; }
  };

  struct graph
  {
    mutable int _ref_count;
    cursor* _cursor;
    bool _stale;

    cursor_ptr _rnd_cursor;
    size_t _rnd_pos;

    ::TABLE* _table;
    ::Field* _source;
    ::Field* _target;
    ::Field* _weight;

    ::THD* get_table_thd();
    void set_table_thd(::THD* thd);

    graph(
        ::TABLE* table,
        ::Field* source,
        ::Field* target,
        ::Field* weight= 0);
    ~graph();

    edges_size_type num_edges() const;

    friend edges_size_type num_edges(const graph& g)
    { return g.num_edges(); }

    friend void intrusive_ptr_add_ref(graph* ptr)
    { ptr->_ref_count++; }

    friend void intrusive_ptr_release(graph* ptr)
    { ptr->_ref_count--; }
  };

}


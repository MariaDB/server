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

#include "oqgraph_judy.h"
#include "oqgraph_thunk.h"

#include <boost/graph/directed_graph.hpp>
#include <boost/graph/adjacency_iterator.hpp>

namespace open_query
{
  struct OQGraphTraversalCategory
    : public boost::bidirectional_graph_tag
    , public boost::adjacency_graph_tag
    , public boost::edge_list_graph_tag
  { };

}

namespace oqgraph3
{
  struct traversal_category
    : public boost::adjacency_graph_tag
    , public boost::bidirectional_graph_tag
    , public boost::edge_list_graph_tag
  { };

  struct edge_iterator
  {
    typedef edge_iterator self;
    typedef edge_info value_type;
    typedef edge_info& reference;
    typedef edge_info pointer;
    typedef std::ptrdiff_t difference_type;
    typedef std::input_iterator_tag iterator_category;
    edge_iterator() { }
    edge_iterator(const graph_ptr& graph, size_t offset=0)
      : _graph(graph)
      , _offset(offset) { }
    edge_iterator(const edge_iterator& pos)
      : _graph(pos._graph)
      , _offset(pos._offset) { }
    value_type operator*();
    self& operator+=(size_t n) { _offset+= n; return *this; }
    self& operator++() { ++_offset; return *this; }
    self operator++(int)
    { size_t temp= _offset++; return edge_iterator(_graph, temp); }
    bool seek();
    bool operator==(const self& x);
    bool operator!=(const self& x);
    graph_ptr _graph;
    size_t _offset;
  };

  struct vertex_iterator
  {
    typedef vertex_iterator self;
    typedef vertex_id value_type;
    typedef vertex_id& reference;
    typedef vertex_id pointer;
    typedef std::ptrdiff_t difference_type;
    typedef std::input_iterator_tag iterator_category;
    vertex_iterator() { }
    vertex_iterator(const cursor_ptr& pos) : _cursor(pos.operator->()) { }
    vertex_id operator*() const
    {
      edge_info edge(*_cursor);
      if (!_seen.test(edge.origid()))
        return edge.origid();
      else
        return edge.destid();
    }

    self& operator++()
    {
      edge_info edge(*_cursor);
      if (!_seen.test(edge.origid()))
      {
        _seen.set(edge.origid());
      }
      else
      {
        _seen.set(edge.destid());
      }

      while (_seen.test(edge.origid()) && _seen.test(edge.destid()))
      {
        if (_cursor->seek_next())
          break;
        edge= _cursor;
      }
      return *this;
    }
    self operator++(int) { cursor* t(new cursor(*_cursor)); ++(*this); return vertex_iterator(t); }
    bool operator==(const self& x) { return *_cursor == *x._cursor; }
    bool operator!=(const self& x) { return *_cursor != *x._cursor; }
    cursor_ptr _cursor;
    open_query::judy_bitset _seen;
  };


  struct out_edge_iterator
  {
    typedef out_edge_iterator self;
    typedef edge_info value_type;
    typedef edge_info& reference;
    typedef edge_info pointer;
    typedef std::ptrdiff_t difference_type;
    typedef std::input_iterator_tag iterator_category;
    out_edge_iterator() { }
    out_edge_iterator(const cursor_ptr& cursor) : _cursor(cursor) { }
    value_type operator*() { return value_type(_cursor); }
    self& operator++() { _cursor->seek_next(); return *this; }
    self operator++(int)
    { cursor_ptr t(new cursor(*_cursor)); ++(*this); return out_edge_iterator(t); }
    bool operator==(const self& x) { return _cursor == x._cursor; }
    bool operator!=(const self& x) { return _cursor != x._cursor; }
    cursor_ptr _cursor;
  };

  struct in_edge_iterator
  {
    typedef in_edge_iterator self;
    typedef edge_info value_type;
    typedef edge_info& reference;
    typedef edge_info pointer;
    typedef std::ptrdiff_t difference_type;
    typedef std::input_iterator_tag iterator_category;
    in_edge_iterator() { }
    in_edge_iterator(const cursor_ptr& cursor) : _cursor(cursor) { }
    value_type operator*() const { return value_type(_cursor); }
    self& operator++() { _cursor->seek_next(); return *this; }
    self operator++(int)
    { cursor_ptr t(new cursor(*_cursor)); ++(*this); return in_edge_iterator(t); }
    bool operator==(const self& x) const { return _cursor == x._cursor; }
    bool operator!=(const self& x) const { return _cursor != x._cursor; }
    cursor_ptr _cursor;
  };

  struct vertex_index_property_map
  {
    typedef vertex_id value_type;
    typedef value_type reference;
    typedef vertex_id key_type;
    typedef boost::readable_property_map_tag category;
    vertex_index_property_map(const graph& g) : _g(g) { }
    const graph& _g;

    friend inline reference
    get(const vertex_index_property_map&, key_type key)
    { return key; }
  };

  struct edge_weight_property_map
  {
    typedef weight_t value_type;
    typedef value_type reference;
    typedef edge_info key_type;
    typedef boost::readable_property_map_tag category;
    edge_weight_property_map(const graph& g) : _g(g) { }
    friend inline reference
    get(const edge_weight_property_map& p, const key_type& key)
    { return key.weight(); }

    const graph& _g;
  };

  struct edge_index_property_map
  {
    typedef cursor_ptr value_type;
    typedef cursor_ptr reference;
    typedef edge_info key_type;
    typedef boost::readable_property_map_tag category;
    edge_index_property_map(const graph& g) : _g(g) { }
    const graph& _g;
  };
}

namespace boost
{

  template<>
  struct graph_traits<oqgraph3::graph>
  {
    typedef oqgraph3::vertex_id vertex_descriptor;
    typedef oqgraph3::edge_info edge_descriptor;
    typedef boost::adjacency_iterator_generator<
        oqgraph3::graph,
        oqgraph3::vertex_id,
        oqgraph3::out_edge_iterator>::type adjacency_iterator;
    typedef oqgraph3::out_edge_iterator out_edge_iterator;
    typedef oqgraph3::in_edge_iterator in_edge_iterator;
    typedef oqgraph3::vertex_iterator vertex_iterator;
    typedef oqgraph3::edge_iterator edge_iterator;

    typedef boost::directed_tag directed_category;
    typedef boost::allow_parallel_edge_tag edge_parallel_category;
    typedef oqgraph3::traversal_category traversal_category;

    typedef oqgraph3::vertices_size_type vertices_size_type;
    typedef oqgraph3::edges_size_type edges_size_type;
    typedef oqgraph3::degree_size_type degree_size_type;

    static inline oqgraph3::vertex_id null_vertex()
    { return oqgraph3::vertex_id(-1); }
  };

  template<>
  struct graph_traits<const oqgraph3::graph>
    : public graph_traits<oqgraph3::graph>
  { };

  template <>
  struct graph_property_type<oqgraph3::graph>
  {
    typedef no_property type;
  };

  template <>
  struct vertex_property_type<oqgraph3::graph>
  {
    typedef no_property type;
  };

  template <>
  struct edge_property_type<oqgraph3::graph>
  {
    typedef no_property type;
  };

#if BOOST_VERSION < 106000 && BOOST_VERSION >= 104601
  template <>
  struct graph_bundle_type<oqgraph3::graph>
  {
    typedef no_graph_bundle type;
  };

  template <>
  struct vertex_bundle_type<oqgraph3::graph>
  {
    typedef no_vertex_bundle type;
  };

  template <>
  struct edge_bundle_type<oqgraph3::graph>
  {
    typedef no_edge_bundle type;
  };
#endif

  template<>
  struct property_map<oqgraph3::graph, edge_weight_t>
  {
    typedef void type;
    typedef oqgraph3::edge_weight_property_map const_type;
  };

  template<>
  struct property_map<oqgraph3::graph, vertex_index_t>
  {
    typedef void type;
    typedef oqgraph3::vertex_index_property_map const_type;
  };

  template<>
  struct property_map<oqgraph3::graph, edge_index_t>
  {
    typedef void type;
    typedef oqgraph3::edge_index_property_map const_type;
  };

}

namespace oqgraph3
{
  using namespace boost;

  inline graph_traits<oqgraph3::graph>::vertex_descriptor
  source(
      const graph_traits<oqgraph3::graph>::edge_descriptor& e,
      const oqgraph3::graph&)
  { return e.origid(); }

  inline graph_traits<oqgraph3::graph>::vertex_descriptor
  target(
      const graph_traits<oqgraph3::graph>::edge_descriptor& e,
      const oqgraph3::graph&)
  { return e.destid(); }

  inline std::pair<
      graph_traits<oqgraph3::graph>::out_edge_iterator,
      graph_traits<oqgraph3::graph>::out_edge_iterator>
  out_edges(
      graph_traits<oqgraph3::graph>::vertex_descriptor v,
      const oqgraph3::graph& g)
  {
    oqgraph3::cursor*
        end= new oqgraph3::cursor(const_cast<oqgraph3::graph*>(&g));
    oqgraph3::cursor*
        start= new oqgraph3::cursor(const_cast<oqgraph3::graph*>(&g));
    start->seek_to(v, boost::none);
    return std::make_pair(
        graph_traits<oqgraph3::graph>::out_edge_iterator(start),
        graph_traits<oqgraph3::graph>::out_edge_iterator(end));
  }

  inline graph_traits<oqgraph3::graph>::degree_size_type
  out_degree(
      graph_traits<oqgraph3::graph>::vertex_descriptor v,
      const oqgraph3::graph& g)
  {
    std::size_t count = 0;
    for (std::pair<
            graph_traits<oqgraph3::graph>::out_edge_iterator,
            graph_traits<oqgraph3::graph>::out_edge_iterator> i= out_edges(v, g);
        i.first != i.second; ++i.first)
    {
      ++count;
    }
    return count;
  }


  inline std::pair<
      graph_traits<oqgraph3::graph>::in_edge_iterator,
      graph_traits<oqgraph3::graph>::in_edge_iterator>
  in_edges(
      graph_traits<oqgraph3::graph>::vertex_descriptor v,
      const oqgraph3::graph& g)
  {
    oqgraph3::cursor*
        end= new oqgraph3::cursor(const_cast<oqgraph3::graph*>(&g));
    oqgraph3::cursor*
        start= new oqgraph3::cursor(const_cast<oqgraph3::graph*>(&g));
    start->seek_to(boost::none, v);
    return std::make_pair(
        graph_traits<oqgraph3::graph>::in_edge_iterator(start),
        graph_traits<oqgraph3::graph>::in_edge_iterator(end));
  }

  inline graph_traits<oqgraph3::graph>::degree_size_type
  in_degree(
      graph_traits<oqgraph3::graph>::vertex_descriptor v,
      const oqgraph3::graph& g)
  {
    std::size_t count = 0;
    for (std::pair<
            graph_traits<oqgraph3::graph>::in_edge_iterator,
            graph_traits<oqgraph3::graph>::in_edge_iterator> it= in_edges(v, g);
        it.first != it.second; ++it.first)
    {
      ++count;
    }
    return count;
  }


  // EdgeListGraph concepts
  inline std::pair<
      graph_traits<oqgraph3::graph>::edge_iterator,
      graph_traits<oqgraph3::graph>::edge_iterator>
  edges(const oqgraph3::graph& g)
  {
    std::size_t end= std::size_t(-1);
    std::size_t start= end;

    if (g.num_edges())
      start= 0;

    return std::make_pair(
        graph_traits<oqgraph3::graph>::edge_iterator(
            const_cast<oqgraph3::graph*>(&g), start),
        graph_traits<oqgraph3::graph>::edge_iterator(
            const_cast<oqgraph3::graph*>(&g), end));
  }

  inline std::pair<
      graph_traits<oqgraph3::graph>::vertex_iterator,
      graph_traits<oqgraph3::graph>::vertex_iterator>
  vertices(const oqgraph3::graph& g)
  {
    oqgraph3::cursor*
        start= new oqgraph3::cursor(const_cast<oqgraph3::graph*>(&g));
    start->seek_to(boost::none, boost::none);
    return std::make_pair(
        graph_traits<oqgraph3::graph>::vertex_iterator(start),
        graph_traits<oqgraph3::graph>::vertex_iterator(
            new oqgraph3::cursor(const_cast<oqgraph3::graph*>(&g))));
  }

  inline graph_traits<oqgraph3::graph>::vertices_size_type
  num_vertices(const oqgraph3::graph& g)
  {
    std::size_t count = 0;
    for (std::pair<
            graph_traits<oqgraph3::graph>::vertex_iterator,
            graph_traits<oqgraph3::graph>::vertex_iterator> i= vertices(g);
        i.first != i.second; ++i.first)
    {
      ++count;
    }
    return count;
  }

  inline property_map<
      oqgraph3::graph,
      edge_weight_t>::const_type::reference
  get(
      edge_weight_t,
      const oqgraph3::graph& g,
      const property_map<
          oqgraph3::graph,
          edge_weight_t>::const_type::key_type& key)
  { return key.weight(); }

  inline property_map<
      oqgraph3::graph,
      edge_weight_t>::const_type
  get(edge_weight_t,
      const oqgraph3::graph& g)
  { return property_map<oqgraph3::graph, edge_weight_t>::const_type(g); }

  inline property_map<
      oqgraph3::graph,
      edge_index_t>::const_type::reference
  get(edge_index_t,
      const oqgraph3::graph&,
      const property_map<
          oqgraph3::graph,
          edge_index_t>::const_type::key_type& key)
  { return key._cursor; }

  inline property_map<oqgraph3::graph, edge_index_t>::const_type
  get(edge_index_t, const oqgraph3::graph& g)
  { return property_map<oqgraph3::graph, edge_index_t>::const_type(g); }

  inline property_map<oqgraph3::graph, edge_index_t>::const_type::reference
  get(const property_map<oqgraph3::graph, edge_index_t>::const_type&,
      const property_map<oqgraph3::graph,
      edge_index_t>::const_type::key_type& key)
  { return key._cursor; }

  inline property_map<oqgraph3::graph, vertex_index_t>::const_type::reference
  get(vertex_index_t, const oqgraph3::graph&,
      const property_map<oqgraph3::graph,
      vertex_index_t>::const_type::key_type& key)
  { return key; }

  inline property_map<oqgraph3::graph, vertex_index_t>::const_type
  get(vertex_index_t, const oqgraph3::graph& g)
  { return property_map<oqgraph3::graph, vertex_index_t>::const_type(g); }

  inline optional<graph_traits<oqgraph3::graph>::vertex_descriptor>
  find_vertex(oqgraph3::vertex_id id, const oqgraph3::graph& g)
  {
    // Fix for https://bugs.launchpad.net/oqgraph/+bug/1196020 returning vertex even when not in graph
    // Psuedocode for fix:
    // if count(*) from g->TABLE where source=id or target=id > 0 then return id else return null
    oqgraph3::cursor* found_cursor = new oqgraph3::cursor(const_cast<oqgraph3::graph*>(&g));
    bool found = (found_cursor->seek_to(id, boost::none) && found_cursor->seek_to(boost::none, id));
    delete found_cursor;
    if (found) {
      // id is neither a from or a to in a link
      return optional<graph_traits<oqgraph3::graph>::vertex_descriptor>();
    }
    return id;
  }

}


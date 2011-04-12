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

#pragma once

#include "oqgraph_thunk.h"
#include "oqgraph_judy.h"

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
    typedef edge_descriptor value_type;
    typedef edge_descriptor& reference;
    typedef edge_descriptor pointer;
    typedef std::ptrdiff_t difference_type;
    typedef std::input_iterator_tag iterator_category;
    edge_iterator() { }
    edge_iterator(row_cursor iter) : _iter(iter) { }
    edge_descriptor operator*() { return _iter->_current; }
    self& operator++() { ++(*_iter); return *this; }
    self operator++(int) { self t= *this; ++(*this); return t; }
    bool operator==(const self& x) { return _iter == x._iter; }
    bool operator!=(const self& x) { return _iter != x._iter; }
    boost::optional<row_cursor> _iter;
  };

  struct vertex_iterator
  {
    typedef vertex_iterator self;
    typedef vertex_descriptor value_type;
    typedef vertex_descriptor& reference;
    typedef vertex_descriptor pointer;
    typedef std::ptrdiff_t difference_type;
    typedef std::input_iterator_tag iterator_category;
    vertex_iterator() { }
    vertex_iterator(edge_iterator iter) : _iter(iter) { }
    vertex_descriptor operator*()
    {
      if (!_seen.test(_iter._iter->operator*().first))
        return _iter._iter->_cache->vertex(_iter._iter->operator*().first);
      else
        return _iter._iter->_cache->vertex(_iter._iter->operator*().second);
    }

    self& operator++()
    {
      if (!_seen.test(_iter._iter->operator*().first))
      {
        _seen.set(_iter._iter->operator*().first);
      }
      else
      {
        _seen.set(_iter._iter->operator*().second);
      }

      while (_iter._iter->_current &&
          _seen.test(_iter._iter->operator*().first) &&
          _seen.test(_iter._iter->operator*().second))
      {
        ++_iter;
      }
      return *this;
    }
    self operator++(int) { self t= *this; ++(*this); return t; }
    bool operator==(const self& x) { return _iter == x._iter; }
    bool operator!=(const self& x) { return _iter != x._iter; }
    edge_iterator _iter;
    open_query::judy_bitset _seen;
  };


  struct out_edge_iterator
  {
    typedef out_edge_iterator self;
    typedef vertex_info::edge_list_type::const_iterator iter_type;

    typedef edge_descriptor value_type;
    typedef edge_descriptor& reference;
    typedef edge_descriptor pointer;
    typedef std::ptrdiff_t difference_type;
    typedef std::input_iterator_tag iterator_category;
    out_edge_iterator() { }
    out_edge_iterator(iter_type iter, const vertex_descriptor& v)
      : _iter(iter), _v(v) { }
    edge_descriptor operator*()
    { return _v->_cache->edge(edge_key(*_iter)); }
    self& operator++() { ++_iter; return *this; }
    self operator++(int) { self t= *this; ++(*this); return t; }
    bool operator==(const self& x) { return _iter == x._iter; }
    bool operator!=(const self& x) { return _iter != x._iter; }
    iter_type _iter;
    vertex_descriptor _v;
  };

  struct in_edge_iterator
  {
    typedef in_edge_iterator self;
    typedef vertex_info::edge_list_type::const_iterator iter_type;

    typedef edge_descriptor value_type;
    typedef edge_descriptor& reference;
    typedef edge_descriptor pointer;
    typedef std::ptrdiff_t difference_type;
    typedef std::input_iterator_tag iterator_category;
    in_edge_iterator() { }
    in_edge_iterator(iter_type iter, const vertex_descriptor& v)
      : _iter(iter), _v(v) { }
    edge_descriptor operator*()
    { return _v->_cache->edge(edge_key(*_iter)); }
    self& operator++() { ++_iter; return *this; }
    self operator++(int) { self t= *this; ++(*this); return t; }
    bool operator==(const self& x) { return _iter == x._iter; }
    bool operator!=(const self& x) { return _iter != x._iter; }
    iter_type _iter;
    vertex_descriptor _v;
  };

  struct vertex_index_property_map
  {
    typedef vertex_id value_type;
    typedef value_type reference;
    typedef vertex_descriptor key_type;
    typedef boost::readable_property_map_tag category;
    vertex_index_property_map(const graph& g) : _g(g) { }
    const graph& _g;
  };

  struct edge_weight_property_map
  {
    typedef weight_t value_type;
    typedef value_type reference;
    typedef edge_descriptor key_type;
    typedef boost::readable_property_map_tag category;
    edge_weight_property_map(const graph& g) : _g(g) { }
    const graph& _g;
  };
  
  struct edge_index_property_map
  {
    typedef edge_key::ref_type value_type;
    typedef const edge_key::ref_type& reference;
    typedef edge_descriptor key_type;
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
    typedef oqgraph3::vertex_descriptor vertex_descriptor;
    typedef oqgraph3::edge_descriptor edge_descriptor;
    typedef boost::adjacency_iterator_generator<
        oqgraph3::graph,
        oqgraph3::vertex_descriptor,
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
    
    static inline vertex_descriptor null_vertex()
    { return oqgraph3::vertex_descriptor(); }
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

  inline std::pair<
      graph_traits<oqgraph3::graph>::out_edge_iterator,
      graph_traits<oqgraph3::graph>::out_edge_iterator>
  out_edges(
      graph_traits<oqgraph3::graph>::vertex_descriptor v,
      const oqgraph3::graph&)
  {
    const oqgraph3::vertex_info::edge_list_type&
        iter= v->out_edges();
    return std::make_pair(
        graph_traits<oqgraph3::graph>::out_edge_iterator(iter.begin(), v),
        graph_traits<oqgraph3::graph>::out_edge_iterator(iter.end(), v));
  }

  inline std::pair<
      graph_traits<oqgraph3::graph>::in_edge_iterator,
      graph_traits<oqgraph3::graph>::in_edge_iterator>
  in_edges(
      graph_traits<oqgraph3::graph>::vertex_descriptor v,
      const oqgraph3::graph&)
  {
    const oqgraph3::vertex_info::edge_list_type&
        iter= v->out_edges();
    return std::make_pair(
        graph_traits<oqgraph3::graph>::in_edge_iterator(iter.begin(), v),
        graph_traits<oqgraph3::graph>::in_edge_iterator(iter.end(), v));
  }

  // EdgeListGraph concepts
  inline std::pair<
      graph_traits<oqgraph3::graph>::edge_iterator,
      graph_traits<oqgraph3::graph>::edge_iterator>
  edges(const oqgraph3::graph& _g)
  {
    oqgraph3::graph& g= const_cast<oqgraph3::graph&>(_g);
    return std::make_pair(
        graph_traits<oqgraph3::graph>::edge_iterator(
            oqgraph3::row_cursor(&g).first()),
        graph_traits<oqgraph3::graph>::edge_iterator(
            oqgraph3::row_cursor(&g).end()));
  }

  inline std::pair<
      graph_traits<oqgraph3::graph>::vertex_iterator,
      graph_traits<oqgraph3::graph>::vertex_iterator>
  vertices(const oqgraph3::graph& g)
  {
    std::pair<
      graph_traits<oqgraph3::graph>::edge_iterator,
      graph_traits<oqgraph3::graph>::edge_iterator> epair= edges(g);
    return std::make_pair(
        graph_traits<oqgraph3::graph>::vertex_iterator(epair.first),
        graph_traits<oqgraph3::graph>::vertex_iterator(epair.second));
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
  
  inline property_map<
      oqgraph3::graph,
      edge_weight_t>::const_type::reference
  get(
      edge_weight_t,
      const oqgraph3::graph& g, 
      const property_map<
          oqgraph3::graph,
          edge_weight_t>::const_type::key_type& key)
  { return oqgraph3::row_cursor(key, const_cast<oqgraph3::graph*>(&g))->_weight; }

  inline property_map<
      oqgraph3::graph,
      edge_weight_t>::const_type
  get(edge_weight_t,
      const oqgraph3::graph& g)
  { return property_map<oqgraph3::graph, edge_weight_t>::const_type(g); }

  inline property_map<
      oqgraph3::graph,
      edge_weight_t>::const_type::reference
  get(property_map<oqgraph3::graph,
      edge_weight_t>::const_type p,
      const property_map<
          oqgraph3::graph,
          edge_weight_t>::const_type::key_type& key)
  { return oqgraph3::row_cursor(
        key, const_cast<oqgraph3::graph*>(&p._g))->_weight; }

  inline property_map<
      oqgraph3::graph,
      edge_index_t>::const_type::reference
  get(edge_index_t,
      const oqgraph3::graph&, 
      const property_map<
          oqgraph3::graph, 
          edge_index_t>::const_type::key_type& key)
  { return key->_ref; }

  inline property_map<oqgraph3::graph, edge_index_t>::const_type
  get(edge_index_t, const oqgraph3::graph& g)
  { return property_map<oqgraph3::graph, edge_index_t>::const_type(g); }

  inline property_map<oqgraph3::graph, edge_index_t>::const_type::reference
  get(property_map<oqgraph3::graph, edge_index_t>::const_type,
      const property_map<oqgraph3::graph,
      edge_index_t>::const_type::key_type& key)
  { return key->_ref; }
      
  inline property_map<oqgraph3::graph, vertex_index_t>::const_type::reference
  get(vertex_index_t, const oqgraph3::graph&, 
      const property_map<oqgraph3::graph,
      vertex_index_t>::const_type::key_type& key)
  { return key->id; }

  inline property_map<oqgraph3::graph, vertex_index_t>::const_type
  get(vertex_index_t, const oqgraph3::graph& g)
  { return property_map<oqgraph3::graph, vertex_index_t>::const_type(g); }

  inline property_map<oqgraph3::graph, vertex_index_t>::const_type::reference
  get(property_map<oqgraph3::graph, vertex_index_t>::const_type,
      const property_map<oqgraph3::graph,
      vertex_index_t>::const_type::key_type& key)
  { return key->id; }

  
  inline optional<graph_traits<oqgraph3::graph>::vertex_descriptor>
  find_vertex(oqgraph3::vertex_id id, const oqgraph3::graph& g)
  {
    graph_traits<oqgraph3::graph>::vertex_descriptor tmp(
        const_cast<oqgraph3::graph&>(g).vertex(id));
    if (tmp)
      return tmp;
    else
      return none;
  }
  

}

#if 0


  class OQGraph
  {
  public:
  
    typedef OQGraph graph_type;
  
    // more commonly used graph types
    typedef void stored_vertex;
    typedef std::size_t vertices_size_type;
    typedef std::size_t edges_size_type;
    typedef std::size_t degree_size_type;
    typedef oqgraph3::vertex_descriptor vertex_descriptor;
    typedef oqgraph3::edge_descriptor edge_descriptor;

    // iterator types


    typedef boost::adjacency_iterator_generator<
        OQGraph,
        vertex_descriptor,
        out_edge_iterator>::type adjacency_iterator;

    // miscellaneous types
    typedef boost::directed_graph_tag graph_tag;
    typedef boost::directed_tag directed_category;
    typedef boost::allow_parallel_edge_tag edge_parallel_category;
    typedef OQGraphTraversalCategory traversal_category;

    typedef oqgraph3::vertex_id vertex_index_type;
    typedef oqgraph3::edge_key::ref_type edge_index_type;
    
    typedef void edge_property_type;
    typedef void vertex_property_type;
    typedef void graph_property_type;

    // Graph concepts
    static vertex_descriptor null_vertex()
    { return vertex_descriptor(); }

    oqgraph3::graph_cache_ptr _cache;
    
    vertex_descriptor source(edge_descriptor e) const
    { return _cache->vertex(oqgraph3::row_cursor(e)->first); }
    vertex_descriptor target(edge_descriptor e) const
    { return _cache->vertex(oqgraph3::row_cursor(e)->second); }
    degree_size_type out_degree(vertex_descriptor v) const
    { return v->out_edges().size(); }
    std::pair<out_edge_iterator,out_edge_iterator>
        out_edges(vertex_descriptor v) const
    {
      const oqgraph3::vertex_info::edge_list_type&
          iter= v->out_edges();
      return std::make_pair(
          out_edge_iterator(iter.begin(), v),
          out_edge_iterator(iter.end(), v));
    }
    degree_size_type in_degree(vertex_descriptor v) const
    { return v->in_edges().size(); }
    std::pair<in_edge_iterator,in_edge_iterator>
        in_edges(vertex_descriptor v) const
    {
      const oqgraph3::vertex_info::edge_list_type&
          iter= v->out_edges();
      return std::make_pair(
          in_edge_iterator(iter.begin(), v),
          in_edge_iterator(iter.end(), v));
    }
    degree_size_type degree(vertex_descriptor v) const
    { return v->degree(); }
    vertex_descriptor vertex() const
    { return null_vertex(); }
    std::pair<edge_descriptor, bool> edge(
        vertex_descriptor u, vertex_descriptor v) const
    { edge_descriptor result= _cache->edge(u, v);
      return std::make_pair(result, bool(result)); }
    edges_size_type num_edges() const
    { return _cache->num_edges(); }
    std::pair<edge_iterator,edge_iterator>
        edges() const
    {
      return std::make_pair(
          edge_iterator(oqgraph3::row_cursor(_cache).first()),
          edge_iterator(oqgraph3::row_cursor(_cache).end()));
    }
  };
  
  
  struct vertex_index_property_map
  {
    typedef oqgraph3::vertex_id value_type;
    typedef value_type reference;
    typedef OQGraph::vertex_descriptor key_type;
    typedef boost::readable_property_map_tag category;
  };

  struct edge_weight_property_map
  {
    typedef oqgraph3::weight_t value_type;
    typedef value_type reference;
    typedef OQGraph::edge_descriptor key_type;
    typedef boost::readable_property_map_tag category;
  };
  
  struct edge_index_property_map
  {
    typedef oqgraph3::edge_key::ref_type value_type;
    typedef const oqgraph3::edge_key::ref_type& reference;
    typedef OQGraph::edge_descriptor key_type;
    typedef boost::readable_property_map_tag category;
  };
}

namespace boost
{
  template<>
  struct property_map<open_query::OQGraph, edge_weight_t>
  {
    typedef void type;
    typedef open_query::edge_weight_property_map const_type;
  };
  
  template<>
  struct property_map<open_query::OQGraph, vertex_index_t>
  {
    typedef void type;
    typedef open_query::vertex_index_property_map const_type;
  };

  template<>
  struct property_map<open_query::OQGraph, edge_index_t>
  {
    typedef void type;
    typedef open_query::edge_index_property_map const_type;
  };
  
  property_map<
      open_query::OQGraph,
      edge_weight_t>::const_type::reference
  get(
      edge_weight_t,
      const open_query::OQGraph&, 
      const property_map<
          open_query::OQGraph,
          edge_weight_t>::const_type::key_type& key)
  { return oqgraph3::row_cursor(key)->_weight; }

  property_map<
      open_query::OQGraph,
      edge_weight_t>::const_type
  get(edge_weight_t,
      const open_query::OQGraph&)
  { return property_map<open_query::OQGraph, edge_weight_t>::const_type(); }

  property_map<
      open_query::OQGraph,
      edge_weight_t>::const_type::reference
  get(property_map<open_query::OQGraph,
      edge_weight_t>::const_type,
      const property_map<
          open_query::OQGraph,
          edge_weight_t>::const_type::key_type& key)
  { return oqgraph3::row_cursor(key)->_weight; }

  property_map<
      open_query::OQGraph,
      edge_index_t>::const_type::reference
  get(edge_index_t,
      const open_query::OQGraph&, 
      const property_map<
          open_query::OQGraph, 
          edge_index_t>::const_type::key_type& key)
  { return key->_ref; }

  property_map<open_query::OQGraph, edge_index_t>::const_type
  get(edge_index_t, const open_query::OQGraph&)
  { return property_map<open_query::OQGraph, edge_index_t>::const_type(); }

  property_map<open_query::OQGraph, edge_index_t>::const_type::reference
  get(property_map<open_query::OQGraph, edge_index_t>::const_type,
      const property_map<open_query::OQGraph, edge_index_t>::const_type::key_type& key)
  { return key->_ref; }
      
  property_map<open_query::OQGraph, vertex_index_t>::const_type::reference
  get(vertex_index_t, const open_query::OQGraph&, 
      const property_map<open_query::OQGraph, vertex_index_t>::const_type::key_type& key)
  { return key->id; }

  property_map<open_query::OQGraph, vertex_index_t>::const_type
  get(vertex_index_t, const open_query::OQGraph&)
  { return property_map<open_query::OQGraph, vertex_index_t>::const_type(); }

  property_map<open_query::OQGraph, vertex_index_t>::const_type::reference
  get(property_map<open_query::OQGraph, vertex_index_t>::const_type,
      const property_map<open_query::OQGraph, vertex_index_t>::const_type::key_type& key)
  { return key->id; }

  
  optional<open_query::OQGraph::vertex_descriptor>
  find_vertex(oqgraph3::vertex_id id, const open_query::OQGraph& g)
  { open_query::OQGraph::vertex_descriptor tmp(g._cache->vertex(id));
    if (tmp)
      return tmp;
    else
      return none;
  }
  



  // IncidenceGraph concepts
  inline open_query::OQGraph::vertex_descriptor
  source(
      open_query::OQGraph::edge_descriptor e,
      open_query::OQGraph const& g)
  { return g.source(e); }

  inline open_query::OQGraph::vertex_descriptor
  target(
      open_query::OQGraph::edge_descriptor e,
      open_query::OQGraph const& g)
  { return g.target(e); }

  inline open_query::OQGraph::degree_size_type
  out_degree(
      open_query::OQGraph::vertex_descriptor v,
      open_query::OQGraph const& g)
  { return g.out_degree(v); }

  inline std::pair<
      open_query::OQGraph::out_edge_iterator,
      open_query::OQGraph::out_edge_iterator
  >
  out_edges(
      open_query::OQGraph::vertex_descriptor v,
      open_query::OQGraph const& g)
  { return g.out_edges(v); }

  // BidirectionalGraph concepts
  inline open_query::OQGraph::degree_size_type
  in_degree(
      open_query::OQGraph::vertex_descriptor v,
      open_query::OQGraph const& g)
  { return g.in_degree(v); }

  inline std::pair<
      open_query::OQGraph::in_edge_iterator,
      open_query::OQGraph::in_edge_iterator
  >
  in_edges(
      open_query::OQGraph::vertex_descriptor v,
      open_query::OQGraph const& g)
  { return g.in_edges(v); }


  inline open_query::OQGraph::degree_size_type
  degree(
      open_query::OQGraph::vertex_descriptor v,
      open_query::OQGraph const& g)
  { return g.degree(v); }

  // AdjacencyGraph concepts
  //inline std::pair<
  //    open_query::OQGraph::adjacency_iterator,
  //    open_query::OQGraph::adjacency_iterator
  //    >
  //adjacent_vertices(
  //    open_query::OQGraph::vertex_descriptor v,
  //    open_query::OQGraph const& g)
  //{ return g.adjacent_vertices(v, g.impl()); }

  open_query::OQGraph::vertex_descriptor
  vertex(
      open_query::OQGraph::vertices_size_type n,
      open_query::OQGraph const& g)
  { return g.vertex(); }

  std::pair<open_query::OQGraph::edge_descriptor, bool>
  edge(
      open_query::OQGraph::vertex_descriptor u,
      open_query::OQGraph::vertex_descriptor v,
      open_query::OQGraph const& g)
  { return g.edge(u, v); }


  // Vertex index management
  inline open_query::OQGraph::vertex_index_type
  get_vertex_index(
      open_query::OQGraph::vertex_descriptor v,
      open_query::OQGraph const& g)
  { return get(boost::vertex_index, g, v); }

  // Edge index management
  inline const open_query::OQGraph::edge_index_type&
  get_edge_index(
      open_query::OQGraph::edge_descriptor v,
      open_query::OQGraph const& g)
  { return get(boost::edge_index, g, v); }
}

#endif

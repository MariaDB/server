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

#include <list>
#include <queue>
#include <string>
#include <utility>

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
  struct vertex_info;

  typedef boost::intrusive_ptr<graph> graph_ptr;

  struct edge_key
  {
    typedef std::string ref_type;
  
    ref_type _ref;
    mutable int _ref_count;

    graph_ptr _cache;
    mutable bool _on_gc;
    
    operator const ref_type&() const { return _ref; }
    
    explicit edge_key(
        std::size_t length,
        const graph_ptr& cache= graph_ptr())
      : _ref(length, '\0')
      , _ref_count(0)
      , _cache(cache)
    { }

    template <typename C, typename T, typename A>
    explicit edge_key(const std::basic_string<C,T,A>& ref)
      : _ref(ref.begin(), ref.end())
      , _ref_count(0)
      , _cache(0)
    { }
    
    edge_key(const edge_key& src)
      : _ref(src._ref)
      , _ref_count(0)
      , _cache(src._cache)
    { }

    edge_key(const edge_key& src, const graph_ptr& cache)
      : _ref(src._ref)
      , _ref_count(0)
      , _cache(cache)
    { }
    
    ~edge_key();
    
    void release() const;

    friend void intrusive_ptr_add_ref(const edge_key* ptr)
    { ++ptr->_ref_count; }
    friend void intrusive_ptr_release(const edge_key* ptr)
    { ptr->release(); }
  };
  
  struct vertex_info
  {
    typedef vertex_id vertex_id_type;
    typedef std::list<edge_key::ref_type> edge_list_type;
    
    const vertex_id_type id;
    mutable int _ref_count;
    boost::optional<edge_list_type> _in_edges;
    boost::optional<edge_list_type> _out_edges;

    graph_ptr _cache;
    mutable bool _on_gc;
    
    explicit vertex_info(
        vertex_id_type i,
        const graph_ptr& cache= graph_ptr())
      : id(i)
      , _ref_count(0)
      , _cache(cache)
      , _on_gc(false)
    { }
    ~vertex_info();
    
    const edge_list_type& out_edges();
    const edge_list_type& in_edges();
    std::size_t degree();
    
    void release();

    friend void intrusive_ptr_add_ref(vertex_info* ptr)
    { ++ptr->_ref_count; }
    friend void intrusive_ptr_release(vertex_info* ptr)
    { ptr->release(); }
  };
  
  struct vertex_descriptor
    : public boost::intrusive_ptr<vertex_info>
  {
    vertex_descriptor()
      : boost::intrusive_ptr<vertex_info>()
    { }

    vertex_descriptor(vertex_info* v)
      : boost::intrusive_ptr<vertex_info>(v)
    { } 

    vertex_descriptor(const vertex_descriptor& v)
      : boost::intrusive_ptr<vertex_info>(v)
    { } 
  
    degree_size_type in_degree() const;
    degree_size_type out_degree() const;

    friend degree_size_type in_degree(const vertex_descriptor& v, const graph&)
    { return v.in_degree(); }
    friend degree_size_type out_degree(const vertex_descriptor& v, const graph&)
    { return v.out_degree(); }
  };

  struct edge_descriptor
    : public boost::intrusive_ptr<const edge_key>
  {
    edge_descriptor()
      : boost::intrusive_ptr<const edge_key>()
    { }
    
    edge_descriptor(const edge_key* e)
      : boost::intrusive_ptr<const edge_key>(e)
    { }
    
    edge_descriptor(const edge_descriptor& e)
      : boost::intrusive_ptr<const edge_key>(e)
    { }
    
    vertex_descriptor source() const;
    vertex_descriptor target() const;
    
    friend vertex_descriptor source(const edge_descriptor& e, const graph&)
    { return e.source(); }
    
    friend vertex_descriptor target(const edge_descriptor& e, const graph&)
    { return e.target(); }
  };

  struct row_info
    : public std::pair<vertex_id, vertex_id>
  {
    weight_t _weight;
    
    const edge_key::ref_type* _prev;
    const edge_key::ref_type* _next;
    
    row_info(
        first_type source,
        second_type target,
        weight_t weight,
        const edge_key::ref_type* prev,
        const edge_key::ref_type* next)
      : std::pair<first_type, second_type>(source, target)
      , _weight(weight)
      , _prev(prev)
      , _next(next)
    { }
  };

  namespace internal
  {
    template <typename Value> struct hash
      : public std::unary_function<Value, std::size_t>
    {
      boost::hash<Value> ref_hasher;
      std::size_t operator()(const Value& v) const
      {
        return ref_hasher(v);
      }
    };
    
    template <typename Value> struct equal_to
      : public std::binary_function<Value, Value, bool>
    {
      std::equal_to<Value> ref_equals;
      bool operator()(const Value& x, const Value& y) const
      {
        return ref_equals(x, y);
      }
    };

    template <> struct hash<edge_key>
      : public std::unary_function<edge_key, std::size_t>
    {
      boost::hash<edge_key::ref_type> ref_hasher;
      std::size_t operator()(edge_key const& v) const
      {
        return ref_hasher(v._ref);
      }
    };

    template <> struct hash<const edge_key*>
      : public std::unary_function<const edge_key*, std::size_t>
    {
      boost::hash<edge_key::ref_type> ref_hasher;
      std::size_t operator()(const edge_key* const& v) const
      {
        return ref_hasher(v->_ref);
      }
    };

    template <> struct hash<vertex_info>
      : public std::unary_function<vertex_info, std::size_t>
    {
      boost::hash<vertex_info::vertex_id_type> id_hasher;
      std::size_t operator()(vertex_info const& v) const
      {
        return id_hasher(v.id);
      }
    };
    
    template <> struct equal_to<edge_key>
      : public std::binary_function<edge_key, edge_key, bool>
    {
      std::equal_to<edge_key::ref_type> ref_equals;
      bool operator()(const edge_key& x, const edge_key& y) const
      {
        return ref_equals(x._ref, y._ref);
      }
    };

    template <> struct equal_to<const edge_key*>
      : public std::binary_function<const edge_key*, edge_key, bool>
    {
      std::equal_to<edge_key::ref_type> ref_equals;
      bool operator()(const edge_key*& x, const edge_key*& y) const
      {
        return ref_equals(x->_ref, y->_ref);
      }
    };
    
    
    template <> struct equal_to<vertex_info>
      : public std::binary_function<vertex_info, vertex_info, bool>
    {
      std::equal_to<vertex_info::vertex_id_type> id_equals;
      bool operator()(const vertex_info& x, const vertex_info& y) const
      {
        return id_equals(x.id, y.id);
      }
    };
  }
  
  struct graph
  {
    typedef boost::unordered_map<
        edge_key,
        row_info,
        internal::hash<edge_key>,
        internal::equal_to<edge_key> > edge_cache_type;
    typedef boost::unordered_set<
        vertex_info,
        internal::hash<vertex_info>,
        internal::equal_to<vertex_info> > vertex_cache_type;

    bool _gc_running;
    mutable int _ref_count;

    edge_cache_type _cache_edges;
    boost::queue<edge_key*> _gc_edges;

    vertex_cache_type _cache_vertices;
    boost::queue<vertex_info*> _gc_vertices;
    
    ::TABLE* _table;
    ::Field* _source;
    ::Field* _target;
    ::Field* _weight;
        
    graph(
        ::TABLE* table,
        ::Field* source,
        ::Field* target,
        ::Field* weight= 0);
    ~graph();
    
    vertex_descriptor vertex(vertex_id);
    edge_descriptor edge(const edge_key&);
    edge_descriptor edge(
        const vertex_descriptor&,
        const vertex_descriptor&);

    edges_size_type num_edges() const;

    friend edges_size_type num_edges(const graph& g)
    { return g.num_edges(); }

    friend void intrusive_ptr_add_ref(graph* ptr)
    { ptr->_ref_count++; }

    friend void intrusive_ptr_release(graph* ptr)
    { ptr->_ref_count--; }
  };

  struct row_cursor
  {
    typedef std::random_access_iterator_tag iterator_category;
    typedef graph::edge_cache_type::value_type::second_type value_type;
    typedef value_type* pointer;
    typedef const value_type* const_pointer;
    typedef value_type& reference;
    typedef const value_type& const_reference;
    typedef graph::edge_cache_type::size_type size_type;
    typedef graph::edge_cache_type::difference_type difference_type;

    graph_ptr _cache;
    edge_descriptor _current;
    
    explicit row_cursor(graph* cache)
      : _cache(cache)
    { }

    explicit row_cursor(const graph_ptr& cache)
      : _cache(cache)
    { }

    explicit row_cursor(const edge_descriptor& e, const graph_ptr& cache)
      : _cache(cache)
      , _current(e)
    { }
    
    reference operator*() const
    { return _cache->_cache_edges.find(*_current)->second; }
    
    pointer operator->() const
    { return &_cache->_cache_edges.find(*_current)->second; }
    
    row_cursor& operator++();
    
    row_cursor operator++(int)
    { row_cursor tmp= *this; ++*this; return tmp; }
    
    row_cursor& operator--();
    
    row_cursor operator--(int)
    { row_cursor tmp= *this; --*this; return tmp; }
    
    row_cursor& operator+=(difference_type delta);
    
    row_cursor operator+(difference_type delta) const
    { row_cursor tmp= *this; return tmp += delta; }
    
    row_cursor& operator-=(difference_type delta);

    row_cursor operator-(difference_type delta) const
    { row_cursor tmp= *this; return tmp -= delta; }
    
    reference operator[](difference_type offset) const
    { return *(*this + offset); }
    
    row_cursor& first();
    row_cursor& last();
    row_cursor& end()
    { _current.reset(); return *this; }
    
    bool operator==(const row_cursor& x) const
    { return _current == x._current; }

    bool operator!=(const row_cursor& x) const
    { return _current != x._current; }
  };

}


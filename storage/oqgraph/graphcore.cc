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

#include <string.h>
#include <cstdlib>

#include "graphcore-graph.h"

#include <set>
#include <stack>

#include <boost/property_map/property_map.hpp>

#include <boost/graph/breadth_first_search.hpp>
#include <boost/graph/dijkstra_shortest_paths.hpp>
#include <boost/graph/iteration_macros.hpp>
#include <boost/graph/reverse_graph.hpp>
#include <boost/graph/graph_utility.hpp>

#include "graphcore.h"

#include <boost/unordered_map.hpp>
#include <boost/version.hpp>

using namespace open_query;
using namespace boost;

static const row empty_row = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

extern "C" const char* const oqgraph_boost_version= BOOST_LIB_VERSION;

namespace open_query
{

  typedef graph_traits<Graph>::vertex_descriptor Vertex;
  typedef graph_traits<Graph>::edge_descriptor Edge;

  typedef std::list<std::pair<Vertex,optional<EdgeWeight> > > shortest_path_list;
  typedef shortest_path_list::iterator shortest_path_iterator;

  template<typename ID, typename IDMap>
  class id_equals_t
  {
  public:
    id_equals_t(ID id, IDMap map)
      : m_id(id), m_map(map)
    { }
    template<typename V>
    bool operator()(V u) const
    {
      return m_map[u] == m_id;
    }
  private:
    ID m_id;
    IDMap m_map;
  };

  template<typename ID, typename IDMap>
  inline id_equals_t<ID,IDMap>
  id_equals(ID id, IDMap idmap)
  {
    return id_equals_t<ID,IDMap>(id, idmap);
  }

  template<typename T, typename Graph>
  class target_equals_t
  {
  public:
    target_equals_t(T target, Graph &g)
      : m_target(target), m_g(g)
    { }
    template<typename V>
    bool operator()(V u) const
    {
      return target(u, m_g) == m_target;
    }
  private:
    T m_target;
    Graph &m_g;
  };

  template<typename T, typename Graph>
  inline target_equals_t<T,Graph>
  target_equals(T target, Graph &g)
  {
    return target_equals_t<T,Graph>(target, g);
  }

  template<typename T, typename Graph>
  class source_equals_t
  {
  public:
    source_equals_t(T source, Graph &g)
      : m_source(source), m_g(g)
    { }
    template<typename V>
    bool operator()(V u) const
    {
      return source(u, m_g) == m_source;
    }
  private:
    T m_source;
    Graph &m_g;
  };

  template<typename T, typename Graph>
  inline source_equals_t<T,Graph>
  source_equals(T source, Graph &g)
  {
    return source_equals_t<T,Graph>(source, g);
  }

  struct reference
  {
    int m_flags;
    int m_sequence;
    Vertex m_vertex;
    Edge m_edge;
    EdgeWeight m_weight;

    enum
    {
      HAVE_SEQUENCE = 1,
      HAVE_WEIGHT = 2,
      HAVE_EDGE = 4,
    };

    // Force assignment operator, so we can trace through in the debugger
    inline reference& operator=(const reference& ref) 
    {    
      m_flags = ref.m_flags;
      m_sequence = ref.m_sequence;
      m_vertex = ref.m_vertex;
      m_edge = ref.m_edge;
      m_weight = ref.m_weight;
      return *this;
    }

    inline reference()
      : m_flags(0), m_sequence(0),
        m_vertex(graph_traits<Graph>::null_vertex()),
        m_edge(), m_weight(0)
    { }

    inline reference(int s, Edge e)
      : m_flags(HAVE_SEQUENCE | HAVE_EDGE), m_sequence(s),
        m_vertex(graph_traits<Graph>::null_vertex()),
        m_edge(e), m_weight(0)
    { }

    inline reference(int s, Vertex v, const optional<Edge> &e,
                     const optional<EdgeWeight> &w)
      : m_flags(HAVE_SEQUENCE | (w ? HAVE_WEIGHT : 0) | (e ? HAVE_EDGE : 0)),
        m_sequence(s), m_vertex(v),
        m_edge(), m_weight(0)
    {
      if (w) m_weight= *w;
      if (e) m_edge= *e;
    }

    inline reference(int s, Vertex v, Edge e, EdgeWeight w)
      : m_flags(HAVE_SEQUENCE | HAVE_WEIGHT | HAVE_EDGE),
        m_sequence(s), m_vertex(v), m_edge(e), m_weight(w)
    { }

    inline reference(int s, Vertex v, EdgeWeight w)
      : m_flags(HAVE_SEQUENCE | HAVE_WEIGHT),
        m_sequence(s), m_vertex(v), m_edge(), m_weight(w)
    { }

    inline reference(int s, Vertex v)
      : m_flags(HAVE_SEQUENCE), m_sequence(s), m_vertex(v), m_edge(),
        m_weight(0)
    { }

    optional<int> sequence() const
    {
      if (m_flags & HAVE_SEQUENCE)
      {
        return m_sequence;
      }
      return optional<int>();
    }

    optional<Vertex> vertex() const
    {
      if (m_vertex != graph_traits<Graph>::null_vertex())
        return m_vertex;
      return optional<Vertex>();
    }

    optional<Edge> edge() const
    {
      if (m_flags & HAVE_EDGE)
        return m_edge;
      return optional<Edge>();
    };

    optional<EdgeWeight> weight() const
    {
      if (m_flags & HAVE_WEIGHT)
        return m_weight;
      return optional<EdgeWeight>();
    }
  };
}

namespace open_query {
  class GRAPHCORE_INTERNAL oqgraph_share
  {
  public:
    Graph g;

    optional<Vertex> find_vertex(VertexID id) const;
    optional<Edge> find_edge(Vertex, Vertex) const;

    inline oqgraph_share(
        TABLE* table,
        Field* origid,
        Field* destid,
        Field* weight) throw()
      : g(table, origid, destid, weight)
    { }
    inline ~oqgraph_share()
    { }
  };

  class GRAPHCORE_INTERNAL oqgraph_cursor
  {
  public:
    oqgraph_share *const share;

    inline oqgraph_cursor(oqgraph_share *arg)
      : share(arg)
    { }
    virtual ~oqgraph_cursor()
    { }

    virtual int fetch_row(const row &, row&) = 0;
    virtual int fetch_row(const row &, row&, const reference&) = 0;
    virtual void current(reference& ref) const = 0;
  };
}

namespace open_query {
  class GRAPHCORE_INTERNAL stack_cursor : public oqgraph_cursor
  {
  private:
    optional<EdgeWeight> no_weight;
  public:
    int sequence;
    std::stack<reference> results;
    reference last;

    inline stack_cursor(oqgraph_share *arg)
      : oqgraph_cursor(arg), no_weight(), sequence(0), results(), last()
    { }

    int fetch_row(const row &, row&);
    int fetch_row(const row &, row&, const reference&);

    void current(reference& ref) const
    {
      ref= last;
    }
  };

  class GRAPHCORE_INTERNAL vertices_cursor : public oqgraph_cursor
  {
    typedef graph_traits<Graph>::vertex_iterator vertex_iterator;

    size_t position;
    reference last;
  public:
    inline vertices_cursor(oqgraph_share *arg)
      : oqgraph_cursor(arg), position(0)
    { }

    int fetch_row(const row &, row&);
    int fetch_row(const row &, row&, const reference&);

    void current(reference& ref) const
    {
      ref= last;
    }

  };

  class GRAPHCORE_INTERNAL edges_cursor : public oqgraph_cursor
  {
    typedef graph_traits<Graph>::edge_iterator edge_iterator;
    typedef edge_iterator::difference_type edge_difference;

    edge_difference position;
    reference last;
  public:
    inline edges_cursor(oqgraph_share *arg)
      : oqgraph_cursor(arg), position(0), last()
    { }

    int fetch_row(const row &, row&);
    int fetch_row(const row &, row&, const reference&);

    void current(reference& ref) const
    {
      ref= last;
    }
  };

  template <typename P, typename D>
  struct GRAPHCORE_INTERNAL oqgraph_visit_leaves
    : public base_visitor< oqgraph_visit_leaves<P,D> >
  {
    typedef on_finish_vertex event_filter;

    oqgraph_visit_leaves(const P& p, const D& d,
                       stack_cursor *cursor)
      : seq(0), m_cursor(*cursor), m_p(p), m_d(d)
    { assert(cursor); }

    template<class T, class Graph>
    void operator()(T u, Graph &g)
    {
      typename graph_traits<Graph>::out_edge_iterator ei, ei_end;
      boost::tuples::tie(ei, ei_end) = out_edges(u, g);
      if (ei == ei_end)
      {
        m_cursor.results.push(reference(++seq, u, m_d[ u ]));
      }
    }
  private:
    int seq;
    stack_cursor &m_cursor;
    P m_p;
    D m_d;
  };

  template <typename P, typename D>
  oqgraph_visit_leaves<P,D>
    make_oqgraph_visit_leaves(const P& p, const D& d, stack_cursor *cursor)
  { return oqgraph_visit_leaves<P,D>(p, d, cursor); }


  template <typename P, typename D>
  struct GRAPHCORE_INTERNAL oqgraph_visit_dist
    : public base_visitor< oqgraph_visit_dist<P,D> >
  {
    typedef on_finish_vertex event_filter;

    oqgraph_visit_dist(const P& p, const D& d,
                       stack_cursor *cursor)
      : seq(0), m_cursor(*cursor), m_p(p), m_d(d)
    { assert(cursor); }

    template<class T, class Graph>
    void operator()(T u, Graph &g)
    {
      m_cursor.results.push(reference(++seq, u, m_d[ u ]));
    }
  private:
    int seq;
    stack_cursor &m_cursor;
    P m_p;
    D m_d;
  };

  template <typename P, typename D>
  oqgraph_visit_dist<P,D>
    make_oqgraph_visit_dist(const P& p, const D& d, stack_cursor *cursor)
  { return oqgraph_visit_dist<P,D>(p, d, cursor); }


  template<bool record_weight, typename goal_filter, typename P>
  struct GRAPHCORE_INTERNAL oqgraph_goal
    : public base_visitor<oqgraph_goal<record_weight,goal_filter,P> >
  {
    typedef goal_filter event_filter;

    oqgraph_goal(const Vertex& goal, const P& p,
                 stack_cursor *cursor)
      : m_goal(goal), m_cursor(*cursor), m_p(p)
    { assert(cursor); }

    template<class T, class Graph>
    void operator()(T u, Graph &g)
    {
      if (u == m_goal)
      {
        int seq= 0;

        for (Vertex q, v= u;; v = q, seq++)
          if ((q= m_p[ v ]) == v)
            break;

        for (Vertex v= u;; u= v)
        {
          optional<Edge> edge;
          optional<EdgeWeight> weight;
          v= m_p[ u ];
          if (record_weight && u != v)
          {
            typename graph_traits<Graph>::out_edge_iterator ei, ei_end;
            for (boost::tuples::tie(ei, ei_end)= out_edges(v, g); ei != ei_end; ++ei)
            {
              if (target(*ei, g) == u)
              {
                edge= *ei;
                weight= get(boost::edge_weight, g, *ei);
                break;
              }
            }
          }
          else if (u != v)
            weight= 1;
          m_cursor.results.push(reference(seq--, u, edge, weight));
          if (u == v)
            break;
        }
        throw this;
      }
    }

  private:
    Vertex m_goal;
    stack_cursor &m_cursor;
    P m_p;
  };

  template<bool record_weight, typename goal_filter, typename P>
  oqgraph_goal<record_weight, goal_filter, P>
    make_oqgraph_goal(const Vertex& goal, const P& p, stack_cursor *cursor)
  { return oqgraph_goal<record_weight, goal_filter, P>(goal, p, cursor); }

}

namespace open_query
{
  inline oqgraph::oqgraph(oqgraph_share *arg) throw()
    : share(arg), cursor(0), lastRetainedLatch(NULL)
  { }

  inline oqgraph::~oqgraph() throw()
  {
    std::free(lastRetainedLatch);
    delete cursor;
  }

  unsigned oqgraph::edges_count() const throw()
  {
    return num_edges(share->g);
  }

  unsigned oqgraph::vertices_count() const throw()
  {
    return num_vertices(share->g);
  }

  THD* oqgraph::get_thd() { return share->g.get_table_thd(); }
  void oqgraph::set_thd(THD* thd) { share->g.set_table_thd(thd); }

  oqgraph* oqgraph::create(oqgraph_share *share) throw()
  {
    assert(share != NULL);
    return new (std::nothrow) oqgraph(share);
  }

  oqgraph_share* oqgraph::create(
      TABLE* table,
      Field* origid,
      Field* destid,
      Field* weight) throw()
  {
    return new (std::nothrow) oqgraph_share(
        table, origid, destid, weight);
  }

  optional<Edge>
  oqgraph_share::find_edge(Vertex orig, Vertex dest) const
  {
    if (in_degree(dest, g) >= out_degree(orig, g))
    {
      graph_traits<Graph>::out_edge_iterator ei, ei_end;
      tie(ei, ei_end)= out_edges(orig, g);
      if ((ei= std::find_if(ei, ei_end, target_equals(dest, g))) != ei_end)
        return *ei;
    }
    else
    {
      graph_traits<Graph>::in_edge_iterator ei, ei_end;
      tie(ei, ei_end)= in_edges(dest, g);
      if ((ei= std::find_if(ei, ei_end, source_equals(orig, g))) != ei_end)
        return *ei;
    }
    return optional<Edge>();
  }

  optional<Vertex>
  oqgraph_share::find_vertex(VertexID id) const
  {
    return oqgraph3::find_vertex(id, g);
  }

#if 0
  int oqgraph::delete_all() throw()
  {
    share->g.clear();
    return 0;
  }

  int oqgraph::insert_edge(
      VertexID orig_id, VertexID dest_id, EdgeWeight weight, bool replace) throw()
  {
    optional<Vertex> orig, dest;
    optional<Edge> edge;
    bool inserted= 0;

    if (weight < 0)
      return INVALID_WEIGHT;
    if (!(orig= share->find_vertex(orig_id)))
    {
      try
      {
        orig= add_vertex(VertexInfo(orig_id), share->g);
        if (orig == graph_traits<Graph>::null_vertex())
          return CANNOT_ADD_VERTEX;
      }
      catch (...)
      {
        return CANNOT_ADD_VERTEX;
      }
    }
    if (!(dest= share->find_vertex(dest_id)))
    {
      try
      {
        dest= add_vertex(VertexInfo(dest_id), share->g);
        if (dest == graph_traits<Graph>::null_vertex())
          return CANNOT_ADD_VERTEX;
      }
      catch (...)
      {
        return CANNOT_ADD_VERTEX;
      }
    }
    if (!(edge= share->find_edge(*orig, *dest)))
    {
      try
      {
        tie(edge, inserted)= add_edge(*orig, *dest, share->g);
        if (!inserted)
          return CANNOT_ADD_EDGE;
      }
      catch (...)
      {
        return CANNOT_ADD_EDGE;
      }
    }
    else
    {
      if (!replace)
        return DUPLICATE_EDGE;
    }
    share->weightmap[*edge]= weight;
    return OK;
  }

  int oqgraph::delete_edge(current_row_st) throw()
  {
    reference ref;
    if (cursor)
      return EDGE_NOT_FOUND;
    cursor->current(ref);
    optional<Edge> edge;
    if (!(edge= ref.edge()))
      return EDGE_NOT_FOUND;
    Vertex orig= source(*edge, share->g);
    Vertex dest= target(*edge, share->g);
    remove_edge(*edge, share->g);
    if (!degree(orig, share->g))
      remove_vertex(orig, share->g);
    if (!degree(dest, share->g))
      remove_vertex(dest, share->g);
    return OK;
  }

  int oqgraph::modify_edge(current_row_st,
      VertexID *orig_id, VertexID *dest_id, EdgeWeight *weight,
      bool replace) throw()
  {
    if (!cursor)
      return EDGE_NOT_FOUND;
    reference ref;
    cursor->current(ref);
    optional<Edge> edge;
    if (!(edge= ref.edge()))
      return EDGE_NOT_FOUND;
    if (weight && *weight < 0)
      return INVALID_WEIGHT;

    optional<Vertex> orig= source(*edge, share->g),
                     dest= target(*edge, share->g);

    bool orig_neq= orig_id ? get(boost::vertex_index, share->g, *orig) != *orig_id : 0;
    bool dest_neq= dest_id ? get(boost::vertex_index, share->g, *dest) != *dest_id : 0;

    if (orig_neq || dest_neq)
    {
      optional<Edge> new_edge;
      if (orig_neq && !(orig= share->find_vertex(*orig_id)))
      {
        try
        {
          orig= add_vertex(VertexInfo(*orig_id), share->g);
          if (orig == graph_traits<Graph>::null_vertex())
            return CANNOT_ADD_VERTEX;
        }
        catch (...)
        {
          return CANNOT_ADD_VERTEX;
        }
      }
      if (dest_neq && !(dest= share->find_vertex(*dest_id)))
      {
        try
        {
          dest= add_vertex(VertexInfo(*dest_id), share->g);
          if (dest == graph_traits<Graph>::null_vertex())
            return CANNOT_ADD_VERTEX;
        }
        catch (...)
        {
          return CANNOT_ADD_VERTEX;
        }
      }
      if (!(new_edge= share->find_edge(*orig, *dest)))
      {
        try
        {
          bool inserted;
          tie(new_edge, inserted)= add_edge(*orig, *dest, share->g);
          if (!inserted)
            return CANNOT_ADD_EDGE;
        }
        catch (...)
        {
          return CANNOT_ADD_EDGE;
        }
      }
      else
      {
        if (!replace)
          return DUPLICATE_EDGE;
      }
      share->weightmap[*new_edge]= share->weightmap[*edge];
      remove_edge(*edge, share->g);
      edge= new_edge;
    }
    if (weight)
      share->weightmap[*edge]= *weight;
    return OK;
  }

  int oqgraph::modify_edge(
      VertexID orig_id, VertexID dest_id, EdgeWeight weight) throw()
  {
    optional<Vertex> orig, dest;
    optional<Edge> edge;

    if (weight < 0)
      return INVALID_WEIGHT;
    if (!(orig= share->find_vertex(orig_id)))
      return EDGE_NOT_FOUND;
    if (!(dest= share->find_vertex(dest_id)))
      return EDGE_NOT_FOUND;
    if (!(edge= share->find_edge(*orig, *dest)))
      return EDGE_NOT_FOUND;
    share->weightmap[*edge]= weight;
    return OK;
  }

  int oqgraph::delete_edge(VertexID orig_id, VertexID dest_id) throw()
  {
    optional<Vertex> orig, dest;
    optional<Edge> edge;

    if (!(orig= share->find_vertex(orig_id)))
      return EDGE_NOT_FOUND;
    if (!(dest= share->find_vertex(dest_id)))
      return EDGE_NOT_FOUND;
    if (!(edge= share->find_edge(*orig, *dest)))
      return EDGE_NOT_FOUND;
    remove_edge(*edge, share->g);
    if (!degree(*orig, share->g))
      remove_vertex(*orig, share->g);
    if (!degree(*dest, share->g))
      remove_vertex(*dest, share->g);
    return OK;
  }
#endif

  // THIS IS UGLY - refactor later
  // Update the retained latch string value, for later retrieval by
  // fetch_row() as a workaround for making sure we return the correct
  // string to match the latch='' clause
  // (This is a hack for mariadb mysql compatibility)
  // IT SHOULD ONLY BE CALLED IMMEIDATELY BEFORE search)(
  void oqgraph::retainLatchFieldValue(const char *retainedLatch) 
  {
    // attempting to use std::string broke lots of stuff
    // Probably more efficient to use mysql String class, FIXME later
    if (lastRetainedLatch) { std::free(lastRetainedLatch); lastRetainedLatch = NULL; }
    if (retainedLatch) { lastRetainedLatch = strdup(retainedLatch); }
  }

  // Because otherwise things can happen and we havent freed a resource since the end of the last query...
  void oqgraph::release_cursor() throw() {
    if (share->g._cursor) {
      // Make sure refs all freed before deleting share->g._cursor
      share->g._rnd_cursor = 0;      
      delete cursor; cursor = 0;
      delete share->g._cursor;
      share->g._cursor = NULL;
    }
    row_info= empty_row;
  }


  int oqgraph::search(int *latch, VertexID *orig_id, VertexID *dest_id) throw()
  {
      optional<Vertex> orig, dest;
      int op= 0, seq= 0;

      delete cursor; cursor= 0;
      row_info= empty_row;
      if ((row_info.latch_indicator= latch)) {
        op= ALGORITHM & (row_info.latch= *latch);
        row_info.latchStringValue = lastRetainedLatch;
        row_info.latchStringValueLen = strlen(lastRetainedLatch);
      }
      if ((row_info.orig_indicator= orig_id) && (op|= HAVE_ORIG))
        orig= share->find_vertex((row_info.orig= *orig_id));
      if ((row_info.dest_indicator= dest_id) && (op|= HAVE_DEST))
        dest= share->find_vertex((row_info.dest= *dest_id));
    //try
    //{
      switch (op)
      {
      case NO_SEARCH | HAVE_ORIG | HAVE_DEST:
      case NO_SEARCH | HAVE_ORIG:
        if ((cursor= new (std::nothrow) stack_cursor(share)) && orig)
        {
          graph_traits<Graph>::out_edge_iterator ei, ei_end;
          for (boost::tuples::tie(ei, ei_end)= out_edges(*orig, share->g); ei != ei_end; ++ei)
          {
            Vertex v= target(*ei, share->g);
            static_cast<stack_cursor*>(cursor)->
                results.push(reference(++seq, v, *ei,
                    get(boost::edge_weight, share->g, *ei)));
          }
        }
        /* fall through */
      case NO_SEARCH | HAVE_DEST:
        if ((op & HAVE_DEST) &&
            (cursor || (cursor= new (std::nothrow) stack_cursor(share))) &&
	    dest)
        {
          graph_traits<Graph>::in_edge_iterator ei, ei_end;
          for (boost::tuples::tie(ei, ei_end)= in_edges(*dest, share->g); ei != ei_end; ++ei)
          {
            Vertex v= source(*ei, share->g);
            static_cast<stack_cursor*>(cursor)->
                results.push(reference(++seq, v, *ei,
                    get(boost::edge_weight, share->g, *ei)));
          }
        }
        break;

      case NO_SEARCH:
        cursor= new (std::nothrow) vertices_cursor(share);
        break;

      case DIJKSTRAS | HAVE_ORIG | HAVE_DEST:
        if ((cursor= new (std::nothrow) stack_cursor(share)) && orig && dest)
        {
          boost::unordered_map<Vertex, Vertex> p;
          boost::unordered_map<Vertex, EdgeWeight> d;
          p[ *orig ]= *orig;
          d[ *orig ] = EdgeWeight();
          try
          {
            dijkstra_shortest_paths_no_init(share->g, *orig,
                make_lazy_property_map(p, identity_initializer<Vertex>()),
                make_lazy_property_map(d, value_initializer<EdgeWeight>(
                    (std::numeric_limits<EdgeWeight>::max)())),
                get(edge_weight, share->g),
                get(vertex_index, share->g),
                std::less<EdgeWeight>(),
                closed_plus<EdgeWeight>(),
                EdgeWeight(),
                make_dijkstra_visitor(
                    make_oqgraph_goal<true, on_finish_vertex>(
                        *dest,
                        boost::make_assoc_property_map(p),
                        static_cast<stack_cursor*>(cursor)
                    )
                ),
                make_two_bit_judy_map(get(vertex_index, share->g)));
          }
          catch (...)
          { /* printf("found\n"); */ }
        }
        break;

      case BREADTH_FIRST | HAVE_ORIG | HAVE_DEST:
        if ((cursor= new (std::nothrow) stack_cursor(share)) && orig && dest)
        {
          boost::unordered_map<Vertex, Vertex> p;
          boost::queue<Vertex> Q;
          p[ *orig ]= *orig;
          try
          {
            breadth_first_visit(share->g, *orig, Q,
                make_bfs_visitor(
                    std::make_pair(
                        record_predecessors(
                            boost::make_assoc_property_map(p),
                            on_tree_edge()
                        ),
                        make_oqgraph_goal<false, on_discover_vertex>(
                            *dest, boost::make_assoc_property_map(p),
                            static_cast<stack_cursor*>(cursor)
                        )
                    )
                ),
                make_two_bit_judy_map(get(vertex_index, share->g)));
          }
          catch (...)
          { /* printf("found\n"); */ }
        }
        break;

      case DIJKSTRAS | HAVE_ORIG:
      case BREADTH_FIRST | HAVE_ORIG:
      case LEAVES | HAVE_ORIG:
        if ((cursor= new (std::nothrow) stack_cursor(share)) && (orig || dest))
        {
          boost::unordered_map<Vertex, Vertex> p;
          boost::unordered_map<Vertex, EdgeWeight> d;
          boost::queue<Vertex> Q;
          p[ *orig ]= *orig;
          d[ *orig ] = EdgeWeight();
          switch (ALGORITHM & op)
          {
          case DIJKSTRAS:
            dijkstra_shortest_paths_no_init(share->g, *orig,
                make_lazy_property_map(p, identity_initializer<Vertex>()),
                make_lazy_property_map(d, value_initializer<EdgeWeight>(
                    (std::numeric_limits<EdgeWeight>::max)())),
                get(edge_weight, share->g),
                get(vertex_index, share->g),
                std::less<EdgeWeight>(),
                closed_plus<EdgeWeight>(),
                EdgeWeight(),
                make_dijkstra_visitor(
                    make_oqgraph_visit_dist(
                        boost::make_assoc_property_map(p), 
                        boost::make_assoc_property_map(d),
                        static_cast<stack_cursor*>(cursor)
                    )
                ),
                make_two_bit_judy_map(get(vertex_index, share->g)));
            break;
          case BREADTH_FIRST:
            breadth_first_visit(share->g, *orig, Q,
                make_bfs_visitor(
                    std::make_pair(
                        record_predecessors(
                            boost::make_assoc_property_map(p),
                            on_tree_edge()
                        ),
                    std::make_pair(
                        record_distances(
                            boost::make_assoc_property_map(d),
                            on_tree_edge()
                        ),
                        make_oqgraph_visit_dist(
                            boost::make_assoc_property_map(p),
                            boost::make_assoc_property_map(d),
                            static_cast<stack_cursor*>(cursor)
                        )
                    ))
                ),
                make_two_bit_judy_map(get(vertex_index, share->g)));
            break;
          case LEAVES:
            breadth_first_visit(share->g, *orig, Q,
                make_bfs_visitor(
                    std::make_pair(
                        record_predecessors(
                            boost::make_assoc_property_map(p),
                            on_tree_edge()
                        ),
                    std::make_pair(
                        record_distances(
                            boost::make_assoc_property_map(d),
                            on_tree_edge()
                        ),
                        make_oqgraph_visit_leaves(
                            boost::make_assoc_property_map(p),
                            boost::make_assoc_property_map(d),
                            static_cast<stack_cursor*>(cursor)
                        )
                    ))
                ),
                make_two_bit_judy_map(get(vertex_index, share->g)));
            break;
          default:
            abort();
          }
        }
        break;
      case BREADTH_FIRST | HAVE_DEST:
      case DIJKSTRAS | HAVE_DEST:
      case LEAVES | HAVE_DEST:
        if ((cursor= new (std::nothrow) stack_cursor(share)) && (orig || dest))
        {
          boost::unordered_map<Vertex, Vertex> p;
          boost::unordered_map<Vertex, EdgeWeight> d;
          boost::queue<Vertex> Q;
          const reverse_graph<Graph> r(share->g);
          p[ *dest ]= *dest;
          d[ *dest ] = EdgeWeight();
          switch (ALGORITHM & op)
          {
          case DIJKSTRAS:
            dijkstra_shortest_paths_no_init(r, *dest,
                make_lazy_property_map(p, identity_initializer<Vertex>()),
                make_lazy_property_map(d, value_initializer<EdgeWeight>(
                    (std::numeric_limits<EdgeWeight>::max)())),
                get(edge_weight, r),
                get(vertex_index, r),
                std::less<EdgeWeight>(),
                closed_plus<EdgeWeight>(),
                EdgeWeight(),
                make_dijkstra_visitor(
                        make_oqgraph_visit_dist(
                            boost::make_assoc_property_map(p),
                            boost::make_assoc_property_map(d),
                            static_cast<stack_cursor*>(cursor)
                        )
                ),
                make_two_bit_judy_map(get(vertex_index, r)));
            break;
          case BREADTH_FIRST:
            breadth_first_visit(r, *dest, Q,
                make_bfs_visitor(
                    std::make_pair(
                        record_predecessors(
                            boost::make_assoc_property_map(p),
                            on_tree_edge()
                        ),
                    std::make_pair(
                        record_distances(
                            boost::make_assoc_property_map(d),
                            on_tree_edge()
                        ),
                        make_oqgraph_visit_dist(
                            boost::make_assoc_property_map(p),
                            boost::make_assoc_property_map(d),
                            static_cast<stack_cursor*>(cursor)
                        )
                    ))
                ),
                make_two_bit_judy_map(get(vertex_index, r)));
            break;
          case LEAVES:
            breadth_first_visit(r, *dest, Q,
                make_bfs_visitor(
                    std::make_pair(
                        record_predecessors(
                            boost::make_assoc_property_map(p),
                            on_tree_edge()
                        ),
                    std::make_pair(
                        record_distances(
                            boost::make_assoc_property_map(d),
                            on_tree_edge()
                        ),
                        make_oqgraph_visit_leaves(
                            boost::make_assoc_property_map(p),
                            boost::make_assoc_property_map(d),
                            static_cast<stack_cursor*>(cursor)
                        )
                    ))
                ),
                make_two_bit_judy_map(get(vertex_index, r)));
            break;
          default:
            abort();
          }
        }
        break;
      default:
        break;
      }
      return 0;
    //}
    //catch (...)
    //{
    //  return MISC_FAIL;
    //}
  }

  int oqgraph::fetch_row(row& result) throw()
  {
    if (!cursor)
      return NO_MORE_DATA;
    return cursor->fetch_row(row_info, result);
  }

  int oqgraph::fetch_row(row& result, const void* ref_ptr) throw()
  {
    const reference &ref= *(const reference*) ref_ptr;
    if (!cursor)
      return NO_MORE_DATA;
    return cursor->fetch_row(row_info, result, ref);
  }

  void oqgraph::row_ref(void *ref_ptr) throw()
  {
    reference &ref= *(reference*) ref_ptr;
    if (cursor)
      cursor->current(ref);
    else
      // Beware: internally this eventually causes a swap by intrusive_ptr, so ref must be initialised to sane on all cases
      ref = reference(); 
  }

  void oqgraph::init_row_ref(void *ref_ptr) throw()
  {
    // Placement new will cause a constructor to be called avoiding the assignment operator of intrusive_ptr
    // This doesnt allocate any memory, assumes ref_ptr is the correct size(!)
    new (ref_ptr) reference(); 
  }

  int oqgraph::random(bool scan) throw()
  {
    if (scan || !cursor)
    {
      delete cursor; cursor= 0;
      if (!(cursor= new (std::nothrow) edges_cursor(share)))
        return MISC_FAIL;
    }
    row_info= empty_row;
    return OK;
  }

  void oqgraph::free(oqgraph *graph) throw()
  {
    delete graph;
  }

  void oqgraph::free(oqgraph_share *graph) throw()
  {
    delete graph;
  }

  const size_t oqgraph::sizeof_ref= sizeof(reference);
}

int stack_cursor::fetch_row(const row &row_info, row &result)
{
  if (!results.empty())
  {
    if (int res= fetch_row(row_info, result, results.top()))
      return res;
    results.pop();
    return oqgraph::OK;
  }
  else
  {
    last= reference();
    return oqgraph::NO_MORE_DATA;
  }
}

int stack_cursor::fetch_row(const row &row_info, row &result,
                            const reference &ref)
{
  last= ref;
  if (last.vertex())
  {
    optional<int> seq;
    optional<EdgeWeight> w;
    optional<Vertex> v;
    result= row_info;
    if ((result.seq_indicator= static_cast<bool>(seq= last.sequence())))
      result.seq= *seq;
    if ((result.link_indicator= static_cast<bool>(v= last.vertex())))
      result.link= get(boost::vertex_index, share->g, *v);
    if ((result.weight_indicator= static_cast<bool>(w= last.weight())))
      result.weight= *w;
    return oqgraph::OK;
  }
  else
    return oqgraph::NO_MORE_DATA;
}


int vertices_cursor::fetch_row(const row &row_info, row &result)
{
  vertex_iterator it, end;
  reference ref;
  size_t count= position;
  for (tie(it, end)= vertices(share->g); count && it != end; ++it, --count)
    ;
  if (it != end)
    ref= reference(position+1, *it);
  if (int res= fetch_row(row_info, result, ref))
    return res;
  position++;
  return oqgraph::OK;
}

int vertices_cursor::fetch_row(const row &row_info, row &result,
                               const reference &ref)
{
  last= ref;
  optional<Vertex> v= last.vertex();
  result= row_info;
  if (v)
  {
    result.link_indicator= 1;
    result.link= get(boost::vertex_index, share->g, *v);
#ifdef DISPLAY_VERTEX_INFO
    result.seq_indicator= 1;
    if ((result.seq= degree(*v, share->g)))
    {
      EdgeWeight weight= 0;
      graph_traits<Graph>::in_edge_iterator iei, iei_end;
      for (tie(iei, iei_end)= in_edges(*v, share->g); iei != iei_end; ++iei)
        weight+= get(boost::edge_weight, share->g, *iei);
      graph_traits<Graph>::out_edge_iterator oei, oei_end;
      for (tie(oei, oei_end)= out_edges(*v, share->g); oei != oei_end; ++oei)
        weight+= get(boost::edge_weight, share->g, *oei);
      result.weight_indicator= 1;
      result.weight= weight / result.seq;
    }
#endif
    return oqgraph::OK;
  }
  else
    return oqgraph::NO_MORE_DATA;
}

int edges_cursor::fetch_row(const row &row_info, row &result)
{
  edge_iterator it, end;
  reference ref;
  tie(it, end)= edges(share->g);
  it+= position;
  if (it != end)
    ref= reference(position+1, *it);
  if (int res= fetch_row(row_info, result, ref))
    return res;
  ++position;
  return oqgraph::OK;
}

int edges_cursor::fetch_row(const row &row_info, row &result,
                            const reference &ref)
{
  optional<Edge> edge;
  if ((edge= (last= ref).edge()))
  {
    result= row_info;
    result.orig_indicator= result.dest_indicator= result.weight_indicator= 1;

    oqgraph3::vertex_id orig = get(boost::vertex_index, share->g, source( *edge, share->g ) );
    oqgraph3::vertex_id dest = get(boost::vertex_index, share->g, target( *edge, share->g ) );

    // bug 796647c - may be symptomatic of a bigger problem with representation
    // but origid and destid can be -1 indicating no such record, NULL? but oqgraph3::vertex_id
    // seems to resolve to VertexID (unsigned) in row
    // in any case we should check for errors (-1) in origid... because all edges have at least one vertex by definition
    if (orig == (oqgraph3::vertex_id)-1 && dest == (oqgraph3::vertex_id)-1) {
      // Select * from graph; -- when backing store is empty (bug MDEV-5891)
      return oqgraph::NO_MORE_DATA;
    }
    //    assert( ! ((size_t)orig == (size_t)-1 && (size_t)dest == (size_t)-1)); 
    // indicates we havent handle a HA_ERR_RECORD_DELETED somewhere

    result.orig= orig;
    result.dest= dest;
    result.weight= get(boost::edge_weight, share->g, *edge);
    return oqgraph::OK;
  }
  return oqgraph::NO_MORE_DATA;
}

namespace boost {
  GRAPHCORE_INTERNAL void throw_exception(std::exception const&)
  {
    abort();
  }
}

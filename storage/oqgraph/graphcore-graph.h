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

#ifndef oq_graphcore_graph_h_
#define oq_graphcore_graph_h_

#include "oqgraph_shim.h"

#include <boost/graph/two_bit_color_map.hpp>

namespace boost
{
  typedef oqgraph3::graph Graph;

  template<typename IndexMap = identity_property_map>
  struct two_bit_judy_map
  {
    typedef typename property_traits<IndexMap>::key_type key_type;
    typedef two_bit_color_type value_type;
    typedef void reference;
    typedef read_write_property_map_tag category;

    open_query::judy_bitset msb;
    open_query::judy_bitset lsb;
    IndexMap index;

    two_bit_judy_map(const IndexMap& i)
      : index(i)
    { }

    friend two_bit_color_type get(
        const two_bit_judy_map<IndexMap>& pm,
        typename property_traits<IndexMap>::key_type key)
    {
      typename property_traits<IndexMap>::value_type i = get(pm.index, key);
      return two_bit_color_type((2*int(pm.msb.test(i))) | int(pm.lsb.test(i)));
    }

    friend void put(
        two_bit_judy_map<IndexMap>& pm,
        typename property_traits<IndexMap>::key_type key,
        two_bit_color_type value)
    {
      typename property_traits<IndexMap>::value_type i = get(pm.index, key);
      pm.msb.set(i, value & 2);
      pm.lsb.set(i, value & 1);
    }
  };

  template<typename IndexMap>
  inline two_bit_judy_map<IndexMap>
  make_two_bit_judy_map(const IndexMap& index)
  {
    return two_bit_judy_map<IndexMap>(index);
  }


  template <typename Type>
  struct default_lazy_initializer
  {
    template <typename Key>
    Type operator()(const Key&) const { return Type(); }
  };

  template <typename Type>
  struct copy_initializer
  {
    copy_initializer(const Type& value) : _(value) { }
    template <typename Key>
    const Type& operator()(const Key&) const { return _; }
    const Type& _;
  };

  template <typename Type>
  copy_initializer<Type> make_copy_initializer(const Type& value)
  { return copy_initializer<Type>(value); }


  template <typename Type>
  struct value_initializer
  {
    value_initializer(const Type& value) : _(value) { }
    template <typename Key>
    const Type& operator()(const Key&) const { return _; }
    const Type _;
  };

  template <typename Type>
  value_initializer<Type> make_value_initializer(const Type& value)
  { return value_initializer<Type>(value); }


  template <typename Key>
  struct identity_initializer
  {
    const Key& operator()(const Key& _) const { return _; }
  };

  template <class Container, class Generator>
  struct lazy_property_map
  {
    typedef lazy_property_map<Container, Generator> self;
    typedef typename Container::key_type key_type;
    typedef typename Container::value_type::second_type value_type;
    typedef value_type& reference;
    typedef lvalue_property_map_tag category;

    lazy_property_map(Container& m, Generator g= Generator())
      : _m(m)
      , _g(g)
    { }

    reference operator[](const key_type& k) const
    {
      typename Container::iterator found= _m.find(k);
      if (_m.end() == found)
      {
        found= _m.insert(std::make_pair(k, _g(k))).first;
      }
      return found->second;
    }

    void set(const key_type& k, const value_type& v)
    { _m[k] = v; }

    friend reference get(const self& s, const key_type& k)
    {
      return s[k];
    }

    friend void put(self& s, const key_type& k, const value_type& v)
    { s.set(k, v); }

    Container& _m;
    Generator _g;
  };

  template <class Container, class Generator>
  lazy_property_map<Container, Generator>
  make_lazy_property_map(Container& c, Generator g)
  { return lazy_property_map<Container, Generator>(c, g); }

}

#endif

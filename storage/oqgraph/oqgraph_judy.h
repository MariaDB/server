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

#include <cstddef>

namespace open_query
{

  class judy_bitset
  {
  public:
    typedef std::size_t size_type;
    enum { npos = (size_type) -1 };

    judy_bitset()
      : array(0)
    { }

    judy_bitset(const judy_bitset& src)
      : array(0)
    {
      set(src);
    }

    ~judy_bitset()
    { clear(); }

    judy_bitset& operator=(const judy_bitset& src)
    {
      clear();
      return set(src);
    }

    void clear();
    bool empty() const { return !array; }
    bool none() const { return npos == find_first(); }

    inline judy_bitset& set(size_type n, bool val = true)
    {
      if (!val)
        return reset(n);
      else
        return setbit(n);
    }

    judy_bitset& set(const judy_bitset& src);

    judy_bitset& reset(size_type n);
    judy_bitset& flip(size_type n);
    bool test(size_type) const;
    size_type count() const;
    size_type size() const;
    size_type num_blocks() const;

    class reference
    {
      friend class judy_bitset;
      reference(judy_bitset& array, size_type pos)
        : j(array)
        , n(pos)
      { }
      void operator&(); // not defined
    public:
      reference& operator=(bool value)
      { j.set(n, value); return *this; }
      reference& operator=(const reference& ref)
      { j.set(n, ref); return *this; }

      reference& operator|=(bool value)
      { if (value) j.set(n); return *this; }
      reference& operator&=(bool value)
      { if (!value) j.reset(n); return *this; }
      reference& operator^=(bool value)
      { if (value) j.flip(n); return *this; }
      reference& operator-=(bool value)
      { if (value) j.reset(n); return *this; }

      bool operator~() const { return !j.test(n); }
      operator bool() const { return j.test(n); }
      reference& flip() { j.flip(n); return *this; }

    private:
      judy_bitset& j;
      size_type n;
    };

    reference operator[](size_type n) { return reference(*this, n); }
    bool operator[](size_type n) const { return test(n); }

    size_type find_first() const;
    size_type find_next(size_type n) const;
  private:
    mutable void* array;

    judy_bitset& setbit(size_type n);
  };
}


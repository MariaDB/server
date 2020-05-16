#ifndef MY_COUNTER_H_INCLUDED
#define MY_COUNTER_H_INCLUDED
/*
   Copyright (C) 2018, 2020, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <atomic>


template <typename Type> class Atomic_counter
{
  std::atomic<Type> m_counter;

  Type add(Type i) { return m_counter.fetch_add(i, std::memory_order_relaxed); }
  Type sub(Type i) { return m_counter.fetch_sub(i, std::memory_order_relaxed); }

public:
  Atomic_counter(const Atomic_counter<Type> &rhs)
  { m_counter.store(rhs, std::memory_order_relaxed); }
  Atomic_counter(Type val): m_counter(val) {}
  Atomic_counter() {}

  Type operator++(int) { return add(1); }
  Type operator--(int) { return sub(1); }

  Type operator++() { return add(1) + 1; }
  Type operator--() { return sub(1) - 1; }

  Type operator+=(const Type i) { return add(i) + i; }
  Type operator-=(const Type i) { return sub(i) - i; }

  operator Type() const { return m_counter.load(std::memory_order_relaxed); }
  Type operator=(const Type val)
  { m_counter.store(val, std::memory_order_relaxed); return val; }
  Type operator=(const Atomic_counter<Type> &rhs) { return *this= Type{rhs}; }

  Type fetch_add(const Type i, std::memory_order m)
  { return m_counter.fetch_add(i, m); }
  Type fetch_sub(const Type i, std::memory_order m)
  { return m_counter.fetch_sub(i, m); }
  bool compare_exchange_strong(Type& i1, const Type i2,
                               std::memory_order m1, std::memory_order m2)
  { return m_counter.compare_exchange_strong(i1, i2, m1, m2); }
  Type exchange(const Type i, std::memory_order m)
  { return m_counter.exchange(i, m); }
};
#endif /* MY_COUNTER_H_INCLUDED */

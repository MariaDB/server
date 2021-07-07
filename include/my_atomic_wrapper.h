/* Copyright (c) 2020, 2021, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#pragma once
#ifdef __cplusplus
#include <atomic>
/**
  A wrapper for std::atomic, defaulting to std::memory_order_relaxed.

  When it comes to atomic loads or stores at std::memory_order_relaxed
  on IA-32 or AMD64, this wrapper is only introducing some constraints
  to the C++ compiler, to prevent some optimizations of loads or
  stores.

  On POWER and ARM, atomic loads and stores involve different instructions
  from normal loads and stores and will thus incur some overhead.

  Because atomic read-modify-write operations will always incur
  overhead, we intentionally do not define
  operator++(), operator--(), operator+=(), operator-=(), or similar,
  to make the overhead stand out in the users of this code.
*/
template <typename Type> class Atomic_relaxed
{
  std::atomic<Type> m;
public:
  Atomic_relaxed(const Atomic_relaxed<Type> &rhs)
  { m.store(rhs, std::memory_order_relaxed); }
  Atomic_relaxed(Type val) : m(val) {}
  Atomic_relaxed() {}

  Type load(std::memory_order o= std::memory_order_relaxed) const
  { return m.load(o); }
  void store(Type i, std::memory_order o= std::memory_order_relaxed)
  { m.store(i, o); }
  operator Type() const { return m.load(); }
  Type operator=(const Type i) { store(i); return i; }
  Type operator=(const Atomic_relaxed<Type> &rhs) { return *this= Type{rhs}; }
  Type operator+=(const Type i) { return fetch_add(i); }
  Type fetch_add(const Type i, std::memory_order o= std::memory_order_relaxed)
  { return m.fetch_add(i, o); }
  Type fetch_sub(const Type i, std::memory_order o= std::memory_order_relaxed)
  { return m.fetch_sub(i, o); }
  Type fetch_xor(const Type i, std::memory_order o= std::memory_order_relaxed)
  { return m.fetch_xor(i, o); }
  Type fetch_and(const Type i, std::memory_order o= std::memory_order_relaxed)
  { return m.fetch_and(i, o); }
  Type fetch_or(const Type i, std::memory_order o= std::memory_order_relaxed)
  { return m.fetch_or(i, o); }
  bool compare_exchange_strong(Type& i1, const Type i2,
                               std::memory_order o1= std::memory_order_relaxed,
                               std::memory_order o2= std::memory_order_relaxed)
  { return m.compare_exchange_strong(i1, i2, o1, o2); }
  Type exchange(const Type i, std::memory_order o= std::memory_order_relaxed)
  { return m.exchange(i, o); }
};
#endif /* __cplusplus */

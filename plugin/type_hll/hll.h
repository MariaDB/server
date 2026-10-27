#ifndef HLL_INCLUDED
#define HLL_INCLUDED
/* Copyright (c) 2026, drrtuy@gmail.com
   Copyright (c) 2026, MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/*
  Version-independent HyperLogLog state and algorithm.
  No dependencies on THD/Item/Field so it can be unit tested standalone.
*/

#include <my_global.h>

static constexpr uint HLL_P= 14;
static constexpr uint HLL_M= 1U << HLL_P;
static constexpr uint HLL_EXPLICIT_MAX= 256;
static constexpr uint8 HLL_MAX_RANK= 64 - HLL_P + 1;   // 51

class Hll_state
{
public:
  enum Repr { EMPTY= 1, EXPLICIT= 2, FULL= 3 };

  Repr repr;
  uint n_explicit;                       // valid when repr == EXPLICIT
  uint64 explicit_hashes[HLL_EXPLICIT_MAX];  // sorted, unique
  uint8 regs[HLL_M];                     // valid when repr == FULL

  Hll_state() { clear(); }
  void clear() { repr= EMPTY; n_explicit= 0; }
  void add(uint64 hash);
  void merge(const Hll_state &other);
  double cardinality() const;

private:
  void add_to_registers(uint64 hash);
  void promote_to_full();
};

#endif // HLL_INCLUDED

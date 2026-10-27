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

#include "hll.h"
#include <my_bit.h>
#include <algorithm>
#include <cmath>


void Hll_state::add_to_registers(uint64 hash)
{
  uint index= (uint) (hash & (HLL_M - 1));
  uint64 tail= hash >> HLL_P;
  uint8 rank= tail ? (uint8) (my_find_first_bit(tail) + 1) : HLL_MAX_RANK;
  regs[index]= std::max(regs[index], rank);
}


void Hll_state::promote_to_full()
{
  DBUG_ASSERT(repr == EXPLICIT);
  memset(regs, 0, sizeof(regs));
  for (uint i= 0; i < n_explicit; i++)
    add_to_registers(explicit_hashes[i]);
  n_explicit= 0;
  repr= FULL;
}


void Hll_state::add(uint64 hash)
{
  switch (repr) {
  case EMPTY:
    explicit_hashes[0]= hash;
    n_explicit= 1;
    repr= EXPLICIT;
    return;
  case EXPLICIT:
  {
    uint64 *end= explicit_hashes + n_explicit;
    uint64 *pos= std::lower_bound(explicit_hashes, end, hash);
    if (pos != end && *pos == hash)
      return;
    if (n_explicit < HLL_EXPLICIT_MAX)
    {
      memmove(pos + 1, pos, (end - pos) * sizeof(uint64));
      *pos= hash;
      n_explicit++;
      return;
    }
    promote_to_full();
    add_to_registers(hash);
    return;
  }
  case FULL:
    add_to_registers(hash);
    return;
  }
}


void Hll_state::merge(const Hll_state &other)
{
  switch (other.repr) {
  case EMPTY:
    return;
  case EXPLICIT:
    for (uint i= 0; i < other.n_explicit; i++)
      add(other.explicit_hashes[i]);
    return;
  case FULL:
    if (repr != FULL)
    {
      if (repr == EMPTY)
      {
        memcpy(regs, other.regs, sizeof(regs));
        repr= FULL;
        return;
      }
      promote_to_full();
    }
    for (uint i= 0; i < HLL_M; i++)
      regs[i]= std::max(regs[i], other.regs[i]);
    return;
  }
}


double Hll_state::cardinality() const
{
  switch (repr) {
  case EMPTY:
    return 0;
  case EXPLICIT:
    return n_explicit;
  case FULL:
    break;
  }
  double sum= 0;
  uint zeros= 0;
  for (uint i= 0; i < HLL_M; i++)
  {
    sum+= std::ldexp(1.0, -(int) regs[i]);
    zeros+= regs[i] == 0;
  }
  const double m= HLL_M;
  const double alpha= 0.7213 / (1 + 1.079 / m);
  double estimate= alpha * m * m / sum;
  if (estimate <= 2.5 * m && zeros)
    return m * std::log(m / zeros);
  return estimate;
}

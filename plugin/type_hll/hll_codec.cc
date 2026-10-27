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

#include "hll_codec.h"


static uint64 read_be64(const uchar *p)
{
  uint64 v= 0;
  for (uint i= 0; i < 8; i++)
    v= (v << 8) | p[i];
  return v;
}


static void write_be64(uchar *p, uint64 v)
{
  for (int i= 7; i >= 0; i--, v>>= 8)
    p[i]= (uchar) v;
}


static uint8 read_register(const uchar *payload, uint i)
{
  uint bit= i * 6;
  uint byte= bit / 8, shift= bit % 8;
  uint window= payload[byte];
  if (shift > 2)               // register spans two bytes
    window|= (uint) payload[byte + 1] << 8;
  return (uint8) ((window >> shift) & 0x3f);
}


bool Hll_codec::decode(const uchar *data, size_t length, Hll_state *state)
{
  if (!length)
    return true;
  uint version= data[0] >> 4, repr= data[0] & 0x0f;
  switch (version) {
  case HLL_FORMAT_V1:
    return decode_v1(repr, data + 1, length - 1, state);
  default:
    return true;
  }
}


bool Hll_codec::decode_v1(uint repr, const uchar *payload, size_t length,
                          Hll_state *state)
{
  switch (repr) {
  case Hll_state::EMPTY:
    if (length)
      return true;
    if (state)
      state->clear();
    return false;

  case Hll_state::EXPLICIT:
  {
    if (!length || length % 8 || length / 8 > HLL_EXPLICIT_MAX)
      return true;
    uint n= (uint) (length / 8);
    uint64 prev= 0;
    for (uint i= 0; i < n; i++)
    {
      uint64 h= read_be64(payload + i * 8);
      if (i && h <= prev)
        return true;
      prev= h;
      if (state)
        state->explicit_hashes[i]= h;
    }
    if (state)
    {
      state->repr= Hll_state::EXPLICIT;
      state->n_explicit= n;
    }
    return false;
  }

  case Hll_state::FULL:
    if (length != HLL_FULL_PAYLOAD_LENGTH)
      return true;
    for (uint i= 0; i < HLL_M; i++)
    {
      uint8 rank= read_register(payload, i);
      if (rank > HLL_MAX_RANK)
        return true;
      if (state)
        state->regs[i]= rank;
    }
    if (state)
    {
      state->repr= Hll_state::FULL;
      state->n_explicit= 0;
    }
    return false;

  default:                       // reserved representation
    return true;
  }
}


size_t Hll_codec::encode(const Hll_state &state, uint8 target_version,
                         uchar *out)
{
  DBUG_ASSERT(target_version == HLL_FORMAT_V1);
  return encode_v1(state, out);
}


size_t Hll_codec::encode_v1(const Hll_state &state, uchar *out)
{
  out[0]= (uchar) ((HLL_FORMAT_V1 << 4) | state.repr);
  switch (state.repr) {
  case Hll_state::EMPTY:
    return 1;
  case Hll_state::EXPLICIT:
    DBUG_ASSERT(state.n_explicit >= 1 && state.n_explicit <= HLL_EXPLICIT_MAX);
    for (uint i= 0; i < state.n_explicit; i++)
      write_be64(out + 1 + i * 8, state.explicit_hashes[i]);
    return 1 + 8 * (size_t) state.n_explicit;
  case Hll_state::FULL:
  {
    uchar *payload= out + 1;
    memset(payload, 0, HLL_FULL_PAYLOAD_LENGTH);
    for (uint i= 0; i < HLL_M; i++)
    {
      uint bit= i * 6;
      uint byte= bit / 8, shift= bit % 8;
      uint v= (uint) state.regs[i] << shift;
      payload[byte]|= (uchar) v;
      if (shift > 2)
        payload[byte + 1]|= (uchar) (v >> 8);
    }
    return HLL_MAX_ENCODED_LENGTH;
  }
  }
  DBUG_ASSERT(0);
  return 0;
}

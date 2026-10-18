#ifndef HLL_CODEC_INCLUDED
#define HLL_CODEC_INCLUDED
/* Copyright (c) 2026, MariaDB

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
  Persisted HLL format. Every path that reads or writes HLL bytes
  goes through Hll_codec.

  Header byte: bits 7..4 format version, bits 3..0 representation.
  v1:
    EMPTY    0x11
    EXPLICIT 0x12 + 1..256 strictly ascending big-endian uint64 hashes
    FULL     0x13 + 16384 6-bit registers packed LSB-first (12288 bytes)
*/

#include "hll.h"

static constexpr uint8 HLL_FORMAT_V1= 1;
static constexpr size_t HLL_FULL_PAYLOAD_LENGTH= HLL_M * 6 / 8;
static constexpr size_t HLL_MAX_ENCODED_LENGTH= 1 + HLL_FULL_PAYLOAD_LENGTH;

class Hll_codec
{
public:
  /*
    Validate and decode a serialized value.
    @param state  decoded state, or NULL to only validate
    @retval true  malformed or unsupported value
  */
  static bool decode(const uchar *data, size_t length, Hll_state *state);
  /*
    @param out    buffer of at least HLL_MAX_ENCODED_LENGTH bytes
    @return       number of bytes written
  */
  static size_t encode(const Hll_state &state, uint8 target_version,
                       uchar *out);

private:
  static bool decode_v1(uint repr, const uchar *payload, size_t length,
                        Hll_state *state);
  static size_t encode_v1(const Hll_state &state, uchar *out);
};

#endif // HLL_CODEC_INCLUDED

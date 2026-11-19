/*
   Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/
#ifndef SQL_MULTI_KEY_BATCH_INCLUDED
#define SQL_MULTI_KEY_BATCH_INCLUDED

class Multi_key_params;
typedef struct st_handler_buffer HANDLER_BUFFER;

class Multi_key_batch
{
public:
  explicit Multi_key_batch(Multi_key_params *params);

  /* reset() takes the memory of the refill: space_per_tuple() bytes for each
     add() until the next reset(). */
  size_t space_per_tuple() const;
  void reset(HANDLER_BUFFER *buff);
  /* Packs the combination now in the record buffers, which the join buffer
     holds at pos; nothing is added when pack() fails. */
  void add(uchar *pos);
  /* Drops exact duplicates and cuts the rest into calls: tuples that share a
     key go to successive calls. */
  void split();
  /* row_data of the next call, the parameter copies moved into row_data[0];
     NULL after the last call. */
  uchar **next_call();
  /* nullptr-terminated join buffer positions of the current call's tuple */
  uchar **positions(uint32 offset);

private:
  union Link
  {
    uchar *pos;
    uchar **positions;
  };

  Multi_key_params *params;
  uchar *slots, *slots_end, *buff_end;
  uchar **row_data;
  uint calls, call, call_tuples;

  size_t slot_length() const;
  Link *link(uchar *slot) const;
};

#endif

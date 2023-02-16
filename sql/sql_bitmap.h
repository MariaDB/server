/* Copyright (c) 2003, 2013, Oracle and/or its affiliates
   Copyright (c) 2009, 2013, Monty Program Ab.

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

/*
  Implementation of a bitmap type.
  The idea with this is to be able to handle any constant number of bits but
  also be able to use 32 or 64 bits bitmaps very efficiently
*/

#ifndef SQL_BITMAP_INCLUDED
#define SQL_BITMAP_INCLUDED

#include <my_sys.h>
#include <my_bitmap.h>
#include <my_bit.h>


/* An iterator to quickly walk over bits in ulonglong bitmap. */
class Table_map_iterator
{
  ulonglong bmp;
public:
  Table_map_iterator(ulonglong t): bmp(t){}
  uint next_bit()
  {
    if (!bmp)
      return BITMAP_END;
    uint bit= my_find_first_bit(bmp);
    bmp &= ~(1ULL << bit);
    return bit;
  }
  int operator++(int) { return next_bit(); }
  enum { BITMAP_END= 64 };
};

template <uint width> class Bitmap
{
/*
  Workaround GCC optimizer bug (generating SSE instuctions on unaligned data)
*/
#if defined (__GNUC__) && defined(__x86_64__) && (__GNUC__ < 6) && !defined(__clang__)
#define NEED_GCC_NO_SSE_WORKAROUND
#endif

#ifdef NEED_GCC_NO_SSE_WORKAROUND
#pragma GCC push_options
#pragma GCC target ("no-sse")
#endif

private:
  static const int BITS_PER_ELEMENT= sizeof(ulonglong) * 8;
  static const int ARRAY_ELEMENTS= (width + BITS_PER_ELEMENT - 1) / BITS_PER_ELEMENT;
  static const ulonglong ALL_BITS_SET= ULLONG_MAX;

  ulonglong buffer[ARRAY_ELEMENTS];

  uint bit_index(uint n) const
  {
    DBUG_ASSERT(n < width);
    return ARRAY_ELEMENTS == 1 ? 0 : n / BITS_PER_ELEMENT;
  }
  ulonglong bit_mask(uint n) const
  {
    DBUG_ASSERT(n < width);
    return ARRAY_ELEMENTS == 1 ? 1ULL << n : 1ULL << (n % BITS_PER_ELEMENT);
  }
  ulonglong last_element_mask(int n) const
  {
    DBUG_ASSERT(n % BITS_PER_ELEMENT != 0);
    return bit_mask(n) - 1;
  }

public:
  /*
   The default constructor does nothing.
   The caller is supposed to either zero the memory
   or to call set_all()/clear_all()/set_prefix()
   to initialize bitmap.
  */
  Bitmap() = default;

  explicit Bitmap(uint prefix)
  {
    set_prefix(prefix);
  }
  void init(uint prefix)
  {
    set_prefix(prefix);
  }
  uint length() const
  {
    return width;
  }
  void set_bit(uint n)
  {
    buffer[bit_index(n)] |= bit_mask(n);
  }
  void clear_bit(uint n)
  {
    buffer[bit_index(n)] &= ~bit_mask(n);
  }
  bool is_set(uint n) const
  {
    return buffer[bit_index(n)] & bit_mask(n);
  }
  void set_prefix(uint prefix_size)
  {
    set_if_smaller(prefix_size, width);

    size_t idx= prefix_size / BITS_PER_ELEMENT;

    for (size_t i= 0; i < idx; i++)
      buffer[i]= ALL_BITS_SET;

    if (prefix_size % BITS_PER_ELEMENT)
      buffer[idx++]= last_element_mask(prefix_size);

    for (size_t i= idx; i < ARRAY_ELEMENTS; i++)
      buffer[i]= 0;
  }
  bool is_prefix(uint prefix_size) const
  {
    DBUG_ASSERT(prefix_size <= width);

    size_t idx= prefix_size / BITS_PER_ELEMENT;

    for (size_t i= 0; i < idx; i++)
      if (buffer[i] != ALL_BITS_SET)
        return false;

    if (prefix_size % BITS_PER_ELEMENT)
      if (buffer[idx++] != last_element_mask(prefix_size))
        return false;

    for (size_t i= idx; i < ARRAY_ELEMENTS; i++)
      if (buffer[i] != 0)
        return false;

    return true;
  }
  void set_all()
  {
    if (width % BITS_PER_ELEMENT)
      set_prefix(width);
    else if (ARRAY_ELEMENTS > 1)
      memset(buffer, 0xff, sizeof(buffer));
    else
      buffer[0] = ALL_BITS_SET;
  }
  void clear_all()
  {
    if (ARRAY_ELEMENTS > 1)
      memset(buffer, 0, sizeof(buffer));
    else
      buffer[0]= 0;
  }
  void intersect(const Bitmap& map2)
  {
    for (size_t i= 0; i < ARRAY_ELEMENTS; i++)
      buffer[i] &= map2.buffer[i];
  }

private:
  /*
     Intersect with a bitmap represented as as longlong.
     In addition, pad the rest of the bitmap with 0 or 1 bits
     depending on pad_with_ones parameter.
  */
  void intersect_and_pad(ulonglong map2buff, bool pad_with_ones)
  {
    buffer[0] &= map2buff;

    for (size_t i= 1; i < ARRAY_ELEMENTS; i++)
      buffer[i]= pad_with_ones ? ALL_BITS_SET : 0;

    if (ARRAY_ELEMENTS > 1 && (width % BITS_PER_ELEMENT) && pad_with_ones)
      buffer[ARRAY_ELEMENTS - 1]= last_element_mask(width);
  }

public:
  void intersect(ulonglong map2buff)
  {
    intersect_and_pad(map2buff, 0);
  }
  /* Use highest bit for all bits above first element. */
  void intersect_extended(ulonglong map2buff)
  {
    intersect_and_pad(map2buff, (map2buff & (1ULL << 63)));
  }
  void subtract(const Bitmap& map2)
  {
    for (size_t i= 0; i < ARRAY_ELEMENTS; i++)
      buffer[i] &= ~(map2.buffer[i]);
  }
  void merge(const Bitmap& map2)
  {
    for (size_t i= 0; i < ARRAY_ELEMENTS; i++)
      buffer[i] |= map2.buffer[i];
  }
  bool is_clear_all() const
  {
    for (size_t i= 0; i < ARRAY_ELEMENTS; i++)
      if (buffer[i])
        return false;
    return true;
  }
  bool is_subset(const Bitmap& map2) const
  {
    for (size_t i= 0; i < ARRAY_ELEMENTS; i++)
      if (buffer[i] & ~(map2.buffer[i]))
        return false;
    return true;
  }
  bool is_overlapping(const Bitmap& map2) const
  {
    for (size_t i= 0; i < ARRAY_ELEMENTS; i++)
      if (buffer[i] & map2.buffer[i])
        return true;
    return false;
  }
  bool operator==(const Bitmap& map2) const
  {
    if (ARRAY_ELEMENTS > 1)
      return !memcmp(buffer,map2.buffer,sizeof(buffer));
    return buffer[0] == map2.buffer[0];
  }
  bool operator!=(const Bitmap& map2) const
  {
    return !(*this == map2);
  }
  /*
    Print hexadecimal representation of bitmap.
    Truncate trailing zeros.
  */
  char *print(char *buf) const
  {
    size_t last; /*index of the last non-zero element, or 0. */

    for (last= ARRAY_ELEMENTS - 1; last && !buffer[last]; last--){}

    const int HEX_DIGITS_PER_ELEMENT= BITS_PER_ELEMENT / 4;
    for (size_t i= 0; i < last; i++)
    {
      ulonglong num = buffer[i];
      uint shift = BITS_PER_ELEMENT - 4;
      size_t pos= i * HEX_DIGITS_PER_ELEMENT;
      for (size_t j= 0; j < HEX_DIGITS_PER_ELEMENT; j++)
      {
        buf[pos + j]= _dig_vec_upper[(num >> shift) & 0xf];
        shift += 4;
      }
    }
    longlong2str(buffer[last], buf, 16);
    return buf;
  }
  ulonglong to_ulonglong() const
  {
    return buffer[0];
  }
  uint bits_set() const
  {
    uint res= 0;
    for (size_t i= 0; i < ARRAY_ELEMENTS; i++)
      if (buffer[i])
        res+= my_count_bits(buffer[i]);
    return res;
  }
  uint find_first_bit() const
  {
    for (size_t i= 0; i < ARRAY_ELEMENTS; i++)
      if (buffer[i])
        return (uint)i*BITS_PER_ELEMENT + my_find_first_bit(buffer[i]);
    return width;
  }
  class Iterator
  {
    const Bitmap& map;
    uint offset;
    Table_map_iterator tmi;
  public:
    Iterator(const Bitmap<width>& map2) : map(map2), offset(0), tmi(map2.buffer[0]) {}
    int operator++(int)
    {
      for (;;)
      {
        int nextbit= tmi++;

        if (nextbit != Table_map_iterator::BITMAP_END)
          return offset + nextbit;

        if (offset + BITS_PER_ELEMENT >= map.length())
          return BITMAP_END;

        offset += BITS_PER_ELEMENT;
        tmi= Table_map_iterator(map.buffer[offset / BITS_PER_ELEMENT]);
      }
    }
    enum { BITMAP_END = width };
  };

#ifdef NEED_GCC_NO_SSE_WORKAROUND
#pragma GCC pop_options
#undef NEED_GCC_NO_SSE_WORKAROUND
#endif
};

typedef Bitmap<MAX_INDEXES> key_map; /* Used for finding keys */

#endif /* SQL_BITMAP_INCLUDED */

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

  uint32 buffer[(width + 31) / 32];
public:
  Bitmap()
  {
    clear_all();
  }
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
    DBUG_ASSERT(n < width);
    ((uchar*)buffer)[n / 8] |= (1 << (n & 7));
  }
  void clear_bit(uint n)
  {
    DBUG_ASSERT(n < width);
    ((uchar*)buffer)[n / 8] &= ~(1 << (n & 7));
  }
  void set_prefix(uint prefix_size)
  {
    set_if_smaller(prefix_size, width);
    uint prefix_bytes, prefix_bits, d;
    uchar* m = (uchar*)buffer;

    if ((prefix_bytes = prefix_size / 8))
      memset(m, 0xff, prefix_bytes);
    m += prefix_bytes;
    if ((prefix_bits = prefix_size & 7))
    {
      *(m++) = (1 << prefix_bits) - 1;
      // As the prefix bits are set, lets count this byte too as a prefix byte.
      prefix_bytes++;
    }
    if ((d = (width + 7) / 8 - prefix_bytes))
      memset(m, 0, d);
  }
  void set_all()
  {
    set_prefix(width);
  }
  void clear_all()
  {
    memset(buffer, 0x00, sizeof(buffer));
  }
  void intersect(Bitmap & map2)
  {
    for (uint i = 0; i < array_elements(buffer); i++)
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
    compile_time_assert(sizeof(ulonglong) == 8);
    uint32 tmp[2];
    int8store(tmp, map2buff);

    buffer[0] &= tmp[0];
    if (array_elements(buffer) > 1)
      buffer[1] &= tmp[1];

    if (array_elements(buffer) <= 2)
      return;
    if (pad_with_ones)
    {
      memset((char*)buffer + 8, 0xff , sizeof(buffer) - 8);
      if (width != sizeof(buffer) * 8)
      {
        ((uchar*)buffer)[sizeof(buffer)-1] = last_byte_mask(width);
      }
    }
    else
      memset((char*)buffer + 8, 0 , sizeof(buffer) - 8);

  }

public:
  void intersect(ulonglong map2buff)
  {
    intersect_and_pad(map2buff, 0);
  }
  /* Use highest bit for all bits above sizeof(ulonglong)*8. */
  void intersect_extended(ulonglong map2buff)
  {
    intersect_and_pad(map2buff, (map2buff & (1ULL << 63)));
  }
  void subtract(Bitmap & map2)
  {
    for (size_t i = 0; i < array_elements(buffer); i++)
      buffer[i] &= ~(map2.buffer[i]);
  }
  void merge(Bitmap & map2)
  {
    for (size_t i = 0; i < array_elements(buffer); i++)
      buffer[i] |= map2.buffer[i];
  }
  bool is_set(uint n) const
  {
    DBUG_ASSERT(n < width);
    return ((uchar*)buffer)[n / 8] & (1 << (n & 7));
  }
  bool is_prefix(uint prefix_size) const
  {
    uint prefix_mask = last_byte_mask(prefix_size);
    uchar* m = (uchar*)buffer;
    uchar* end_prefix = m + (prefix_size - 1) / 8;
    uchar* end;
    DBUG_ASSERT(prefix_size <= width);

    /* Empty prefix is always true */
    if (!prefix_size)
      return true;

    while (m < end_prefix)
      if (*m++ != 0xff)
        return false;

    end = ((uchar*)buffer) + (width + 7) / 8 - 1;
    if (m == end)
      return ((*m & last_byte_mask(width)) == prefix_mask);

    if (*m != prefix_mask)
      return false;

    while (++m < end)
      if (*m != 0)
        return false;
    return ((*m & last_byte_mask(width)) == 0);
  }
  bool is_clear_all() const
  {
    for (size_t i= 0; i < array_elements(buffer); i++)
      if (buffer[i])
        return false;
    return true;
  }
  bool is_set_all() const
  {
    if (width == sizeof(buffer) * 8)
    {
      for (size_t i = 0; i < array_elements(buffer); i++)
        if (buffer[i] != 0xFFFFFFFFU)
          return false;
      return true;
    }
    else
      return is_prefix(width);
  }

  bool is_subset(const Bitmap & map2) const
  {
    for (size_t i= 0; i < array_elements(buffer); i++)
      if (buffer[i] & ~(map2.buffer[i]))
        return false;
    return true;
  }
  bool is_overlapping(const Bitmap & map2) const
  {
    for (size_t i = 0; i < array_elements(buffer); i++)
      if (buffer[i] & map2.buffer[i])
        return true;
    return false;
  }
  bool operator==(const Bitmap & map2) const
  {
    return memcmp(buffer, map2.buffer, sizeof(buffer)) == 0;
  }
  bool operator!=(const Bitmap & map2) const
  {
    return !(*this == map2);
  }
  char *print(char *buf) const
  {
    char *s=buf;
    const uchar *e=(uchar *)buffer, *b=e+sizeof(buffer)-1;
    while (!*b && b>e)
      b--;
    if ((*s=_dig_vec_upper[*b >> 4]) != '0')
        s++;
    *s++=_dig_vec_upper[*b & 15];
    while (--b>=e)
    {
      *s++=_dig_vec_upper[*b >> 4];
      *s++=_dig_vec_upper[*b & 15];
    }
    *s=0;
    return buf;
  }
  ulonglong to_ulonglong() const
  {
    DBUG_ASSERT(sizeof(buffer) >= 4);
    uchar *b=(uchar *)buffer;
    if (sizeof(buffer) >= 8)
      return uint8korr(b);
    return (ulonglong) uint4korr(b);
  }
  uint bits_set()
  {
    uint res = 0;
    for (size_t i = 0; i < array_elements(buffer); i++)
      res += my_count_bits_uint32(buffer[i]);
    return res;
  }
  class Iterator
  {
    Bitmap &map;
    uint no;
  public:
    Iterator(Bitmap<width> &map2): map(map2), no(0) {}
    int operator++(int) {
      if (no == width) return BITMAP_END;
      while (!map.is_set(no))
      {
        if ((++no) == width) return BITMAP_END;
      }
      return no++;
    }
    enum { BITMAP_END = width };
  };

#ifdef NEED_GCC_NO_SSE_WORKAROUND
#pragma GCC pop_options
#undef NEED_GCC_NO_SSE_WORKAROUND
#endif

};


/* An iterator to quickly walk over bits in ulonglong bitmap. */
class Table_map_iterator
{
  ulonglong bmp;
  uint no;
public:
  Table_map_iterator(ulonglong t) : bmp(t), no(0) {}
  uint next_bit()
  {
    static const uchar last_bit[16]= {32, 0, 1, 0,
                                      2, 0, 1, 0, 
                                      3, 0, 1, 0,
                                      2, 0, 1, 0};
    uint bit;
    while ((bit= last_bit[bmp & 0xF]) == 32)
    {
      no += 4;
      bmp= bmp >> 4;
      if (!bmp)
        return BITMAP_END;
    }
    bmp &= ~(1ULL << bit);
    return no + bit;
  }
  uint operator++(int) { return next_bit(); }
  enum { BITMAP_END= 64 };
};

template <> class Bitmap<64>
{
  ulonglong map;
public:
  Bitmap<64>() { }
  explicit Bitmap<64>(uint prefix_to_set) { set_prefix(prefix_to_set); }
  void init(uint prefix_to_set) { set_prefix(prefix_to_set); }
  uint length() const { return 64; }
  void set_bit(uint n) { map|= ((ulonglong)1) << n; }
  void clear_bit(uint n) { map&= ~(((ulonglong)1) << n); }
  void set_prefix(uint n)
  {
    if (n >= length())
      set_all();
    else
      map= (((ulonglong)1) << n)-1;
  }
  void set_all() { map=~(ulonglong)0; }
  void clear_all() { map=(ulonglong)0; }
  void intersect(Bitmap<64>& map2) { map&= map2.map; }
  void intersect(ulonglong map2) { map&= map2; }
  void intersect_extended(ulonglong map2) { map&= map2; }
  void subtract(Bitmap<64>& map2) { map&= ~map2.map; }
  void merge(Bitmap<64>& map2) { map|= map2.map; }
  bool is_set(uint n) const { return MY_TEST(map & (((ulonglong) 1) << n)); }
  bool is_prefix(uint n) const { return map == (((ulonglong)1) << n)-1; }
  bool is_clear_all() const { return map == (ulonglong)0; }
  bool is_set_all() const { return map == ~(ulonglong)0; }
  bool is_subset(const Bitmap<64>& map2) const { return !(map & ~map2.map); }
  bool is_overlapping(const Bitmap<64>& map2) const { return (map & map2.map)!= 0; }
  bool operator==(const Bitmap<64>& map2) const { return map == map2.map; }
  char *print(char *buf) const {
    longlong2str(longlong(map), buf, 16);
    return buf;
  }
  ulonglong to_ulonglong() const { return map; }
  class Iterator : public Table_map_iterator
  {
  public:
    Iterator(Bitmap<64> &map2) : Table_map_iterator(map2.map) {}
  };
  uint bits_set()
  {
    //TODO: use my_count_bits()
    uint res= 0, i= 0;
    for (; i < 64 ; i++)
    {
      if (map & ((ulonglong)1<<i))
        res++;
    }
    return res;
  }
};

#if MAX_INDEXES <= 64
typedef Bitmap<64>  key_map;          /* Used for finding keys */
#elif MAX_INDEXES > 128
#error "MAX_INDEXES values greater than 128 is not supported."
#else
typedef Bitmap<((MAX_INDEXES+7)/8*8)> key_map; /* Used for finding keys */
#endif

#endif /* SQL_BITMAP_INCLUDED */

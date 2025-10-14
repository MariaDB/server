/*
   Copyright (c) 2001, 2011, Oracle and/or its affiliates.
   Copyright (C) 2009- 2011 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA
 */

/*
  Handling of my_bitmap_map (ulonglong) arrays as large bitmaps.

  API limitations (or, rather asserted safety assumptions,
  to encourage correct programming)

    * the internal storage is a set of 64 bit words
    * the number of bits specified in creation can be any number > 0

  Implementation notes:
    * MY_BITMAP includes a pointer, last_word_ptr, to the last word.
     The implication is that if one copies bitmaps to another memory
     location, one has to call create_last_bit_mask() on the bitmap to
     fix the internal pointer.
   * The not used part of a the last word should always be 0.
     This avoids special handling of the last bitmap in several cases.
     This is checked for most calls to bitmap functions.

  TODO:
  Make assembler thread safe versions of these using test-and-set instructions

  Original version created by Sergei Golubchik 2001 - 2004.
  New version written and test program added and some changes to the interface
  was made by Mikael Ronstrom 2005, with assistance of Tomas Ulin and Mats
  Kindahl.
  Updated to 64 bits and use my_find_first_bit() to speed up
  bitmap_get_next_set() by Monty in 2024
*/

#include "mysys_priv.h"
#include <my_bitmap.h>
#include <m_string.h>
#include <my_bit.h>
#include <my_byteorder.h>


/* Defines to check bitmaps */

#define DBUG_ASSERT_BITMAP(M) \
  DBUG_ASSERT((M)->bitmap); \
  DBUG_ASSERT((M)->n_bits > 0); \
  DBUG_ASSERT((M)->last_word_ptr == (M)->bitmap + no_words_in_map(M)-1); \
  DBUG_ASSERT((*(M)->last_word_ptr & (M)->last_bit_mask) == 0);

#define DBUG_ASSERT_BITMAP_AND_BIT(M,B) \
  DBUG_ASSERT_BITMAP(M); \
  DBUG_ASSERT((B) < (M)->n_bits);

#define DBUG_ASSERT_DIFFERENT_BITMAPS(M,N) \
  DBUG_ASSERT_BITMAP(M); \
  DBUG_ASSERT_BITMAP(N);

#define DBUG_ASSERT_IDENTICAL_BITMAPS(M,N) \
  DBUG_ASSERT_BITMAP(M); \
  DBUG_ASSERT_BITMAP(N); \
  DBUG_ASSERT((M)->n_bits == (N)->n_bits);

/*
  Create a mask for the usable bits on the LAST my_bitmap_map position for
  a bitmap with 'bits' number of bits.

  The lowest 'bits' bits are set to zero and the rest bits are set to 1.
  For (bits & 63) == 0 , 0 is returned as in this case all bits in the
  my_bitmap_position are significant. (This example assumes the
  storage is ulonglong).

  For 'bits & 63' it will return values from the series
  0, 0xfffffffffffffffe,.... 0x8000000000000000
*/

static inline my_bitmap_map last_bit_mask(uint bits)
{
  uint bits_in_last_map= (bits & (my_bitmap_map_bits-1));
  return bits_in_last_map ? ~((1ULL << bits_in_last_map)-1) : 0ULL;
}


/*
  Get a mask of the bits that are to be considered as 'on' at location
  starting with 'bits'.
  This function has _inv in it's name as it's usage is invers compared
  to last_bit_mask().

  For (bits & 63) it will return values from the series
  0xffffffffffffffff, 0xfffffffffffffffe,.... 0x8000000000000000
*/

static inline my_bitmap_map first_bit_mask_inv(uint bits)
{
  uint bits_in_last_map= (bits & (my_bitmap_map_bits-1));
  return ~((1ULL << bits_in_last_map)-1);
}


/*
  Update the bitmap's last_word_ptr and last_bit_mask
  Also ensure that the last world is all zero to make it
  easy to find the next set bit.

  Note that if n_bits is 0, then last_word_ptr will point to
  bitmap (safely). The bitmap will not be usable for almost any operation.
*/

void create_last_bit_mask(MY_BITMAP *map)
{
  my_bitmap_map mask= last_bit_mask(map->n_bits);
  map->last_bit_mask= mask;
  map->last_word_ptr= map->bitmap + MY_MAX(no_words_in_map(map),1) -1;
  if (map->n_bits > 0)
  {
    *map->last_word_ptr&= ~mask;           /* Set not used bits to 0 */
    DBUG_ASSERT_BITMAP(map);
  }
}


static inline void bitmap_lock(MY_BITMAP *map __attribute__((unused)))
{
  if (map->mutex)
    mysql_mutex_lock(map->mutex);
}

static inline void bitmap_unlock(MY_BITMAP *map __attribute__((unused)))
{
  if (map->mutex)
    mysql_mutex_unlock(map->mutex);
}


/*
  Initialize a bitmap object. All bits will be set to zero
*/

my_bool my_bitmap_init(MY_BITMAP *map, my_bitmap_map *buf, uint n_bits,
                       my_bool thread_safe)
{
  DBUG_ENTER("my_bitmap_init");
  map->mutex= 0;

  if (!buf)
  {
    uint size_in_bytes= bitmap_buffer_size(n_bits);
    uint extra= 0;
    if (thread_safe)
    {
      size_in_bytes= ALIGN_SIZE(size_in_bytes);
      extra= sizeof(mysql_mutex_t);
    }
    if (!(buf= (my_bitmap_map*) my_malloc(key_memory_MY_BITMAP_bitmap,
                                          size_in_bytes+extra, MYF(MY_WME))))
    {
      map->bitmap= 0;
      DBUG_RETURN(1);
    }
    if (thread_safe)
    {
      map->mutex= (mysql_mutex_t *) ((char*) buf + size_in_bytes);
      mysql_mutex_init(key_BITMAP_mutex, map->mutex, MY_MUTEX_INIT_FAST);
    }
    map->bitmap_allocated= 1;
  }
  else
  {
    DBUG_ASSERT(thread_safe == 0);
    map->bitmap_allocated= 0;
  }

  map->bitmap= buf;
  map->n_bits= n_bits;
  create_last_bit_mask(map);
  bitmap_clear_all(map);
  DBUG_RETURN(0);
}


void my_bitmap_free(MY_BITMAP *map)
{
  DBUG_ENTER("my_bitmap_free");
  if (map->bitmap)
  {
    if (map->mutex)
      mysql_mutex_destroy(map->mutex);
    if (map->bitmap_allocated)
      my_free(map->bitmap);
    map->bitmap=0;
  }
  DBUG_VOID_RETURN;
}


/*
  test if bit already set and set it if it was not (thread unsafe method)

  SYNOPSIS
    bitmap_fast_test_and_set()
    MAP   bit map struct
    BIT   bit number

  RETURN
    0    bit was not set
    !=0  bit was set
*/

my_bool bitmap_fast_test_and_set(MY_BITMAP *map, uint bitmap_bit)
{
  my_bitmap_map *value, bit, res;
  DBUG_ASSERT_BITMAP_AND_BIT(map, bitmap_bit);

  value= map->bitmap + (bitmap_bit/my_bitmap_map_bits);
  bit= 1ULL << (bitmap_bit & (my_bitmap_map_bits-1));
  res= *value & bit;
  *value|= bit;
  return MY_TEST(res);
}


/*
  test if bit already set and set it if it was not (thread safe method)

  SYNOPSIS
    bitmap_fast_test_and_set()
    map          bit map struct
    bitmap_bit   bit number

  RETURN
    0    bit was not set
    !=0  bit was set
*/

my_bool bitmap_test_and_set(MY_BITMAP *map, uint bitmap_bit)
{
  my_bool res;
  DBUG_ASSERT_BITMAP_AND_BIT(map, bitmap_bit);
  bitmap_lock(map);
  res= bitmap_fast_test_and_set(map, bitmap_bit);
  bitmap_unlock(map);
  return res;
}

/*
  test if bit already set and clear it if it was set(thread unsafe method)

  SYNOPSIS
    bitmap_fast_test_and_set()
    MAP   bit map struct
    BIT   bit number

  RETURN
    0    bit was not set
    !=0  bit was set
*/

my_bool bitmap_fast_test_and_clear(MY_BITMAP *map, uint bitmap_bit)
{
  my_bitmap_map *value, bit, res;
  DBUG_ASSERT_BITMAP_AND_BIT(map, bitmap_bit);

  value= map->bitmap + (bitmap_bit/my_bitmap_map_bits);
  bit= 1ULL << (bitmap_bit & (my_bitmap_map_bits-1));
  res= *value & bit;
  *value&= ~bit;
  return MY_TEST(res);
}


my_bool bitmap_test_and_clear(MY_BITMAP *map, uint bitmap_bit)
{
  my_bool res;
  DBUG_ASSERT_BITMAP_AND_BIT(map, bitmap_bit);

  bitmap_lock(map);
  res= bitmap_fast_test_and_clear(map, bitmap_bit);
  bitmap_unlock(map);
  return res;
}


uint bitmap_set_next(MY_BITMAP *map)
{
  uint bit_found;
  DBUG_ASSERT_BITMAP(map);
  if ((bit_found= bitmap_get_first_clear(map)) != MY_BIT_NONE)
    bitmap_set_bit(map, bit_found);
  return bit_found;
}


/**
  Set the specified number of bits in the bitmap buffer.

  @param map         [IN]       Bitmap
  @param prefix_size [IN]       Number of bits to be set or (uint) ~0 for all
*/

void bitmap_set_prefix(MY_BITMAP *map, uint prefix_size)
{
  uint prefix, prefix_bits;
  my_bitmap_map *value= map->bitmap;
  DBUG_ASSERT_BITMAP(map);
  DBUG_ASSERT(prefix_size <= map->n_bits || prefix_size == (uint) ~0);
  set_if_smaller(prefix_size, map->n_bits);

  if ((prefix= prefix_size / my_bitmap_map_bits))
  {
    my_bitmap_map *end= value+prefix;
    do
    {
      *value++= ~(my_bitmap_map) 0;
    } while (value < end);
  }
  if ((prefix_bits= prefix_size & (my_bitmap_map_bits-1)))
    *value++= (1ULL << prefix_bits)-1;
  while (value <= map->last_word_ptr)
    *value++= 0;
  DBUG_ASSERT_BITMAP(map);
}


/**
   Check if bitmap is a bitmap of prefix bits set in the beginning

   @param map          bitmap
   @param prefix_size  number of bits that should be set. 0 is allowed.

   @return 1           Yes, prefix bits where set or prefix_size == 0.
   @return 0           No
*/

my_bool bitmap_is_prefix(const MY_BITMAP *map, uint prefix_size)
{
  my_bitmap_map *value= map->bitmap;
  my_bitmap_map *end=  value+ (prefix_size/my_bitmap_map_bits);
  uint prefix_bits;

  /* Empty prefix is always true */
  if (!prefix_size)
    return 1;

  DBUG_ASSERT_BITMAP_AND_BIT(map, prefix_size-1);

  while (value < end)
    if (*value++ != ~(my_bitmap_map) 0)
      return 0;

  if ((prefix_bits= prefix_size & (my_bitmap_map_bits-1)))
  {
    if (*value++ != (1ULL << prefix_bits)-1)
      return 0;
  }
  end= map->last_word_ptr;
  while (value <= end)
    if (*value++ != 0)
      return 0;
  return 1;
}


my_bool bitmap_is_set_all(const MY_BITMAP *map)
{
  my_bitmap_map *data_ptr= map->bitmap;
  my_bitmap_map *end= map->last_word_ptr;
  DBUG_ASSERT_BITMAP(map);

  for (; data_ptr < end; data_ptr++)
    if (*data_ptr != ~(my_bitmap_map)0)
      return FALSE;
  return (*data_ptr | map->last_bit_mask) == ~(my_bitmap_map)0;
}


my_bool bitmap_is_clear_all(const MY_BITMAP *map)
{
  my_bitmap_map *data_ptr= map->bitmap;
  my_bitmap_map *end= map->last_word_ptr;
  DBUG_ASSERT_BITMAP(map);

  for (; data_ptr <= end; data_ptr++)
    if (*data_ptr)
      return FALSE;
  return TRUE;
}


/* Return TRUE if map1 is a subset of map2 */

my_bool bitmap_is_subset(const MY_BITMAP *map1, const MY_BITMAP *map2)
{
  my_bitmap_map *m1= map1->bitmap, *m2= map2->bitmap, *end= map1->last_word_ptr;
  DBUG_ASSERT_IDENTICAL_BITMAPS(map1,map2);

  while (m1 <= end)
  {
    if ((*m1++) & ~(*m2++))
      return 0;
  }
  return 1;
}

/* True if bitmaps has any common bits */

my_bool bitmap_is_overlapping(const MY_BITMAP *map1, const MY_BITMAP *map2)
{
  my_bitmap_map *m1= map1->bitmap, *m2= map2->bitmap, *end= map1->last_word_ptr;
  DBUG_ASSERT_IDENTICAL_BITMAPS(map1,map2);

  while (m1 <= end)
  {
    if ((*m1++) & (*m2++))
      return 1;
  }
  return 0;
}


/*
  Create intersection of two bitmaps

  @param map    map1. Result is stored here
  @param map2   map2
*/

void bitmap_intersect(MY_BITMAP *map, const MY_BITMAP *map2)
{
  my_bitmap_map *to= map->bitmap, *from= map2->bitmap, *end;
  uint len= no_words_in_map(map), len2 = no_words_in_map(map2);
  DBUG_ASSERT_DIFFERENT_BITMAPS(map,map2);

  end= to+MY_MIN(len,len2);
  while (to < end)
    *to++ &= *from++;

  if (len2 <= len)
  {
    to[-1]&= ~map2->last_bit_mask; /* Clear last not relevant bits */
    end+= len-len2;
    while (to < end)
      *to++= 0;
  }
}


/*
  Check if there is some bit index between start_bit and end_bit, such that
  this is at least on bit that set for all bitmaps in bitmap_list.

  SYNOPSIS
    bitmap_exists_intersection()
    bitmpap_array [in]  a set of MY_BITMAPs
    bitmap_count  [in]  number of elements in bitmap_array
    start_bit     [in]  beginning (inclusive) of the range of bits to search
    end_bit       [in]  end (inclusive) of the range of bits to search, must be
                        no bigger than the bits of the shortest bitmap.

  RETURN
    TRUE  if an intersecion exists
    FALSE no intersection
*/

my_bool bitmap_exists_intersection(MY_BITMAP **bitmap_array,
                                   uint bitmap_count,
                                   uint start_bit, uint end_bit)
{
  uint i, j, start_idx, end_idx;
  my_bitmap_map cur_res, first_map;

  DBUG_ASSERT(bitmap_count);
  DBUG_ASSERT(end_bit >= start_bit);
  for (j= 0; j < bitmap_count; j++)
  {
    DBUG_ASSERT_BITMAP_AND_BIT(bitmap_array[j], end_bit);
  }

  start_idx= start_bit/8/sizeof(my_bitmap_map);
  end_idx= end_bit/8/sizeof(my_bitmap_map);

  first_map= first_bit_mask_inv(start_bit);
  cur_res= first_map;
  for (i= start_idx; i < end_idx; i++)
  {
    for (j= 0; cur_res && j < bitmap_count; j++)
      cur_res &= bitmap_array[j]->bitmap[i];
    if (cur_res)
      return TRUE;
    cur_res= ~(my_bitmap_map) 0;
  }
  cur_res= ~last_bit_mask(end_bit+1);
  if (start_idx == end_idx)
    cur_res&= first_map;
  for (j= 0; cur_res && j < bitmap_count; j++)
    cur_res &= bitmap_array[j]->bitmap[end_idx];
  return cur_res != 0;
}


/* True if union of bitmaps have all bits set */

my_bool bitmap_union_is_set_all(const MY_BITMAP *map1, const MY_BITMAP *map2)
{
  my_bitmap_map *m1= map1->bitmap, *m2= map2->bitmap, *end= map1->last_word_ptr;
  DBUG_ASSERT_IDENTICAL_BITMAPS(map1,map2);

  while ( m1 < end)
    if ((*m1++ | *m2++) != ~(my_bitmap_map)0)
      return FALSE;
  /* here both maps have the same number of bits - see assert above */
  return ((*m1 | *m2 | map1->last_bit_mask) != ~(my_bitmap_map)0);
}


void bitmap_subtract(MY_BITMAP *map, const MY_BITMAP *map2)
{
  my_bitmap_map *to= map->bitmap, *from= map2->bitmap, *end= map->last_word_ptr;
  DBUG_ASSERT_IDENTICAL_BITMAPS(map,map2);

  while (to <= end)
    *to++ &= ~(*from++);
}


void bitmap_union(MY_BITMAP *map, const MY_BITMAP *map2)
{
  my_bitmap_map *to= map->bitmap, *from= map2->bitmap, *end;
  uint len= no_words_in_map(map), len2= no_words_in_map(map2);
  uint32 num_bits= MY_MIN(map->n_bits, map2->n_bits);
  DBUG_ASSERT_DIFFERENT_BITMAPS(map,map2);

  end= to + MY_MIN(len, len2) - 1;

  while (to < end)
    *to++ |= *from++;
  *to|= *from & ~last_bit_mask(num_bits); /* Omit last not relevant bits */
}


void bitmap_xor(MY_BITMAP *map, const MY_BITMAP *map2)
{
  my_bitmap_map *to= map->bitmap, *from= map2->bitmap, *end= map->last_word_ptr;
  DBUG_ASSERT_IDENTICAL_BITMAPS(map,map2);

  while (to <= end)
    *to++ ^= *from++;
}


void bitmap_invert(MY_BITMAP *map)
{
  my_bitmap_map *to= map->bitmap, *end= map->last_word_ptr;
  DBUG_ASSERT_BITMAP(map);

  while (to < end)
    *to++ ^= ~(my_bitmap_map)0;

  *to ^= (~(my_bitmap_map)0 & ~map->last_bit_mask);
  DBUG_ASSERT_BITMAP(map);
}


uint bitmap_bits_set(const MY_BITMAP *map)
{
  my_bitmap_map *data_ptr= map->bitmap;
  my_bitmap_map *end= map->last_word_ptr;
  uint res= 0;
  DBUG_ASSERT_BITMAP(map);

  for (; data_ptr <= end; data_ptr++)
    res+= my_count_bits(*data_ptr);

  return res;
}


/**
  Copy bitmaps

  @param map1   to-bitmap
  @param map2   from-bitmap

  @notes
  Code will work even of the bitmaps are of different size.
  In this case, only up to to->n_bits will be copied.
*/

void bitmap_copy(MY_BITMAP *map1, const MY_BITMAP *map2)
{
  my_bitmap_map *to= map1->bitmap, *from= map2->bitmap;
  uint map1_length= no_words_in_map(map1)*sizeof(my_bitmap_map);
  uint map2_length= no_words_in_map(map2)*sizeof(my_bitmap_map);
  uint length= MY_MIN(map1_length, map2_length);
  DBUG_ASSERT_DIFFERENT_BITMAPS(map1,map2);

  memcpy(to, from, length);
  if (length < map1_length)
    bzero(to + length, map1_length - length);
  *map1->last_word_ptr&= ~map1->last_bit_mask;
}


/*
  Find first set bit in the bitmap
*/

uint bitmap_get_first_set(const MY_BITMAP *map)
{
  my_bitmap_map *data_ptr= map->bitmap, *end= map->last_word_ptr;
  DBUG_ASSERT_BITMAP(map);

  for (uint i=0; data_ptr <= end; data_ptr++, i++)
    if (*data_ptr)
      return my_find_first_bit(*data_ptr) + i * sizeof(my_bitmap_map)*8;
  return MY_BIT_NONE;
}


/**
  Get the next set bit.

  @param  map         Bitmap
  @param  bitmap_bit  Bit to start search from

  @return Index to first bit set after bitmap_bit
*/

uint bitmap_get_next_set(const MY_BITMAP *map, uint bitmap_bit)
{
  uint word_pos;
  my_bitmap_map first_word, *data_ptr, *end= map->last_word_ptr;
  DBUG_ASSERT_BITMAP(map);

  /* Look for the next bit */
  bitmap_bit++;
  if (bitmap_bit >= map->n_bits)
    return MY_BIT_NONE;

  word_pos= bitmap_bit / 64;
  data_ptr= map->bitmap + word_pos;

  first_word= *data_ptr & first_bit_mask_inv(bitmap_bit);

  if (first_word)
  {
    /* Optimize common case when most bits are set */
    if (first_word & (1ULL << ((bitmap_bit & (my_bitmap_map_bits-1)))))
      return bitmap_bit;
    return my_find_first_bit(first_word) + (bitmap_bit & ~(my_bitmap_map_bits-1));
  }

  for (data_ptr++; data_ptr <= end; data_ptr++)
  {
    bitmap_bit+= 64;
    if (*data_ptr)
      return my_find_first_bit(*data_ptr) + (bitmap_bit & ~(my_bitmap_map_bits-1));
  }
  return MY_BIT_NONE;
}


/* Get first clear bit */

uint bitmap_get_first_clear(const MY_BITMAP *map)
{
  uint i;
  my_bitmap_map *data_ptr= map->bitmap, *end= map->last_word_ptr;
  DBUG_ASSERT_BITMAP(map);

  for (i= 0; data_ptr < end; data_ptr++, i++)
    if (*data_ptr != ~(my_bitmap_map)0)
      goto found;
  if ((*data_ptr | map->last_bit_mask) == ~(my_bitmap_map)0)
    return MY_BIT_NONE;
found:
  /* find first zero bit by reverting all bits and find first bit */
  return my_find_first_bit(~*data_ptr) + i * sizeof(my_bitmap_map)*8;
}


uint bitmap_lock_set_next(MY_BITMAP *map)
{
  uint bit_found;
  bitmap_lock(map);
  bit_found= bitmap_set_next(map);
  bitmap_unlock(map);
  return bit_found;
}


void bitmap_lock_clear_bit(MY_BITMAP *map, uint bitmap_bit)
{
  bitmap_lock(map);
  DBUG_ASSERT(map->bitmap);
  DBUG_ASSERT(bitmap_bit < map->n_bits);
  bitmap_clear_bit(map, bitmap_bit);
  bitmap_unlock(map);
}
/*
  Functions to export/import bitmaps to an architecture independent format
  (low_byte_first)
*/

#ifdef WORDS_BIGENDIAN
/* Big endian machines, like powerpc or s390x */

void bitmap_export(uchar *to, MY_BITMAP *map)
{
  my_bitmap_map *value;
  uint length;
  uchar buff[my_bitmap_map_bytes];

  for (value= map->bitmap ; value < map->last_word_ptr ; value++)
  {
    int8store(to, *value);
    to+= 8;
  }
  int8store(buff, *value);

  /* We want length & 7 to return a serie 8,2,3,4,5,6,7, 8,2,3,... */
  length= 1+ ((no_bytes_in_export_map(map) + 7) & 7);
  memcpy(to, buff, length);
}


void bitmap_import(MY_BITMAP *map, uchar *from)
{
  my_bitmap_map *value;
  uint length;
  uchar buff[my_bitmap_map_bytes];

  for (value= map->bitmap ; value < map->last_word_ptr ; value++)
  {
    *value= uint8korr(from);
    from+= 8;
  }
  bzero(buff, sizeof(buff));

  /* We want length & 7 to return a serie 8,2,3,4,5,6,7, 8,2,3,... */
  length= 1+ ((no_bytes_in_export_map(map) + 7) & 7);
  memcpy(buff, from, length);
  *value= uint8korr(buff) & ~map->last_bit_mask;
}

#else

/* Little endian machines, like intel and amd */

void bitmap_export(uchar *to, MY_BITMAP *map)
{
  memcpy(to, (uchar*) map->bitmap, no_bytes_in_export_map(map));
}

void bitmap_import(MY_BITMAP *map, uchar *from)
{
  memcpy((uchar*) map->bitmap, from, no_bytes_in_export_map(map));
  *map->last_word_ptr&= ~map->last_bit_mask;
}
#endif /* WORDS_BIGENDIAN */

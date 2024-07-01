/*
   Copyright (c) 2006, 2012, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA

   This test was copied from the unit test inside the
   mysys/my_bitmap.c file and adapted by Mats Kindahl to use the mytap
   library.
*/

#include <my_global.h>
#include <my_sys.h>
#include <my_bitmap.h>
#include <tap.h>
#include <m_string.h>

#define MAX_TESTED_BITMAP_SIZE 1024

uint get_rand_bit(uint bitsize)
{
  if (bitsize == 0)
    return 0;
  return (rand() % bitsize);
}

my_bool test_set_get_clear_bit(MY_BITMAP *map, uint bitsize)
{
  uint i, test_bit;
  uint no_loops= bitsize > 128 ? 128 : bitsize;
  for (i=0; i < no_loops; i++)
  {
    test_bit= get_rand_bit(bitsize);
    bitmap_set_bit(map, test_bit);
    if (!bitmap_is_set(map, test_bit))
      goto error1;
    bitmap_clear_bit(map, test_bit);
    if (bitmap_is_set(map, test_bit))
      goto error2;
  }
  return FALSE;
error1:
  printf("Error in set bit, bit %u, bitsize = %u", test_bit, bitsize);
  return TRUE;
error2:
  printf("Error in clear bit, bit %u, bitsize = %u", test_bit, bitsize);
  return TRUE;
}

my_bool test_flip_bit(MY_BITMAP *map, uint bitsize)
{
  uint i, test_bit;
  uint no_loops= bitsize > 128 ? 128 : bitsize;
  for (i=0; i < no_loops; i++)
  {
    test_bit= get_rand_bit(bitsize);
    bitmap_flip_bit(map, test_bit);
    if (!bitmap_is_set(map, test_bit))
      goto error1;
    bitmap_flip_bit(map, test_bit);
    if (bitmap_is_set(map, test_bit))
      goto error2;
  }
  return FALSE;
error1:
  printf("Error in flip bit 1, bit %u, bitsize = %u", test_bit, bitsize);
  return TRUE;
error2:
  printf("Error in flip bit 2, bit %u, bitsize = %u", test_bit, bitsize);
  return TRUE;
}


my_bool test_get_all_bits(MY_BITMAP *map, uint bitsize)
{
  uint i;
  bitmap_set_all(map);
  if (!bitmap_is_set_all(map))
    goto error1;
  if (!bitmap_is_prefix(map, bitsize))
    goto error5;
  bitmap_clear_all(map);
  if (!bitmap_is_clear_all(map))
    goto error2;
  if (!bitmap_is_prefix(map, 0))
    goto error6;
  for (i=0; i<bitsize;i++)
    bitmap_set_bit(map, i);
  if (!bitmap_is_set_all(map))
    goto error3;
  for (i=0; i<bitsize;i++)
    bitmap_clear_bit(map, i);
  if (!bitmap_is_clear_all(map))
    goto error4;
  return FALSE;
error1:
  diag("Error in set_all, bitsize = %u", bitsize);
  return TRUE;
error2:
  diag("Error in clear_all, bitsize = %u", bitsize);
  return TRUE;
error3:
  diag("Error in bitmap_is_set_all, bitsize = %u", bitsize);
  return TRUE;
error4:
  diag("Error in bitmap_is_clear_all, bitsize = %u", bitsize);
  return TRUE;
error5:
  diag("Error in set_all through set_prefix, bitsize = %u", bitsize);
  return TRUE;
error6:
  diag("Error in clear_all through set_prefix, bitsize = %u", bitsize);
  return TRUE;
}

PRAGMA_DISABLE_CHECK_STACK_FRAME

my_bool test_compare_operators(MY_BITMAP *map, uint bitsize)
{
  uint i, j, test_bit1, test_bit2, test_bit3,test_bit4;
  uint no_loops= bitsize > 128 ? 128 : bitsize;
  MY_BITMAP map2_obj, map3_obj;
  MY_BITMAP *map2= &map2_obj, *map3= &map3_obj;
  my_bitmap_map map2buf[MAX_TESTED_BITMAP_SIZE];
  my_bitmap_map map3buf[MAX_TESTED_BITMAP_SIZE];
  my_bitmap_init(&map2_obj, map2buf, bitsize);
  my_bitmap_init(&map3_obj, map3buf, bitsize);
  bitmap_clear_all(map2);
  bitmap_clear_all(map3);
  for (i=0; i < no_loops; i++)
  {
    test_bit1=get_rand_bit(bitsize);
    bitmap_set_prefix(map, test_bit1);
    test_bit2=get_rand_bit(bitsize);
    bitmap_set_prefix(map2, test_bit2);
    bitmap_intersect(map, map2);
    test_bit3= test_bit2 < test_bit1 ? test_bit2 : test_bit1;
    bitmap_set_prefix(map3, test_bit3);
    if (!bitmap_cmp(map, map3))
      goto error1;
    bitmap_clear_all(map);
    bitmap_clear_all(map2);
    bitmap_clear_all(map3);
    test_bit1=get_rand_bit(bitsize);
    test_bit2=get_rand_bit(bitsize);
    test_bit3=get_rand_bit(bitsize);
    bitmap_set_prefix(map, test_bit1);
    bitmap_set_prefix(map2, test_bit2);
    test_bit3= test_bit2 > test_bit1 ? test_bit2 : test_bit1;
    bitmap_set_prefix(map3, test_bit3);
    bitmap_union(map, map2);
    if (!bitmap_cmp(map, map3))
      goto error2;
    bitmap_clear_all(map);
    bitmap_clear_all(map2);
    bitmap_clear_all(map3);
    test_bit1=get_rand_bit(bitsize);
    test_bit2=get_rand_bit(bitsize);
    test_bit3=get_rand_bit(bitsize);
    bitmap_set_prefix(map, test_bit1);
    bitmap_set_prefix(map2, test_bit2);
    bitmap_xor(map, map2);
    test_bit3= test_bit2 > test_bit1 ? test_bit2 : test_bit1;
    test_bit4= test_bit2 < test_bit1 ? test_bit2 : test_bit1;
    bitmap_set_prefix(map3, test_bit3);
    for (j=0; j < test_bit4; j++)
      bitmap_clear_bit(map3, j);
    if (!bitmap_cmp(map, map3))
      goto error3;
    bitmap_clear_all(map);
    bitmap_clear_all(map2);
    bitmap_clear_all(map3);
    test_bit1=get_rand_bit(bitsize);
    test_bit2=get_rand_bit(bitsize);
    test_bit3=get_rand_bit(bitsize);
    bitmap_set_prefix(map, test_bit1);
    bitmap_set_prefix(map2, test_bit2);
    bitmap_subtract(map, map2);
    if (test_bit2 < test_bit1)
    {
      bitmap_set_prefix(map3, test_bit1);
      for (j=0; j < test_bit2; j++)
        bitmap_clear_bit(map3, j);
    }
    if (!bitmap_cmp(map, map3))
      goto error4;
    bitmap_clear_all(map);
    bitmap_clear_all(map2);
    bitmap_clear_all(map3);
    test_bit1=get_rand_bit(bitsize);
    bitmap_set_prefix(map, test_bit1);
    bitmap_invert(map);
    bitmap_set_all(map3);
    for (j=0; j < test_bit1; j++)
      bitmap_clear_bit(map3, j);
    if (!bitmap_cmp(map, map3))
      goto error5;
    bitmap_clear_all(map);
    bitmap_clear_all(map3);
  }
  return FALSE;
error1:
  diag("intersect error  bitsize=%u,size1=%u,size2=%u", bitsize,
  test_bit1,test_bit2);
  return TRUE;
error2:
  diag("union error  bitsize=%u,size1=%u,size2=%u", bitsize,
  test_bit1,test_bit2);
  return TRUE;
error3:
  diag("xor error  bitsize=%u,size1=%u,size2=%u", bitsize,
  test_bit1,test_bit2);
  return TRUE;
error4:
  diag("subtract error  bitsize=%u,size1=%u,size2=%u", bitsize,
  test_bit1,test_bit2);
  return TRUE;
error5:
  diag("invert error  bitsize=%u,size=%u", bitsize,
  test_bit1);
  return TRUE;
}
PRAGMA_REENABLE_CHECK_STACK_FRAME


my_bool test_count_bits_set(MY_BITMAP *map, uint bitsize)
{
  uint i, bit_count=0, test_bit;
  uint no_loops= bitsize > 128 ? 128 : bitsize;
  for (i=0; i < no_loops; i++)
  {
    test_bit=get_rand_bit(bitsize);
    if (!bitmap_is_set(map, test_bit))
    {
      bitmap_set_bit(map, test_bit);
      bit_count++;
    }
  }
  if (bit_count==0 && bitsize > 0)
    goto error1;
  if (bitmap_bits_set(map) != bit_count)
    goto error2;
  return FALSE;
error1:
  diag("No bits set  bitsize = %u", bitsize);
  return TRUE;
error2:
  diag("Wrong count of bits set, bitsize = %u", bitsize);
  return TRUE;
}

my_bool test_get_first_bit(MY_BITMAP *map, uint bitsize)
{
  uint i, test_bit= 0;
  uint no_loops= bitsize > 128 ? 128 : bitsize;

  bitmap_set_all(map);
  for (i=0; i < bitsize; i++)
    bitmap_clear_bit(map, i);
  if (bitmap_get_first_set(map) != MY_BIT_NONE)
    goto error1;
  bitmap_clear_all(map);
  for (i=0; i < bitsize; i++)
    bitmap_set_bit(map, i);
  if (bitmap_get_first_clear(map) != MY_BIT_NONE)
    goto error2;
  bitmap_clear_all(map);

  for (i=0; i < no_loops; i++)
  {
    test_bit=get_rand_bit(bitsize);
    bitmap_set_bit(map, test_bit);
    if (bitmap_get_first_set(map) != test_bit)
      goto error1;
    bitmap_set_all(map);
    bitmap_clear_bit(map, test_bit);
    if (bitmap_get_first_clear(map) != test_bit)
      goto error2;
    bitmap_clear_all(map);
  }
  return FALSE;
error1:
  diag("get_first_set error bitsize=%u,prefix_size=%u",bitsize,test_bit);
  return TRUE;
error2:
  diag("get_first error bitsize= %u, prefix_size= %u",bitsize,test_bit);
  return TRUE;
}

my_bool test_get_next_bit(MY_BITMAP *map, uint bitsize)
{
  uint i, j, test_bit;
  uint no_loops= bitsize > 128 ? 128 : bitsize;
  for (i=0; i < no_loops; i++)
  {
    uint count= 0, bits_set= 0;
    bitmap_clear_all(map);
    test_bit=get_rand_bit(bitsize);
    for (j=0; j < test_bit; j++)
      bitmap_set_next(map);
    if (!bitmap_is_prefix(map, test_bit))
      goto error1;
    j=  bitmap_get_first_set(map);
    if (j == MY_BIT_NONE)
    {
      if (test_bit != 0)
        goto error1;
      continue;
    }
    count= 1;
    while ((j= bitmap_get_next_set(map,j)) != MY_BIT_NONE)
      count++;
    if (count != test_bit)
      goto error1;

    if (test_bit < 3)
      continue;
    bitmap_clear_all(map);
    for (j=1; j < test_bit; j+=2)
    {
      bits_set++;
      bitmap_set_bit(map, j);
    }
    if ((j= bitmap_get_first_set(map)) == MY_BIT_NONE)
      goto error1;
    count= 1;
    while ((j= bitmap_get_next_set(map,j)) != MY_BIT_NONE)
      count++;
    if (count != bits_set)
      goto error1;
  }

  return FALSE;

error1:
  diag("get_next error  bitsize= %u, prefix_size= %u", bitsize,test_bit);
  return TRUE;
}

my_bool test_prefix(MY_BITMAP *map, uint bitsize)
{
  uint i, j, test_bit;
  uint no_loops= bitsize > 128 ? 128 : bitsize;
  for (i=0; i < no_loops; i++)
  {
    test_bit=get_rand_bit(bitsize);
    bitmap_set_prefix(map, test_bit);
    if (!bitmap_is_prefix(map, test_bit))
      goto error1;
    bitmap_clear_all(map);
    for (j=0; j < test_bit; j++)
      bitmap_set_bit(map, j);
    if (!bitmap_is_prefix(map, test_bit))
      goto error2;
    bitmap_set_all(map);
    for (j=bitsize - 1; ~(j-test_bit); j--)
      bitmap_clear_bit(map, j);
    if (!bitmap_is_prefix(map, test_bit))
      goto error3;
    bitmap_clear_all(map);
  }
  for (i=0; i < bitsize; i++)
  {
    if (bitmap_is_prefix(map, i + 1))
      goto error4;
    bitmap_set_bit(map, i);
    if (!bitmap_is_prefix(map, i + 1))
      goto error5;
    test_bit=get_rand_bit(bitsize);
    bitmap_set_bit(map, test_bit);
    if (test_bit <= i && !bitmap_is_prefix(map, i + 1))
      goto error5;
    else if (test_bit > i)
    {
      if (bitmap_is_prefix(map, i + 1))
        goto error4;
      bitmap_clear_bit(map, test_bit);
    }
  }
  return FALSE;
error1:
  diag("prefix1 error  bitsize = %u, prefix_size = %u", bitsize,test_bit);
  return TRUE;
error2:
  diag("prefix2 error  bitsize = %u, prefix_size = %u", bitsize,test_bit);
  return TRUE;
error3:
  diag("prefix3 error  bitsize = %u, prefix_size = %u", bitsize,test_bit);
  return TRUE;
error4:
  diag("prefix4 error  bitsize = %u, i = %u", bitsize,i);
  return TRUE;
error5:
  diag("prefix5 error  bitsize = %u, i = %u", bitsize,i);
  return TRUE;
}

my_bool test_compare(MY_BITMAP *map, uint bitsize)
{
  MY_BITMAP map2;
  my_bitmap_map map2buf[MAX_TESTED_BITMAP_SIZE];
  uint i, test_bit;
  uint no_loops= bitsize > 128 ? 128 : bitsize;
  if (my_bitmap_init(&map2, map2buf, bitsize))
  {
    diag("init error for bitsize %d", bitsize);
    return TRUE;
  }
  /* Test all 4 possible combinations of set/unset bits. */
  for (i=0; i < no_loops; i++)
  {
    test_bit=get_rand_bit(bitsize);
    bitmap_clear_bit(map, test_bit);
    bitmap_clear_bit(&map2, test_bit);
    if (!bitmap_is_subset(map, &map2))
      goto error_is_subset;
    bitmap_set_bit(map, test_bit);
    if (bitmap_is_subset(map, &map2))
      goto error_is_subset;
    bitmap_set_bit(&map2, test_bit);
    if (!bitmap_is_subset(map, &map2))
      goto error_is_subset;
    bitmap_clear_bit(map, test_bit);
    if (!bitmap_is_subset(map, &map2))
      goto error_is_subset;
    /* Note that test_bit is not cleared i map2. */
  }
  bitmap_clear_all(map);
  bitmap_clear_all(&map2);
  /* Test all 4 possible combinations of set/unset bits. */
  for (i=0; i < no_loops; i++)
  {
    test_bit=get_rand_bit(bitsize);
    if (bitmap_is_overlapping(map, &map2))
      goto error_is_overlapping;
    bitmap_set_bit(map, test_bit);
    if (bitmap_is_overlapping(map, &map2))
      goto error_is_overlapping;
    bitmap_set_bit(&map2, test_bit);
    if (!bitmap_is_overlapping(map, &map2))
      goto error_is_overlapping;
    bitmap_clear_bit(map, test_bit);
    if (bitmap_is_overlapping(map, &map2))
      goto error_is_overlapping;
    bitmap_clear_bit(&map2, test_bit);
    /* Note that test_bit is not cleared i map2. */
  }
  return FALSE;
error_is_subset:
  diag("is_subset error  bitsize = %u", bitsize);
  return TRUE;
error_is_overlapping:
  diag("is_overlapping error  bitsize = %u", bitsize);
  return TRUE;
}

my_bool test_intersect(MY_BITMAP *map, uint bitsize)
{
  uint bitsize2 = 1 + get_rand_bit(MAX_TESTED_BITMAP_SIZE - 1);
  MY_BITMAP map2;
  my_bitmap_map map2buf[MAX_TESTED_BITMAP_SIZE];
  uint i, test_bit1, test_bit2, test_bit3;
  if (my_bitmap_init(&map2, map2buf, bitsize2))
  {
    diag("init error for bitsize %d", bitsize2);
    return TRUE;
  }
  test_bit1= get_rand_bit(bitsize);
  test_bit2= get_rand_bit(bitsize);
  bitmap_set_bit(map, test_bit1);
  bitmap_set_bit(map, test_bit2);
  test_bit3= get_rand_bit(bitsize2);
  bitmap_set_bit(&map2, test_bit3);
  if (test_bit2 < bitsize2)
    bitmap_set_bit(&map2, test_bit2);

  bitmap_intersect(map, &map2);
  if (test_bit2 < bitsize2)
  {
    if (!bitmap_is_set(map, test_bit2))
      goto error;
    bitmap_clear_bit(map, test_bit2);
  }
  if (test_bit1 == test_bit3)
  {
    if (!bitmap_is_set(map, test_bit1))
      goto error;
    bitmap_clear_bit(map, test_bit1);
  }
  if (!bitmap_is_clear_all(map))
    goto error;

  bitmap_set_all(map);
  bitmap_set_all(&map2);
  for (i=0; i < bitsize2; i++)
    bitmap_clear_bit(&map2, i);
  bitmap_intersect(map, &map2);
  if (!bitmap_is_clear_all(map))
    goto error;
  return FALSE;
error:
  diag("intersect error  bitsize = %u, bit1 = %u, bit2 = %u, bit3 = %u",
       bitsize, test_bit1, test_bit2, test_bit3);
  return TRUE;
}

my_bool test_copy(MY_BITMAP *map, uint bitsize)
{
  my_bitmap_map buff[16], buff2[16], buff3[16];
  MY_BITMAP map2, map3;
  uint rnd_bit;

  my_bitmap_init(&map2, buff, sizeof(buff)*8);
  my_bitmap_init(&map3, buff2, sizeof(buff)*8);
  bitmap_set_all(&map2);
  bitmap_set_all(&map3);

  bitsize= MY_MIN(bitsize, map2.n_bits);
  bitmap_copy(map, &map2);
  if (bitmap_bits_set(map) != bitsize)
  {
    diag("bitmap_copy failed on bitsize %d", bitsize);
    return 1;
  }
  bitmap_set_prefix(&map2, rnd_bit= get_rand_bit(bitsize)+1);
  bitmap_export((uchar*) buff3, &map2);
  bitmap_import(&map3, (uchar*) buff3);
  if (!bitmap_cmp(&map2, &map3))
  {
    diag("bitmap_export/bitmap_import failed on bitsize %d  rnd_bit: %d",
         bitsize, rnd_bit);
    return 1;
  }
  return 0;
}

static my_bool exec_bitmap_exists_intersection(MY_BITMAP **maps, uint bitsize,
                                               uint start, uint end, uint bit)
{
  bitmap_clear_all(maps[0]);
  bitmap_clear_all(maps[1]);
  bitmap_set_bit(maps[0], bit);
  bitmap_set_bit(maps[1], bit);
  return bitmap_exists_intersection(maps, 2, start, end);
}

my_bool test_bitmap_exists_intersection(MY_BITMAP *map, uint bitsize)
{
  MY_BITMAP map2;
  uint start_bit, end_bit, rnd_bit;
  MY_BITMAP *maps[2];
  maps[0]= map;
  maps[1]= &map2;

  my_bitmap_init(&map2, 0, bitsize);
  bitmap_clear_all(map);
  bitmap_clear_all(&map2);

  start_bit= get_rand_bit(bitsize);
  end_bit= get_rand_bit(bitsize);
  if (start_bit > end_bit)
    swap_variables(uint, start_bit, end_bit);
  rnd_bit= start_bit+get_rand_bit(end_bit-start_bit);

  if (!exec_bitmap_exists_intersection(maps, bitsize, start_bit, end_bit,
                                       rnd_bit))
    goto err;

  start_bit= end_bit= rnd_bit= 0;
  if (!exec_bitmap_exists_intersection(maps, bitsize, start_bit, end_bit,
                                       rnd_bit))
    goto err;

  start_bit= rnd_bit= 0 ; end_bit= bitsize-1;
  if (!exec_bitmap_exists_intersection(maps, bitsize, start_bit, end_bit,
                                       rnd_bit))
    goto err;

  start_bit= rnd_bit= end_bit= bitsize-1;
  if (!exec_bitmap_exists_intersection(maps, bitsize, start_bit, end_bit,
                                       rnd_bit))
    goto err;

  if (bitsize > 1)
  {
    start_bit= end_bit= 1 ; rnd_bit= 0;
    if (exec_bitmap_exists_intersection(maps, bitsize, start_bit, end_bit,
                                         rnd_bit))
      goto err;

    start_bit= end_bit= bitsize-1 ; rnd_bit= bitsize-2;
    if (exec_bitmap_exists_intersection(maps, bitsize, start_bit, end_bit,
                                        rnd_bit))
      goto err;
  }

  my_bitmap_free(&map2);
  return 0;
err:
  diag("bitmap_exist_intersection failed on bitsize: %d  start_bit: %d  "
       "end_bit: %d  rnd_bit: %d",
       bitsize, start_bit, end_bit, rnd_bit);
  my_bitmap_free(&map2);
  return 1;
}


my_bool do_test(uint bitsize)
{
  MY_BITMAP map;
  my_bitmap_map buf[MAX_TESTED_BITMAP_SIZE];
  if (my_bitmap_init(&map, buf, bitsize))
  {
    diag("init error for bitsize %d", bitsize);
    goto error;
  }
  if (test_set_get_clear_bit(&map,bitsize))
    goto error;
  bitmap_clear_all(&map);
  if (test_flip_bit(&map,bitsize))
    goto error;
  bitmap_clear_all(&map);
  if (test_get_all_bits(&map, bitsize))
    goto error;
  bitmap_clear_all(&map);
  if (test_compare_operators(&map,bitsize))
    goto error;
  bitmap_clear_all(&map);
  if (test_count_bits_set(&map,bitsize))
    goto error;
  bitmap_clear_all(&map);
  if (test_get_first_bit(&map,bitsize))
    goto error;
  bitmap_clear_all(&map);
  if (test_get_next_bit(&map,bitsize))
    goto error;
  bitmap_clear_all(&map);
  if (test_prefix(&map,bitsize))
    goto error;
  bitmap_clear_all(&map);
  if (test_compare(&map,bitsize))
    goto error;
  bitmap_clear_all(&map);
  if (test_intersect(&map,bitsize))
    goto error;
  bitmap_clear_all(&map);
  if (test_copy(&map,bitsize))
    goto error;
  bitmap_clear_all(&map);
  if (test_bitmap_exists_intersection(&map, bitsize))
    goto error;
  return FALSE;
error:
  return TRUE;
}

int main(int argc __attribute__((unused)),char *argv[])
{
  int i;
  int const min_size = 1;
  int const max_size = MAX_TESTED_BITMAP_SIZE;
  MY_INIT(argv[0]);

  plan((max_size - min_size)/7+1);

  /*
    It's ok to do steps in 7, as i module 64 will go trough all values 1..63.
    Any errors in the code should manifest as we are working with integers
    of size 16, 32, or 64 bits...
  */
  for (i= min_size; i < max_size; i+=7)
    ok(do_test(i) == 0, "bitmap size %d", i);
  my_end(0);
  return exit_status();
}

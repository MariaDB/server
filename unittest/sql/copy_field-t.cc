/*
   Copyright (c) 2026, MariaDB Foundation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

/**
  Unit test for the state a Copy_field starts life in.

  Copy_field has two set() overloads.  The field to field one records the
  two fields it works between; the field to string one has no destination
  field to record and records neither, and set(Field*, Field*, bool)
  returns early for a MYSQL_TYPE_NULL destination without recording them
  either.  Entries of both kinds are put in one array, which callers walk
  with a single loop, so from_field and to_field have to read as absent
  where they were never filled in rather than as whatever the memory the
  array was built on happened to hold.  The same goes for the bit the
  destination carries, which the field to string overload never records
  either and which decides whether the copy confirms the attestation.
*/

#include <my_global.h>
#include <my_sys.h>
#include <tap.h>
#include <sql_class.h>
#include <new>

/*
  Any byte but zero: memory the constructor leaves alone must be
  distinguishable from memory it cleared.
*/
static const int POISON= 0xAB;


/**
  Whether a bool reads back as false.

  Asked of the byte rather than of the value.  A bool holding neither 0
  nor 1 makes any comparison of it undefined, and ok() takes anything
  but zero for a pass, so a plain comparison would silently pass over
  exactly the memory this file is about.
*/

static bool starts_false(const bool *flag)
{
  unsigned char raw;

  memcpy(&raw, flag, sizeof(raw));
  return raw == 0;
}


/**
  Construct over memory known to be dirty.
*/

static void test_construction_over_dirty_memory()
{
  union
  {
    char bytes[2 * sizeof(Copy_field)];
    longlong align;
  } storage;
  Copy_field *first, *second;

  memset(storage.bytes, POISON, sizeof(storage.bytes));
  first=  ::new ((void*) storage.bytes) Copy_field;
  second= ::new ((void*) (storage.bytes + sizeof(Copy_field))) Copy_field;

  ok(first->from_field == NULL, "first from_field starts absent");
  ok(first->to_field == NULL, "first to_field starts absent");
  ok(starts_false(&first->to_needs_confirm),
     "first to_needs_confirm starts false");
  ok(second->from_field == NULL, "second from_field starts absent");
  ok(second->to_field == NULL, "second to_field starts absent");
  ok(starts_false(&second->to_needs_confirm),
     "second to_needs_confirm starts false");

  second->~Copy_field();
  first->~Copy_field();
}


/**
  Build an array the way the server builds one, on a MEM_ROOT.

  The root is dirtied and its blocks marked free rather than released, so
  the array is built on bytes that are known not to be zero.
*/

static void test_array_on_mem_root()
{
  const uint count= 3;
  const size_t dirty= count * sizeof(Copy_field) + 64;
  MEM_ROOT mem_root;
  Copy_field *copy;
  uint i;

  init_alloc_root(PSI_NOT_INSTRUMENTED, &mem_root, 4096, 0, MYF(0));
  memset(alloc_root(&mem_root, dirty), POISON, dirty);
  free_root(&mem_root, MYF(MY_MARK_BLOCKS_FREE));

  copy= new (&mem_root) Copy_field[count];
  for (i= 0; i < count; i++)
  {
    ok(copy[i].from_field == NULL, "copy[%u] from_field starts absent", i);
    ok(copy[i].to_field == NULL, "copy[%u] to_field starts absent", i);
    ok(starts_false(&copy[i].to_needs_confirm),
       "copy[%u] to_needs_confirm starts false", i);
  }
  free_root(&mem_root, MYF(0));
}


int main(int argc __attribute__((unused)), char *argv[])
{
  MY_INIT(argv[0]);
  plan(15);

  test_construction_over_dirty_memory();
  test_array_on_mem_root();

  my_end(0);
  return exit_status();
}

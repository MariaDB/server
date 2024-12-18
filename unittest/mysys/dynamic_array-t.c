/* Copyright (c) 2024 Eric Herman and MariaDB Foundation.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include <my_global.h>
#include <my_sys.h>
#include <tap.h>

struct thing {
  unsigned id;
  char name[40];
};

#define THING_STACK_BUF_SIZE 3
#define ROUND_TRIP_NO_GROW_SIZE (THING_STACK_BUF_SIZE - 1)
#define ROUND_TRIP_NEED_GROW_SIZE ((2 * THING_STACK_BUF_SIZE) + 1)

int dyn_array_round_trip(size_t grow_to_size)
{
  struct thing things_buf[THING_STACK_BUF_SIZE];

  DYNAMIC_ARRAY things_array;
  PSI_memory_key psi_key= PSI_NOT_INSTRUMENTED;
  size_t element_size= sizeof(struct thing);
  void *init_buffer= (void *)&things_buf;
  size_t init_alloc= THING_STACK_BUF_SIZE;
  size_t alloc_increment= 2;
  myf my_flags= MYF(MY_WME);
  my_bool err= FALSE;

  ok(1, "THING_STACK_BUF_SIZE: %d, grow_to_size: %zu", THING_STACK_BUF_SIZE,
     grow_to_size);

  err= my_init_dynamic_array2(psi_key, &things_array, element_size, init_buffer,
                              init_alloc, alloc_increment, my_flags);

  ok(!err, "my_init_dynamic_array2");
  if (err) {
    return EXIT_FAILURE;
  }

  for (size_t i= 0; i < grow_to_size; ++i) {
    struct thing tmp_thing;

    tmp_thing.id= (unsigned)i;
    snprintf(tmp_thing.name, sizeof(tmp_thing.name), "thing %zu", i);

    err= insert_dynamic(&things_array, &tmp_thing);
    ok(!err, "insert_dynamic for %zu", i);
    if (err) {
      delete_dynamic(&things_array);
      return EXIT_FAILURE;
    }
  }
  ok(grow_to_size == things_array.elements, "size expect: %zu, actual: %zu",
     grow_to_size, things_array.elements);

  for (size_t i= 0; i < things_array.elements; ++i) {
    struct thing retrieved, expected;

    expected.id= (unsigned)i;
    snprintf(expected.name, sizeof(expected.name), "thing %zu", i);

    memset(&retrieved, 0x00, sizeof(struct thing));
    get_dynamic(&things_array, &retrieved, i);

    ok(retrieved.id == expected.id, "%zu: retrieved id: %u, expected id: %u",
       i, retrieved.id, expected.id);
    ok(strncmp(retrieved.name, expected.name, sizeof(expected.name)) == 0,
       "%zu: retrieved name: '%s', expected name: '%s'",
       i, retrieved.name, expected.name);
  }

  delete_dynamic(&things_array);

  return EXIT_SUCCESS;
}
static const int plan_round_trip_no_grow= 3 + (3 * ROUND_TRIP_NO_GROW_SIZE);
static const int plan_round_trip_grow= 3 + (3 * ROUND_TRIP_NEED_GROW_SIZE);

int dyn_array_push_pop(void)
{
  DYNAMIC_ARRAY things_array;
  PSI_memory_key psi_key= PSI_NOT_INSTRUMENTED;
  size_t element_size= sizeof(struct thing);
  size_t init_alloc= THING_STACK_BUF_SIZE;
  size_t alloc_increment= 2;
  myf my_flags= MYF(MY_WME);
  my_bool err= FALSE;
  struct thing tmp_thing, *popped;

  err= my_init_dynamic_array(psi_key, &things_array, element_size, init_alloc,
		             alloc_increment, my_flags);

  ok(!err, "my_init_dynamic_array");
  if (err) {
    return EXIT_FAILURE;
  }

  tmp_thing.id= 0;
  snprintf(tmp_thing.name, sizeof(tmp_thing.name), "thing 0");
  err= push_dynamic(&things_array, &tmp_thing);
  ok(!err, "push_dynamic for 0");
  if (err) {
    delete_dynamic(&things_array);
    return EXIT_FAILURE;
  }

  tmp_thing.id= 1;
  snprintf(tmp_thing.name, sizeof(tmp_thing.name), "thing 1");
  err= push_dynamic(&things_array, &tmp_thing);
  ok(!err, "push_dynamic for 1");
  if (err) {
    delete_dynamic(&things_array);
    return EXIT_FAILURE;
  }

  ok(2 == things_array.elements, "size expect: %d, actual: %zu",
     2, things_array.elements);

  popped= (struct thing *)pop_dynamic(&things_array);
  ok(popped != NULL, "pop_dynamic 1");
  if (!popped) {
    delete_dynamic(&things_array);
    return EXIT_FAILURE;
  }
  ok(popped->id == 1, "pop expect 1, popped->id: %u", popped->id);
  ok(1 == things_array.elements, "size expect: %d, actual: %zu",
     1, things_array.elements);

  popped= (struct thing *)pop_dynamic(&things_array);
  ok(popped != NULL, "pop_dynamic 0");
  if (!popped) {
    delete_dynamic(&things_array);
    return EXIT_FAILURE;
  }
  ok(popped->id == 0, "pop expect 0, popped->id: %u", popped->id);
  ok(0 == things_array.elements, "size expect: %d, actual: %zu",
     0, things_array.elements);


  popped= (struct thing *)pop_dynamic(&things_array);
  ok(!popped, "pop %p from empty array", popped);

  delete_dynamic(&things_array);

  return EXIT_SUCCESS;
}
static const int plan_push_pop= 11;

struct string_thing {
  unsigned id;
  char *str;
  size_t str_size;
};

static void free_string_thing(void *p)
{
  struct string_thing *thing= (struct string_thing *)p;
  my_free(thing->str);
}

#define NUM_DELETE_WITH_CALLBACK 3

int dyn_array_delete_with_callback(void)
{
  DYNAMIC_ARRAY things_array;
  PSI_memory_key psi_key= PSI_NOT_INSTRUMENTED;
  size_t element_size= sizeof(struct string_thing);
  size_t init_alloc= (NUM_DELETE_WITH_CALLBACK - 1);
  size_t alloc_increment= 2;
  myf my_flags= MYF(MY_WME);
  my_bool err= FALSE;

  err= my_init_dynamic_array(psi_key, &things_array, element_size, init_alloc,
                             alloc_increment, my_flags);

  ok(!err, "my_init_dynamic_array");
  if (err) {
    return EXIT_FAILURE;
  }

  for (size_t i= 0; i < NUM_DELETE_WITH_CALLBACK; ++i) {
    struct string_thing thing= { (unsigned)i, NULL, 40 };
    thing.str= my_malloc(psi_key, thing.str_size, my_flags);
    ok(thing.str != NULL, "%zu thing.str= malloc(%zu))", i, thing.str_size);
    if (!thing.str) {
      delete_dynamic_with_callback(&things_array, free_string_thing);
      return EXIT_FAILURE;
    }
    snprintf(thing.str, thing.str_size, "thing %zu", i);

    err= insert_dynamic(&things_array, &thing);
    ok(!err, "insert_dynamic for %zu", i);
    if (err) {
      delete_dynamic_with_callback(&things_array, free_string_thing);
      return EXIT_FAILURE;
    }
  }
  ok(3 == things_array.elements, "size expect: %d, actual: %zu",
     3, things_array.elements);

  delete_dynamic_with_callback(&things_array, free_string_thing);

  ok(0 == things_array.elements, "size expect: %d, actual: %zu",
     0, things_array.elements);

  return EXIT_SUCCESS;
}
static const int plan_delete_with_callback= 3 + (2 * NUM_DELETE_WITH_CALLBACK);

int main(void)
{
  int err= 0;

  plan(plan_round_trip_no_grow + 1
     + plan_round_trip_grow + 1
     + plan_push_pop + 1
     + plan_delete_with_callback + 1
  );

  err= dyn_array_round_trip(ROUND_TRIP_NO_GROW_SIZE);
  ok(!err, "dyn_array_round_trip (no need to grow)");

  err= dyn_array_round_trip(ROUND_TRIP_NEED_GROW_SIZE);
  ok(!err, "dyn_array_round_trip (need to grow)");

  err= dyn_array_push_pop();
  ok(!err, "dyn_array_push_pop");

  err= dyn_array_delete_with_callback();
  ok(!err, "dyn_array_delete_with_callback");

  return exit_status();
}

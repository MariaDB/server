/* Copyright (c) 2016, MariaDB Corp. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include "my_config.h"
#include "config.h"
#include <tap.h>
#include <my_global.h>
#include <my_sys.h>
#include <json_lib.h>

/* The character set used for JSON all over this test. */
static CHARSET_INFO *ci;

#define s_e(j) j, j + strlen((const char *) j)


struct st_parse_result
{
  int n_keys;
  int n_values;
  int n_arrays;
  int n_objects;
  int n_steps;
  int error;
  uchar keyname_csum;
};


static void parse_json(const uchar *j, struct st_parse_result *result, json_engine_t *je)
{
  bzero(result, sizeof(*result));

  if (json_scan_start(je, ci, s_e(j)))
    return;

  do
  {
    result->n_steps++;
    switch (je->state)
    {
    case JST_KEY:
      result->n_keys++;
      while (json_read_keyname_chr(je) == 0)
      {
        result->keyname_csum^= je->s.c_next;
      }
      if (je->s.error)
      {
        return;
      }
      break;
    case JST_VALUE:
      result->n_values++;
      break;
    case JST_OBJ_START:
      result->n_objects++;
      break;
    case JST_ARRAY_START:
      result->n_arrays++;
      break;
    default:
      break;
    };
  } while (json_scan_next(je) == 0);

  result->error= je->s.error;
}


static const uchar *js0= (const uchar *) "123";
static const uchar *js1= (const uchar *) "[123, \"text\"]";
static const uchar *js2= (const uchar *) "{\"key1\":123, \"key2\":\"text\"}";
static const uchar *js3= (const uchar *) "{\"key1\":{\"ikey1\":321},"
                                          "\"key2\":[\"text\", 321]}";

/*
  Test json_lib functions to parse JSON.
*/
static void
test_json_parsing(json_engine_t *je)
{
  struct st_parse_result r;
  parse_json(js0, &r, je);
  ok(r.n_steps == 1 && r.n_values == 1, "simple value");
  parse_json(js1, &r, je);
  ok(r.n_steps == 5 && r.n_values == 3 && r.n_arrays == 1, "array");
  parse_json(js2, &r, je);
  ok(r.n_steps == 5 && r.n_keys == 2 && r.n_objects == 1 && r.keyname_csum == 3,
     "object");
  parse_json(js3, &r, je);
  ok(r.n_steps == 12 && r.n_keys == 3 && r.n_objects == 2 &&
     r.n_arrays == 1 && r.keyname_csum == 44,
     "complex json");
}


static const uchar *p0= (const uchar *) "$.key1[12].*[*]";
/*
  Test json_lib functions to parse JSON path.
*/
static void
test_path_parsing(json_path_t *p)
{
  json_path_step_t *initial_step= NULL;
  initial_step= (json_path_step_t*)(mem_root_dynamic_array_get_val(&p->steps, 0));

  if (json_path_setup(p, ci, s_e(p0)))
    goto error;

  ok(p->last_step_idx == 4 &&
     (initial_step+0)->type == JSON_PATH_ARRAY_WILD &&
     (initial_step+1)->type == JSON_PATH_KEY &&
     (initial_step+2)->type == JSON_PATH_ARRAY && (initial_step+2)->n_item == 12 &&
     (initial_step+3)->type == JSON_PATH_KEY_WILD &&
     (initial_step+4)->type == JSON_PATH_ARRAY_WILD,
     "path");

  error:
    return;
}


static const uchar *fj0=(const uchar *) "[{\"k0\":123, \"k1\":123, \"k1\":123},"
                                        " {\"k3\":321, \"k4\":\"text\"},"
                                        " {\"k1\":[\"text\"], \"k2\":123}]";
static const uchar *fp0= (const uchar *) "$[*].k1";
/*
  Test json_lib functions to search through JSON.
*/
static void
test_search(MEM_ROOT_DYNAMIC_ARRAY* array_counters, json_engine_t *je, json_path_t *p)
{
  MEM_ROOT_DYNAMIC_ARRAY *cur_step;
  int n_matches, scal_values;
  json_path_step_t *tmp_ptr= NULL;

  if (json_scan_start(je, ci, s_e(fj0)) ||
      json_path_setup(p, ci, s_e(fp0)))
    goto end;

  cur_step= &p->steps;
  n_matches= scal_values= 0;
  tmp_ptr= (json_path_step_t*)mem_root_dynamic_array_get_val(cur_step, 0);
  while (json_find_path(je, p, &tmp_ptr, array_counters) == 0)
  {
    n_matches++;
    if (json_read_value(je))
      goto end;
    if (json_value_scalar(je))
    {
      scal_values++;
      if (json_scan_next(je))
        goto end;
    }
    else
    {
      if (json_skip_level(je) || json_scan_next(je))
        goto end;
    }

  }

  ok(n_matches == 3, "search");

  end:
  return;
}


int main()
{
  MEM_ROOT current_mem_root;
  MEM_ROOT_DYNAMIC_ARRAY array_counters;
  json_engine_t je;
  json_path_t p;

  init_alloc_root(PSI_INSTRUMENT_MEM, &current_mem_root,
                  BLOCK_SIZE_JSON_DYN_ARRAY, 0, MYF(0));

  mem_root_dynamic_array_init(&current_mem_root, PSI_INSTRUMENT_MEM,
                              &array_counters, sizeof(int), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));
  mem_root_dynamic_array_init(&current_mem_root, PSI_INSTRUMENT_MEM,
                              &je.stack, sizeof(int), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));
  mem_root_dynamic_array_init(&current_mem_root, PSI_INSTRUMENT_MEM,
                              &p.steps, sizeof(json_path_step_t), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));

  ci= &my_charset_utf8mb3_general_ci;

  plan(6);
  diag("Testing json_lib functions.");

  test_json_parsing(&je);
  test_path_parsing(&p);
  test_search(&array_counters, &je, &p);

  free_root(&current_mem_root, MYF(0));

  return exit_status();
}

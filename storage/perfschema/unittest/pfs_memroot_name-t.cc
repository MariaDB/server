/* Copyright (c) 2025, MariaDB Corporation.

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

#include <my_global.h>
#include <my_thread.h>
#include <pfs_instr.h>
#include <pfs_stat.h>
#include <pfs_global.h>
#include <pfs_instr_class.h>
#include <pfs_buffer_container.h>
#include <tap.h>

#include "stub_global_status_var.h"

#ifndef DBUG_OFF

void do_all_tests()
{
  MEM_ROOT root;
  PFS_memory_key key;
  const char *name;
  int rc;

  rc= init_memory_class(5);
  ok(rc == 0, "init_memory_class");

  key= register_memory_class("test_memroot", 12, 0);
  ok(key > 0, "test_memroot registered (key=%u)", key);

  /* NULL input */
  name= dbug_print_memroot_name(NULL);
  ok(strcmp(name, "<NULL>") == 0, "NULL returns '<NULL>'");

  /* Registered key returns name */
  init_alloc_root(key, &root, 1024, 0, MYF(0));
  name= dbug_print_memroot_name(&root);
  ok(strcmp(name, "test_memroot") == 0,
     "registered key returns 'test_memroot' (got '%s')", name);
  free_root(&root, MYF(0));

  /* PSI_NOT_INSTRUMENTED returns empty string */
  init_alloc_root(PSI_NOT_INSTRUMENTED, &root, 1024, 0, MYF(0));
  name= dbug_print_memroot_name(&root);
  ok(strcmp(name, "") == 0, "PSI_NOT_INSTRUMENTED returns ''");
  free_root(&root, MYF(0));

  /* Invalid key returns empty string */
  init_alloc_root(9999, &root, 1024, 0, MYF(0));
  name= dbug_print_memroot_name(&root);
  ok(strcmp(name, "") == 0, "invalid key 9999 returns ''");
  free_root(&root, MYF(0));

  cleanup_memory_class();
}

int main(int, char **)
{
  plan(6);
  MY_INIT("pfs_memroot_name-t");
  do_all_tests();
  my_end(0);
  return (exit_status());
}

#else /* DBUG_OFF */

int main(int, char **)
{
  plan(1);
  ok(1, "dbug_print_memroot_name is a no-op in release builds");
  return (exit_status());
}

#endif /* DBUG_OFF */

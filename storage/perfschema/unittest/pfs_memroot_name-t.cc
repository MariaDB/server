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
  PFS_memory_key key1, key2;
  char *p;
  int rc;

  /* Initialize PFS memory class storage */
  rc= init_memory_class(5);
  ok(rc == 0, "init_memory_class");

  /* Register test memory classes */
  key1= register_memory_class("test_memroot", 12, 0);
  ok(key1 > 0, "test_memroot registered (key=%u)", key1);

  key2= register_memory_class("test_memroot_second", 19, 0);
  ok(key2 > 0, "test_memroot_second registered (key=%u)", key2);

  /* Test 1: NULL MEM_ROOT does not crash */
  dbug_print_memroot_name(NULL);
  ok(1, "NULL MEM_ROOT does not crash");

  /* Test 2: MEM_ROOT with registered PSI key */
  init_alloc_root(key1, &root, 1024, 0, MYF(0));
  dbug_print_memroot_name(&root);
  ok(1, "MEM_ROOT with registered PSI key does not crash");
  free_root(&root, MYF(0));

  /* Test 3: MEM_ROOT with PSI_NOT_INSTRUMENTED */
  init_alloc_root(PSI_NOT_INSTRUMENTED, &root, 1024, 0, MYF(0));
  dbug_print_memroot_name(&root);
  ok(root.psi_key == PSI_NOT_INSTRUMENTED,
     "PSI_NOT_INSTRUMENTED key is 0");
  free_root(&root, MYF(0));

  /* Test 4: MEM_ROOT with invalid/unregistered key */
  init_alloc_root(9999, &root, 1024, 0, MYF(0));
  dbug_print_memroot_name(&root);
  ok(root.psi_key == 9999,
     "Invalid key 9999 preserved and does not crash");
  free_root(&root, MYF(0));

  /* Test 5: MEM_ROOT with second key + allocation works */
  init_alloc_root(key2, &root, 2048, 0, MYF(0));
  dbug_print_memroot_name(&root);
  p= (char *) alloc_root(&root, 100);
  ok(p != NULL, "Allocation on MEM_ROOT with second key succeeds");
  free_root(&root, MYF(0));

  cleanup_memory_class();
}

int main(int, char **)
{
  plan(8);
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

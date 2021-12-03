/* Copyright (c) 2008, 2017, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include <my_global.h>
#include <my_pthread.h>
#include <pfs_instr_class.h>
#include <pfs_global.h>
#include <tap.h>

#include "stub_pfs_global.h"

void test_oom()
{
  int rc;

  rc= init_sync_class(1000, 0, 0);
  ok(rc == 1, "oom (mutex)");
  rc= init_sync_class(0, 1000, 0);
  ok(rc == 1, "oom (rwlock)");
  rc= init_sync_class(0, 0, 1000);
  ok(rc == 1, "oom (cond)");
  rc= init_thread_class(1000);
  ok(rc == 1, "oom (thread)");
  rc= init_file_class(1000);
  ok(rc == 1, "oom (file)");
  rc= init_table_share(1000);
  ok(rc == 1, "oom (cond)");
  rc= init_socket_class(1000);
  ok(rc == 1, "oom (socket)");
  rc= init_stage_class(1000);
  ok(rc == 1, "oom (stage)");
  rc= init_statement_class(1000);
  ok(rc == 1, "oom (statement)");

  cleanup_sync_class();
  cleanup_thread_class();
  cleanup_file_class();
  cleanup_table_share();
  cleanup_socket_class();
  cleanup_stage_class();
  cleanup_statement_class();
}

void do_all_tests()
{
  test_oom();
}

int main(int argc, char **argv)
{
  plan(9);
  MY_INIT(argv[0]);
  do_all_tests();
  my_end(0);
  return (exit_status());
}


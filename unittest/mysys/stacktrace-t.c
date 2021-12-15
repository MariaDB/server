
/* Copyright (c) 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include <my_global.h>
#include <my_sys.h>
#include <stdio.h>
#include <my_stacktrace.h>
#include <tap.h>

char b_bss[10];

void test_my_safe_print_str()
{
  char b_stack[10];
  char *b_heap= strdup("LEGAL");
  memcpy(b_stack, "LEGAL", 6);
  memcpy(b_bss, "LEGAL", 6);

#ifdef HAVE_STACKTRACE
#ifndef __SANITIZE_ADDRESS__
  fprintf(stderr, "\n===== stack =====\n");
  my_safe_print_str(b_stack, 65535);
  fprintf(stderr, "\n===== heap =====\n");
  my_safe_print_str(b_heap, 65535);
  fprintf(stderr, "\n===== BSS =====\n");
  my_safe_print_str(b_bss, 65535);
  fprintf(stderr, "\n===== data =====\n");
  my_safe_print_str("LEGAL", 65535);
  fprintf(stderr, "\n===== Above is a junk, but it is expected. =====\n");
#endif /*__SANITIZE_ADDRESS__*/
  fprintf(stderr, "\n===== Nornal length test =====\n");
  my_safe_print_str("LEGAL", 5);
  fprintf(stderr, "\n===== NULL =====\n");
  my_safe_print_str(0, 5);
#ifndef __SANITIZE_ADDRESS__
  fprintf(stderr, "\n===== (const char*) 1 =====\n");
  my_safe_print_str((const char*)1, 5);
#endif /*__SANITIZE_ADDRESS__*/
#endif /*HAVE_STACKTRACE*/

  free(b_heap);

  ok(1, "test_my_safe_print_str");
}


int main(int argc __attribute__((unused)), char **argv)
{
  MY_INIT(argv[0]);
  plan(1);

  test_my_safe_print_str();

  my_end(0);
  return exit_status();
}

/* -*- c-basic-offset: 2; coding: utf-8 -*- */
/*
  Copyright (C) 2015-2016  Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

/*
  Groonga: ed300a833d44eaefa978b5ecf46a96ef91ae0891

  CFLAGS: -O2 -g
  % make --quiet -C benchmark run-bench-nfkc
  run-bench-nfkc:
                               (total)    (average)  (median)
    map1 - switch            : (0.0060ms) (0.00060000ms) (0.00000000ms)
    map1 -  table            : (0.00000000ms) (0.00000000ms) (0.00000000ms)
    map2 - switch - no change: (0.0010ms) (0.00010000ms) (0.00000000ms)
    map2 -  table - no change: (0.00000000ms) (0.00000000ms) (0.00000000ms)
    map2 - switch -    change: (0.0010ms) (0.00010000ms) (0.00000000ms)
    map2 -  table -    change: (0.0010ms) (0.00010000ms) (0.00000000ms)
*/

#include <stdlib.h>
#include <stdio.h>

#include <glib.h>

#include <groonga.h>

#include "lib/benchmark.h"

#include "../lib/nfkc50.c"

#define MAX_UNICODE 0x110000
#define BUFFER_SIZE 0x100

static inline int
ucs2utf8(unsigned int i, unsigned char *buf)
{
  unsigned char *p = buf;
  if (i < 0x80) {
    *p++ = i;
  } else {
    if (i < 0x800) {
      *p++ = (i >> 6) | 0xc0;
    } else {
      if (i < 0x00010000) {
        *p++ = (i >> 12) | 0xe0;
      } else {
        if (i < 0x00200000) {
          *p++ = (i >> 18) | 0xf0;
        } else {
          if (i < 0x04000000) {
            *p++ = (i >> 24) | 0xf8;
          } else if (i < 0x80000000) {
            *p++ = (i >> 30) | 0xfc;
            *p++ = ((i >> 24) & 0x3f) | 0x80;
          }
          *p++ = ((i >> 18) & 0x3f) | 0x80;
        }
        *p++ = ((i >> 12) & 0x3f) | 0x80;
      }
      *p++ = ((i >> 6) & 0x3f) | 0x80;
    }
    *p++ = (0x3f & i) | 0x80;
  }
  *p = '\0';
  return (p - buf);
}

static void
bench_char_type(gpointer user_data)
{
  uint64_t code_point;
  char utf8[7];

  for (code_point = 1; code_point < MAX_UNICODE; code_point++) {
    ucs2utf8(code_point, (unsigned char *)utf8);
    grn_nfkc50_char_type(utf8);
  }
}

static void
bench_decompose(gpointer user_data)
{
  uint64_t code_point;
  char utf8[7];

  for (code_point = 1; code_point < MAX_UNICODE; code_point++) {
    ucs2utf8(code_point, (unsigned char *)utf8);
    grn_nfkc50_decompose(utf8);
  }
}

static void
bench_compose_no_change(gpointer user_data)
{
  uint64_t prefix_code_point;
  uint64_t suffix_code_point = 0x61; /* a */
  char prefix_utf8[7];
  char suffix_utf8[7];

  ucs2utf8(suffix_code_point, (unsigned char *)suffix_utf8);
  for (prefix_code_point = 1;
       prefix_code_point < MAX_UNICODE;
       prefix_code_point++) {
    ucs2utf8(prefix_code_point, (unsigned char *)prefix_utf8);
    grn_nfkc50_compose(prefix_utf8, suffix_utf8);
  }
}

static void
bench_compose_change(gpointer user_data)
{
  uint64_t prefix_code_point;
  uint64_t suffix_code_point = 0x11ba;
  char prefix_utf8[7];
  char suffix_utf8[7];

  ucs2utf8(suffix_code_point, (unsigned char *)suffix_utf8);
  for (prefix_code_point = 1;
       prefix_code_point < MAX_UNICODE;
       prefix_code_point++) {
    ucs2utf8(prefix_code_point, (unsigned char *)prefix_utf8);
    grn_nfkc50_compose(prefix_utf8, suffix_utf8);
  }
}

/*
static void
check_char_type(gpointer user_data)
{
  uint64_t code_point;
  char utf8[7];

  for (code_point = 1; code_point < MAX_UNICODE; code_point++) {
    grn_char_type a;
    grn_char_type b;

    ucs2utf8(code_point, (unsigned char *)utf8);
    a = grn_nfkc_char_type(utf8);
    b = grn_nfkc50_char_type(utf8);
    if (a == b) {
      continue;
    }
    printf("%lx: %s: %d != %d\n", code_point, utf8, a, b);
  }
}

static void
check_decompose(gpointer user_data)
{
  uint64_t code_point;
  char utf8[7];

  for (code_point = 1; code_point < MAX_UNICODE; code_point++) {
    const char *a;
    const char *b;

    ucs2utf8(code_point, (unsigned char *)utf8);
    a = grn_nfkc_decompose(utf8);
    b = grn_nfkc50_decompose(utf8);
    if (a == b) {
      continue;
    }
    if (!a || !b) {
      printf("%lx: %s: %s != %s\n", code_point, utf8, a, b);
      continue;
    }
    if (strcmp(a, b) != 0) {
      printf("%lx: %s: %s != %s\n", code_point, utf8, a, b);
    }
  }
}

static void
check_compose(gpointer user_data)
{
  uint64_t prefix_code_point;
  uint64_t suffix_code_point;
  char prefix_utf8[7];
  char suffix_utf8[7];

  for (prefix_code_point = 1;
       prefix_code_point < MAX_UNICODE;
       prefix_code_point++) {
    ucs2utf8(prefix_code_point, (unsigned char *)prefix_utf8);
    for (suffix_code_point = 1;
         suffix_code_point < MAX_UNICODE;
         suffix_code_point++) {
      const char *a;
      const char *b;

      ucs2utf8(suffix_code_point, (unsigned char *)suffix_utf8);
      a = grn_nfkc_compose(prefix_utf8, suffix_utf8);
      b = grn_nfkc50_compose(prefix_utf8, suffix_utf8);
      if (a == b) {
        continue;
      }
      if (!a || !b) {
        printf("%lx-%lx: %s-%s: %s != %s\n",
               prefix_code_point, suffix_code_point,
               prefix_utf8, suffix_utf8,
               a, b);
        continue;
      }
      if (strcmp(a, b) != 0) {
        printf("%lx-%lx: %s-%s: %s != %s\n",
               prefix_code_point, suffix_code_point,
               prefix_utf8, suffix_utf8,
               a, b);
      }
    }
    if ((prefix_code_point % 10000) == 0) {
      printf("%" G_GUINT64_FORMAT "\n", prefix_code_point);
    }
  }
}
*/

int
main(int argc, gchar **argv)
{
  grn_rc rc;
  BenchReporter *reporter;
  gint n = 10;

  rc = grn_init();
  if (rc != GRN_SUCCESS) {
    g_print("failed to initialize Groonga: <%d>: %s\n",
            rc, grn_get_global_error_message());
    return EXIT_FAILURE;
  }
  bench_init(&argc, &argv);

  reporter = bench_reporter_new();

  if (g_getenv("N")) {
    n = atoi(g_getenv("N"));
  }

#define REGISTER(label, bench_function)                 \
  bench_reporter_register(reporter, label, n,           \
                          NULL,                         \
                          bench_function,               \
                          NULL,                         \
                          NULL)
  REGISTER("char_type            ", bench_char_type);
  REGISTER("decompose            ", bench_decompose);
  REGISTER("compose   - no change", bench_compose_no_change);
  REGISTER("compose   -    change", bench_compose_change);

  /*
    REGISTER("check - char_type", check_char_type);
    REGISTER("check - decompose", check_decompose);
    REGISTER("check - compose  ", check_compose);
  */
#undef REGISTER

  bench_reporter_run(reporter);
  g_object_unref(reporter);

  return EXIT_SUCCESS;
}

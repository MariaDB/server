/* -*- c-basic-offset: 2; coding: utf-8 -*- */
/*
  Copyright (C) 2008  Kouhei Sutou <kou@cozmixng.org>

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

#ifndef __BENCH_REPORTER_H__
#define __BENCH_REPORTER_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define BENCH_TYPE_REPORTER            (bench_reporter_get_type())
#define BENCH_REPORTER(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), BENCH_TYPE_REPORTER, BenchReporter))
#define BENCH_REPORTER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), BENCH_TYPE_REPORTER, BenchReporterClass))
#define BENCH_IS_REPORTER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), BENCH_TYPE_REPORTER))
#define BENCH_IS_REPORTER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), BENCH_TYPE_REPORTER))
#define BENCH_REPORTER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), BENCH_TYPE_REPORTER, BenchReporterClass))

typedef struct _BenchReporter         BenchReporter;
typedef struct _BenchReporterClass    BenchReporterClass;

typedef void (*BenchSetupFunc)    (gpointer user_data);
typedef void (*BenchFunc)         (gpointer user_data);
typedef void (*BenchTeardownFunc) (gpointer user_data);

struct _BenchReporter
{
  GObject object;
};

struct _BenchReporterClass
{
  GObjectClass parent_class;
};

GType           bench_reporter_get_type  (void) G_GNUC_CONST;

BenchReporter  *bench_reporter_new       (void);
void            bench_reporter_register  (BenchReporter     *reporter,
                                          const gchar       *label,
                                          gint               n,
                                          BenchSetupFunc     bench_setup,
                                          BenchFunc          bench,
                                          BenchTeardownFunc  bench_teardown,
                                          gpointer           data);
void            bench_reporter_run       (BenchReporter     *reporter);

#endif /* __BENCH_REPORTER_H__ */



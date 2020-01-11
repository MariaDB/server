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

#include <string.h>

#include "bench-reporter.h"

typedef struct _BenchItem BenchItem;
struct _BenchItem
{
  gchar *label;
  gint n;
  BenchSetupFunc bench_setup;
  BenchFunc bench;
  BenchTeardownFunc bench_teardown;
  gpointer data;
};

static BenchItem *
bench_item_new(const gchar *label, gint n,
               BenchSetupFunc bench_setup,
               BenchFunc bench,
               BenchTeardownFunc bench_teardown,
               gpointer data)
{
  BenchItem *item;

  item = g_slice_new(BenchItem);

  item->label = g_strdup(label);
  item->n = n;
  item->bench_setup = bench_setup;
  item->bench = bench;
  item->bench_teardown = bench_teardown;
  item->data = data;

  return item;
}

static void
bench_item_free(BenchItem *item)
{
  if (item->label)
    g_free(item->label);

  g_slice_free(BenchItem, item);
}

#define BENCH_REPORTER_GET_PRIVATE(obj)                                 \
  (G_TYPE_INSTANCE_GET_PRIVATE((obj),                                   \
                               BENCH_TYPE_REPORTER,                     \
                               BenchReporterPrivate))

typedef struct _BenchReporterPrivate BenchReporterPrivate;
struct _BenchReporterPrivate
{
  GList *items;
};

G_DEFINE_TYPE(BenchReporter, bench_reporter, G_TYPE_OBJECT)

static void dispose        (GObject         *object);

static void
bench_reporter_class_init(BenchReporterClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS(klass);

  gobject_class->dispose = dispose;

  g_type_class_add_private(gobject_class, sizeof(BenchReporterPrivate));
}

static void
bench_reporter_init(BenchReporter *reporter)
{
  BenchReporterPrivate *priv;

  priv = BENCH_REPORTER_GET_PRIVATE(reporter);

  priv->items = NULL;
}

static void
dispose(GObject *object)
{
  BenchReporterPrivate *priv;

  priv = BENCH_REPORTER_GET_PRIVATE(object);

  if (priv->items) {
    g_list_foreach(priv->items, (GFunc)bench_item_free, NULL);
    g_list_free(priv->items);
    priv->items = NULL;
  }

  G_OBJECT_CLASS(bench_reporter_parent_class)->dispose(object);
}

BenchReporter *
bench_reporter_new(void)
{
  return g_object_new(BENCH_TYPE_REPORTER, NULL);
}

void
bench_reporter_register(BenchReporter *reporter,
                        const gchar *label, gint n,
                        BenchSetupFunc bench_setup,
                        BenchFunc bench,
                        BenchTeardownFunc bench_teardown,
                        gpointer data)
{
  BenchReporterPrivate *priv;

  priv = BENCH_REPORTER_GET_PRIVATE(reporter);

  priv->items = g_list_append(priv->items, bench_item_new(label, n,
                                                          bench_setup,
                                                          bench,
                                                          bench_teardown,
                                                          data));
}

#define INDENT "  "

static void
print_header(BenchReporterPrivate *priv, gint max_label_length)
{
  gint n_spaces;

  g_print(INDENT);
  for (n_spaces = max_label_length + strlen(": ");
       n_spaces > 0;
       n_spaces--) {
    g_print(" ");
  }
  g_print("(total)    ");
  g_print("(average)  ");
  g_print("(median)\n");
}

static void
print_label(BenchReporterPrivate *priv, BenchItem *item, gint max_label_length)
{
  gint n_left_spaces;

  g_print(INDENT);
  if (item->label) {
    n_left_spaces = max_label_length - strlen(item->label);
  } else {
    n_left_spaces = max_label_length;
  }
  for (; n_left_spaces > 0; n_left_spaces--) {
    g_print(" ");
  }
  if (item->label)
    g_print("%s", item->label);
  g_print(": ");
}

static void
report_elapsed_time(gdouble elapsed_time)
{
  gdouble one_second = 1.0;
  gdouble one_millisecond = one_second / 1000.0;
  gdouble one_microsecond = one_millisecond / 1000.0;

  if (elapsed_time < one_microsecond) {
    g_print("(%.8fms)", elapsed_time * 1000.0);
  } else if (elapsed_time < one_millisecond) {
    g_print("(%.4fms)", elapsed_time * 1000.0);
  } else {
    g_print("(%.4fs) ", elapsed_time);
  }
}

static gdouble
compute_total_elapsed_time(GArray *elapsed_times)
{
  guint i;
  gdouble total = 0.0;

  for (i = 0; i< elapsed_times->len; i++) {
    gdouble elapsed_time = g_array_index(elapsed_times, gdouble, i);
    total += elapsed_time;
  }

  return total;
}

static void
report_elapsed_time_total(GArray *elapsed_times)
{
  report_elapsed_time(compute_total_elapsed_time(elapsed_times));
}

static void
report_elapsed_time_average(GArray *elapsed_times)
{
  gdouble total;
  gdouble average;

  total = compute_total_elapsed_time(elapsed_times);
  average = total / elapsed_times->len;
  report_elapsed_time(average);
}

static gint
compare_elapsed_time(gconstpointer a, gconstpointer b)
{
  const gdouble *elapsed_time1 = a;
  const gdouble *elapsed_time2 = b;

  if (*elapsed_time1 > *elapsed_time2) {
    return 1;
  } else if (*elapsed_time1 < *elapsed_time2) {
    return -1;
  } else {
    return 0;
  }
}

static void
report_elapsed_time_median(GArray *elapsed_times)
{
  gdouble median;

  g_array_sort(elapsed_times, compare_elapsed_time);
  median = g_array_index(elapsed_times, gdouble, elapsed_times->len / 2);
  report_elapsed_time(median);
}

static void
report_elapsed_time_statistics(GArray *elapsed_times)
{
  report_elapsed_time_total(elapsed_times);
  g_print(" ");
  report_elapsed_time_average(elapsed_times);
  g_print(" ");
  report_elapsed_time_median(elapsed_times);
  g_print("\n");
}

static void
run_item(BenchReporterPrivate *priv, BenchItem *item, gint max_label_length)
{
  GTimer *timer;
  GArray *elapsed_times;
  gint i;

  print_label(priv, item, max_label_length);

  elapsed_times = g_array_new(FALSE, FALSE, sizeof(gdouble));

  timer = g_timer_new();
  for (i = 0; i < item->n; i++) {
    gdouble elapsed_time;
    if (item->bench_setup)
      item->bench_setup(item->data);
    g_timer_start(timer);
    item->bench(item->data);
    g_timer_stop(timer);
    elapsed_time = g_timer_elapsed(timer, NULL);
    g_array_append_val(elapsed_times, elapsed_time);
    if (item->bench_teardown)
      item->bench_teardown(item->data);
  }
  g_timer_destroy(timer);

  report_elapsed_time_statistics(elapsed_times);

  g_array_free(elapsed_times, TRUE);

}

void
bench_reporter_run(BenchReporter *reporter)
{
  BenchReporterPrivate *priv;
  GList *node;
  gint max_label_length = 0;

  priv = BENCH_REPORTER_GET_PRIVATE(reporter);
  for (node = priv->items; node; node = g_list_next(node)) {
    BenchItem *item = node->data;

    if (item->label)
      max_label_length = MAX(max_label_length, strlen(item->label));
  }

  print_header(priv, max_label_length);
  for (node = priv->items; node; node = g_list_next(node)) {
    BenchItem *item = node->data;

    run_item(priv, item, max_label_length);
  }
}

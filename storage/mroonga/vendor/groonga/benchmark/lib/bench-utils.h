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

#ifndef __BENCH_UTILS_H__
#define __BENCH_UTILS_H__

#include <glib.h>

G_BEGIN_DECLS

gboolean bench_utils_remove_path                 (const gchar  *path,
                                                  GError      **error);
gboolean bench_utils_remove_path_recursive       (const gchar  *path,
                                                  GError      **error);
void     bench_utils_remove_path_recursive_force (const gchar  *path);

G_END_DECLS

#endif /* __BENCH_UTILS_H__ */

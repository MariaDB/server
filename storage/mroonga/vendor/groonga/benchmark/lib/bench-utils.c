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

#include <errno.h>
#include <glib/gstdio.h>

#include "bench-utils.h"

gboolean
bench_utils_remove_path(const gchar *path, GError **error)
{
  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                "path doesn't exist: %s", path);
    return FALSE;
  }

  if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
    if (g_rmdir(path) == -1) {
      g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                  "can't remove directory: %s", path);
      return FALSE;
    }
  } else {
    if (g_unlink(path) == -1) {
      g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                  "can't remove path: %s", path);
      return FALSE;
    }
  }

  return TRUE;
}

gboolean
bench_utils_remove_path_recursive(const gchar *path, GError **error)
{
  if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
    GDir *dir;
    const gchar *name;

    dir = g_dir_open(path, 0, error);
    if (!dir)
      return FALSE;

    while ((name = g_dir_read_name(dir))) {
      const gchar *full_path;

      full_path = g_build_filename(path, name, NULL);
      if (!bench_utils_remove_path_recursive(full_path, error))
        return FALSE;
    }

    g_dir_close(dir);

    return bench_utils_remove_path(path, error);
  } else {
    return bench_utils_remove_path(path, error);
  }

  return TRUE;
}

void
bench_utils_remove_path_recursive_force(const gchar *path)
{
  bench_utils_remove_path_recursive(path, NULL);
}

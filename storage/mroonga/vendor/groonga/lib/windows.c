/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010-2017 Brazil

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

#include "grn.h"
#include "grn_windows.h"

#ifdef WIN32
static char *windows_base_dir = NULL;
const char *
grn_windows_base_dir(void)
{
  if (!windows_base_dir) {
    HMODULE dll;
    const wchar_t *dll_filename = GRN_DLL_FILENAME;
    wchar_t absolute_dll_filename[MAX_PATH];
    DWORD absolute_dll_filename_size;
    dll = GetModuleHandleW(dll_filename);
    absolute_dll_filename_size = GetModuleFileNameW(dll,
                                                    absolute_dll_filename,
                                                    MAX_PATH);
    if (absolute_dll_filename_size == 0) {
      windows_base_dir = grn_strdup_raw(".");
    } else {
      DWORD ansi_dll_filename_size;
      ansi_dll_filename_size =
        WideCharToMultiByte(CP_ACP, 0,
                            absolute_dll_filename, absolute_dll_filename_size,
                            NULL, 0, NULL, NULL);
      if (ansi_dll_filename_size == 0) {
        windows_base_dir = grn_strdup_raw(".");
      } else {
        char *path;
        windows_base_dir = malloc(ansi_dll_filename_size + 1);
        WideCharToMultiByte(CP_ACP, 0,
                            absolute_dll_filename, absolute_dll_filename_size,
                            windows_base_dir, ansi_dll_filename_size,
                            NULL, NULL);
        windows_base_dir[ansi_dll_filename_size] = '\0';
        if ((path = strrchr(windows_base_dir, '\\'))) {
          *path = '\0';
        }
        path = strrchr(windows_base_dir, '\\');
        if (path && (grn_strcasecmp(path + 1, "bin") == 0 ||
                     grn_strcasecmp(path + 1, "lib") == 0)) {
          *path = '\0';
        } else {
          path = windows_base_dir + strlen(windows_base_dir);
          *path = '\0';
        }
        for (path = windows_base_dir; *path; path++) {
          if (*path == '\\') {
            *path = '/';
          }
        }
      }
    }
  }
  return windows_base_dir;
}

UINT
grn_windows_encoding_to_code_page(grn_encoding encoding)
{
  UINT code_page;

  switch (encoding) {
  case GRN_ENC_EUC_JP :
    code_page = 20932;
    break;
  case GRN_ENC_UTF8 :
    code_page = CP_UTF8;
    break;
  case GRN_ENC_SJIS :
    code_page = 932;
    break;
  case GRN_ENC_LATIN1 :
    code_page = 1252;
    break;
  case GRN_ENC_KOI8R :
    code_page = 20866;
    break;
  default :
    code_page = CP_ACP;
    break;
  }

  return code_page;
}
#endif /* WIN32 */

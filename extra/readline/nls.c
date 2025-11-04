/* nls.c -- skeletal internationalization code. */

/* Copyright (C) 1996 Free Software Foundation, Inc.

   This file is part of the GNU Readline Library, a library for
   reading lines of text with interactive input and history editing.

   The GNU Readline Library is free software; you can redistribute it
   and/or modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2, or
   (at your option) any later version.

   The GNU Readline Library is distributed in the hope that it will be
   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   The GNU General Public License is often shipped with GNU software, and
   is generally kept in a file called COPYING or LICENSE.  If you do not
   have a copy of the license, write to the Free Software Foundation,
   51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA. */
#define READLINE_LIBRARY

#if defined (HAVE_CONFIG_H)
#  include "config_readline.h"
#endif

#include <sys/types.h>

#include <stdio.h>

#if defined (HAVE_UNISTD_H)
#  include <unistd.h>
#endif /* HAVE_UNISTD_H */

#if defined (HAVE_STDLIB_H)
#  include <stdlib.h>
#else
#  include "ansi_stdlib.h"
#endif /* HAVE_STDLIB_H */

#if defined (HAVE_LOCALE_H)
#  include <locale.h>
#endif

#include <ctype.h>

#include "rldefs.h"
#include "readline.h"
#include "rlshell.h"
#include "rlprivate.h"

static char *_rl_get_locale_var PARAMS((const char *));

static char *
_rl_get_locale_var (v)
     const char *v;
{
  char *lspec;

  lspec = sh_get_env_value ("LC_ALL");
  if (lspec == 0 || *lspec == 0)
    lspec = sh_get_env_value (v);
  if (lspec == 0 || *lspec == 0)
    lspec = sh_get_env_value ("LANG");

  return lspec;
}
  
/* Check for LC_ALL, LC_CTYPE, and LANG and use the first with a value
   to decide the defaults for 8-bit character input and output.  Returns
   1 if we set eight-bit mode. */
int
_rl_init_eightbit ()
{
/* If we have setlocale(3), just check the current LC_CTYPE category
   value, and go into eight-bit mode if it's not C or POSIX. */
  const char *lspec;
  char *t;

  /* Set the LC_CTYPE locale category from environment variables. */
  lspec = _rl_get_locale_var ("LC_CTYPE");
  /* Since _rl_get_locale_var queries the right environment variables,
     we query the current locale settings with setlocale(), and, if
     that doesn't return anything, we set lspec to the empty string to
     force the subsequent call to setlocale() to define the `native'
     environment. */
  if (lspec == 0 || *lspec == 0)
    lspec = setlocale (LC_CTYPE, (char *)NULL);
  if (lspec == 0)
    lspec = "";
  t = setlocale (LC_CTYPE, lspec);

  if (t && *t && (t[0] != 'C' || t[1]) && (STREQ (t, "POSIX") == 0))
    {
      _rl_meta_flag = 1;
      _rl_convert_meta_chars_to_ascii = 0;
      _rl_output_meta_chars = 1;
      return (1);
    }
  else
    return (0);
}

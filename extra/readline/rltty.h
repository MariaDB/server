/* rltty.h - tty driver-related definitions used by some library files. */

/* Copyright (C) 1995 Free Software Foundation, Inc.

   This file contains the Readline Library (the Library), a set of
   routines for providing Emacs style line input to programs that ask
   for it.

   The Library is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   The Library is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   The GNU General Public License is often shipped with GNU software, and
   is generally kept in a file called COPYING or LICENSE.  If you do not
   have a copy of the license, write to the Free Software Foundation,
   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA. */

#if !defined (_RLTTY_H_)
#define _RLTTY_H_

/* Posix systems use termios and the Posix signal functions. */
#if defined (TERMIOS_TTY_DRIVER)
#  include <termios.h>
#endif /* TERMIOS_TTY_DRIVER */

/* System V machines use termio. */
#if defined (TERMIO_TTY_DRIVER)
#  include <termio.h>
#  if !defined (TCOON)
#    define TCOON 1
#  endif
#endif /* TERMIO_TTY_DRIVER */

/* Other (BSD) machines use sgtty. */
#if defined (NEW_TTY_DRIVER)
#  include <sgtty.h>
#endif

#include "rlwinsize.h"

/* Define _POSIX_VDISABLE if we are not using the `new' tty driver and
   it is not already defined.  It is used both to determine if a
   special character is disabled and to disable certain special
   characters.  Posix systems should set to 0, USG systems to -1. */
#if !defined (NEW_TTY_DRIVER) && !defined (_POSIX_VDISABLE)
#  if defined (_SVR4_VDISABLE)
#    define _POSIX_VDISABLE _SVR4_VDISABLE
#  else
#    if defined (_POSIX_VERSION)
#      define _POSIX_VDISABLE 0
#    else /* !_POSIX_VERSION */
#      define _POSIX_VDISABLE -1
#    endif /* !_POSIX_VERSION */
#  endif /* !_SVR4_DISABLE */
#endif /* !NEW_TTY_DRIVER && !_POSIX_VDISABLE */

typedef struct _rl_tty_chars {
  unsigned char t_eof;
  unsigned char t_eol;
  unsigned char t_eol2;
  unsigned char t_erase;
  unsigned char t_werase;
  unsigned char t_kill;
  unsigned char t_reprint;
  unsigned char t_intr;
  unsigned char t_quit;
  unsigned char t_susp;
  unsigned char t_dsusp;
  unsigned char t_start;
  unsigned char t_stop;
  unsigned char t_lnext;
  unsigned char t_flush;
  unsigned char t_status;
} _RL_TTY_CHARS;

#endif /* _RLTTY_H_ */

/* Copyright (c) 2023, MariaDB Corporation.

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

#ifndef MY_ALLOCA_INCLUDED
#define MY_ALLOCA_INCLUDED

#ifdef _WIN32
#include <malloc.h> /*for alloca*/
/*
  MSVC may define "alloca" when compiling in /Ze mode
  (with extensions from Microsoft), but otherwise only
  the _alloca function is defined:
*/
#ifndef alloca
#define alloca _alloca
#endif
#else
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#endif

#if defined(_AIX) && !defined(__GNUC__) && !defined(_AIX43)
#pragma alloca
#endif /* _AIX */

/*
  If the GCC/LLVM compiler from the MinGW is used,
  alloca may not be defined when using the MSVC CRT:
*/
#if defined(__GNUC__) && !defined(HAVE_ALLOCA_H) && !defined(alloca)
#define alloca __builtin_alloca
#endif /* GNUC */

#endif /* MY_ALLOCA_INCLUDED */

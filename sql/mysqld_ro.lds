/*
   Copyright (c) 2026, MariaDB plc, MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/*
  Give the "ro_after_init" section a page-aligned span of its own, so that the
  variables placed there (with READ_ONLY_SYSVAR tag) can be mprotect()-ed
  once server initialization is complete, without affecting neighbouring data.
*/
SECTIONS
{
  ro_after_init ALIGN(CONSTANT(MAXPAGESIZE)) :
  {
    ro_after_init_start = .;
    *(ro_after_init)
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    ro_after_init_end = .;
  }
} INSERT AFTER .data;

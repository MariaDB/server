/*
   Copyright (c) 2009, 2021, MariaDB

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

/*
  Interface to indexed virtual column substitution module
*/

/* Do substitution in one join */
bool substitute_indexed_vcols_for_join(JOIN *join);

/*
  Do substitution for one table and condition. This is for single-table
  UPDATE/DELETE.
*/
bool substitute_indexed_vcols_for_table(TABLE *table, Item *item);


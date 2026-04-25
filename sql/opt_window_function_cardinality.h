/*
   Copyright (c) 2026, MariaDB

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

#ifndef OPT_WINDOW_FUNCTION_CARDINALITY_INCLUDED
#define OPT_WINDOW_FUNCTION_CARDINALITY_INCLUDED

bool  est_derived_window_fn_cardinality(st_select_lex* derived,
                                        ulong *out_records,
                                        Field **reg_fields,
                                        uint key_parts);

#endif /* OPT_WINDOW_FUNCTION_CARDINALITY_INCLUDED */


#ifndef OPT_HINTS_STRUCTS_H
#define OPT_HINTS_STRUCTS_H
/*
   Copyright (c) 2024, MariaDB

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

/**
  Hint types, MAX_HINT_ENUM should be always last.
  This enum should be synchronized with opt_hint_info
  array(see opt_hints.cc).
*/
enum opt_hints_enum
{
  BKA_HINT_ENUM= 0,
  BNL_HINT_ENUM,
  ICP_HINT_ENUM,
  MRR_HINT_ENUM,
  NO_RANGE_HINT_ENUM,
  QB_NAME_HINT_ENUM,
  MAX_EXEC_TIME_HINT_ENUM,
  SEMIJOIN_HINT_ENUM,
  SUBQUERY_HINT_ENUM,
  JOIN_PREFIX_HINT_ENUM,
  JOIN_SUFFIX_HINT_ENUM,
  JOIN_ORDER_HINT_ENUM,
  JOIN_FIXED_ORDER_HINT_ENUM,
  DERIVED_CONDITION_PUSHDOWN_HINT_ENUM,
  MERGE_HINT_ENUM,
  SPLIT_MATERIALIZED_HINT_ENUM,
  INDEX_HINT_ENUM,
  JOIN_INDEX_HINT_ENUM,
  GROUP_INDEX_HINT_ENUM,
  ORDER_INDEX_HINT_ENUM,
  ROWID_FILTER_HINT_ENUM,
  INDEX_MERGE_HINT_ENUM,
  MAX_HINT_ENUM // This one must be the last in the list
};

enum class hint_resolution_stage
{
  EARLY,
  LATE,
  NOT_SET /* If specified for a hint at `st_opt_hint_info`, then it means that
             the resolution stage is not determined by the hint type but
             depends on the hint body. For example, simple QB_NAME is resolved
             in EARLY stage while QB_NAME with path - in LATE stage.
             The particular resolution method is responsible in this case */
};

#endif /* OPT_HINTS_STRUCTS_H */

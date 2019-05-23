/*
  Copyright(C) 2011 Kentoku SHIBA
  Copyright(C) 2014-2015 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#ifndef MRN_ERR_H_
#define MRN_ERR_H_

#define ER_MRN_INVALID_TABLE_PARAM_NUM 16501
#define ER_MRN_INVALID_TABLE_PARAM_STR "The table parameter '%-.64s' is invalid"
#define ER_MRN_CHARSET_NOT_SUPPORT_NUM 16502
#define ER_MRN_CHARSET_NOT_SUPPORT_STR "The character set '%s[%s]' is not supported by groonga"
#define ER_MRN_GEOMETRY_NOT_SUPPORT_NUM 16503
#define ER_MRN_GEOMETRY_NOT_SUPPORT_STR "This geometry type is not supported. Groonga is supported point only"
#define ER_MRN_ERROR_FROM_GROONGA_NUM 16504
#define ER_MRN_ERROR_FROM_GROONGA_STR "Error from Groonga [%s]"
#define ER_MRN_INVALID_NULL_VALUE_NUM 16505
#define ER_MRN_INVALID_NULL_VALUE_STR "NULL value can't be used for %s"
#define ER_MRN_UNSUPPORTED_COLUMN_FLAG_NUM 16506
#define ER_MRN_UNSUPPORTED_COLUMN_FLAG_STR \
  "The column flag '%-.64s' is unsupported. It is ignored"
#define ER_MRN_INVALID_COLUMN_FLAG_NUM 16507
#define ER_MRN_INVALID_COLUMN_FLAG_STR \
  "The column flag '%-.64s' is invalid. It is ignored"
#define ER_MRN_INVALID_INDEX_FLAG_NUM 16508
#define ER_MRN_INVALID_INDEX_FLAG_STR \
  "The index flag '%-.64s' is invalid. It is ignored"
#define ER_MRN_KEY_BASED_ON_GENERATED_VIRTUAL_COLUMN_NUM 16509
#define ER_MRN_KEY_BASED_ON_GENERATED_VIRTUAL_COLUMN_STR \
  "Index for virtual generated column is not supported: %s"

#endif /* MRN_ERR_H_ */

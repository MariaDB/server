/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Kouhei Sutou <kou@clear-code.com>

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

#ifndef MRN_VARIABLES_HPP_
#define MRN_VARIABLES_HPP_

#include "mrn_mysql_compat.h"

#ifdef MRN_HAVE_PSI_MEMORY_KEY
extern PSI_memory_key mrn_memory_key;
#endif

namespace mrn {
  namespace variables {
    enum BooleanModeSyntaxFlag {
      BOOLEAN_MODE_SYNTAX_FLAG_DEFAULT           = (1 << 0),
      BOOLEAN_MODE_SYNTAX_FLAG_SYNTAX_QUERY      = (1 << 1),
      BOOLEAN_MODE_SYNTAX_FLAG_SYNTAX_SCRIPT     = (1 << 2),
      BOOLEAN_MODE_SYNTAX_FLAG_ALLOW_COLUMN      = (1 << 3),
      BOOLEAN_MODE_SYNTAX_FLAG_ALLOW_UPDATE      = (1 << 4),
      BOOLEAN_MODE_SYNTAX_FLAG_ALLOW_LEADING_NOT = (1 << 5)
    };

    ulonglong get_boolean_mode_syntax_flags(THD *thd);

    enum ActionOnError {
      ACTION_ON_ERROR_ERROR,
      ACTION_ON_ERROR_ERROR_AND_LOG,
      ACTION_ON_ERROR_IGNORE,
      ACTION_ON_ERROR_IGNORE_AND_LOG,
    };

    ActionOnError get_action_on_fulltext_query_error(THD *thd);
  }
}

#endif /* MRN_VARIABLES_HPP_ */

/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010 Tetsuro IKEDA
  Copyright(C) 2011 Kentoku SHIBA
  Copyright(C) 2011-2012 Kouhei Sutou <kou@clear-code.com>

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

#ifndef MRN_CONSTANTS_HPP_
#define MRN_CONSTANTS_HPP_

#include <limits.h>

#include <groonga.h>

#define MRN_BUFFER_SIZE 1024
#define MRN_MAX_KEY_SIZE GRN_TABLE_MAX_KEY_SIZE
#if defined(MAX_PATH)
#  define MRN_MAX_PATH_SIZE (MAX_PATH + 1)
#elif defined(PATH_MAX)
#  define MRN_MAX_PATH_SIZE (PATH_MAX)
#elif defined(MAXPATHLEN)
#  define MRN_MAX_PATH_SIZE (MAXPATHLEN)
#else
#  define MRN_MAX_PATH_SIZE (256)
#endif
#define MRN_DB_FILE_SUFFIX ".mrn"
#define MRN_LOG_FILE_PATH "groonga.log"
#define MRN_COLUMN_NAME_ID "_id"
#define MRN_COLUMN_NAME_KEY "_key"
#define MRN_COLUMN_NAME_SCORE "_score"
#ifndef MRN_DEFAULT_TOKENIZER
#  define MRN_DEFAULT_TOKENIZER "TokenBigram"
#endif

#endif /* MRN_CONSTANTS_HPP_ */

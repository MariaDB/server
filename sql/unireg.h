#ifndef UNIREG_INCLUDED
#define UNIREG_INCLUDED

/*
   Copyright (c) 2000, 2011, Oracle and/or its affiliates.

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


#include <mysql_version.h>                      /* FRM_VER */

/*  Extra functions used by unireg library */

#ifndef NO_ALARM_LOOP
#define NO_ALARM_LOOP		/* lib5 and popen can't use alarm */
#endif

/* These paths are converted to other systems (WIN95) before use */

#define LANGUAGE	"english/"
#define ERRMSG_FILE	"errmsg.sys"
#define TEMP_PREFIX	"MY"
#define LOG_PREFIX	"ML"
#define PROGDIR		"bin/"
#ifndef MYSQL_DATADIR
#define MYSQL_DATADIR		"data/"
#endif
#ifndef SHAREDIR
#define SHAREDIR	"share/"
#endif
#ifndef PLUGINDIR
#define PLUGINDIR	"lib/plugin"
#endif

#define MAX_ERROR_RANGES 4  /* 1000-2000, 2000-3000, 3000-4000, 4000-5000 */
#define ERRORS_PER_RANGE 1000

#define DEFAULT_ERRMSGS           my_default_lc_messages->errmsgs->errmsgs
#define CURRENT_THD_ERRMSGS       (current_thd)->variables.errmsgs

#ifndef mysqld_error_find_printf_error_used
#define ER_DEFAULT(X) DEFAULT_ERRMSGS[((X)-ER_ERROR_FIRST) / ERRORS_PER_RANGE][(X)% ERRORS_PER_RANGE]
#define ER_THD(thd,X) ((thd)->variables.errmsgs[((X)-ER_ERROR_FIRST) / ERRORS_PER_RANGE][(X) % ERRORS_PER_RANGE])
#define ER(X)         ER_THD(current_thd, (X))
#endif
#define ER_THD_OR_DEFAULT(thd,X) ((thd) ? ER_THD(thd, (X)) : ER_DEFAULT(X))

#define SPECIAL_USE_LOCKS	1		/* Lock used databases */
#define SPECIAL_NO_NEW_FUNC	2		/* Skip new functions */
#define SPECIAL_SKIP_SHOW_DB    4               /* Don't allow 'show db' */
#define SPECIAL_WAIT_IF_LOCKED	8		/* Wait if locked database */
#define SPECIAL_SAME_DB_NAME   16		/* form name = file name */
#define SPECIAL_ENGLISH        32		/* English error messages */
#define SPECIAL_NO_RESOLVE     64		/* Obsolete */
#define SPECIAL_NO_PRIOR	128		/* Obsolete */
#define SPECIAL_BIG_SELECTS	256		/* Don't use heap tables */
#define SPECIAL_NO_HOST_CACHE	512		/* Don't cache hosts */
#define SPECIAL_SHORT_LOG_FORMAT 1024
#define SPECIAL_SAFE_MODE	2048
#define SPECIAL_LOG_QUERIES_NOT_USING_INDEXES 4096 /* Obsolete */

	/* Extern defines */
#define store_record(A,B) memcpy((A)->B,(A)->record[0],(size_t) (A)->s->reclength)
#define restore_record(A,B) memcpy((A)->record[0],(A)->B,(size_t) (A)->s->reclength)
#define cmp_record(A,B) memcmp((A)->record[0],(A)->B,(size_t) (A)->s->reclength)
#define empty_record(A) { \
                          restore_record((A),s->default_values); \
                          if ((A)->s->null_bytes) \
                            bfill((A)->null_flags,(A)->s->null_bytes,255); \
                        }

	/* Defines for use with openfrm, openprt and openfrd */

#define READ_ALL               (1 <<  0) /* openfrm: Read all parameters */
#define EXTRA_RECORD           (1 <<  3) /* Reserve space for an extra record */
#define DELAYED_OPEN           (1 << 12) /* Open table later */
#define OPEN_VIEW_NO_PARSE     (1 << 14) /* Open frm only if it's a view,
                                            but do not parse view itself */
/**
  This flag is used in function get_all_tables() which fills
  I_S tables with data which are retrieved from frm files and storage engine
  The flag means that we need to open FRM file only to get necessary data.
*/
#define OPEN_FRM_FILE_ONLY     (1 << 15)
/**
  This flag is used in function get_all_tables() which fills
  I_S tables with data which are retrieved from frm files and storage engine
  The flag means that we need to process tables only to get necessary data.
  Views are not processed.
*/
#define OPEN_TABLE_ONLY        (1 << 16)
/**
  This flag is used in function get_all_tables() which fills
  I_S tables with data which are retrieved from frm files and storage engine
  The flag means that we need to process views only to get necessary data.
  Tables are not processed.
*/
#define OPEN_VIEW_ONLY         (1 << 17)
/**
  This flag is used in function get_all_tables() which fills
  I_S tables with data which are retrieved from frm files and storage engine.
  The flag means that we need to open a view using
  open_normal_and_derived_tables() function.
*/
#define OPEN_VIEW_FULL         (1 << 18)
/**
  This flag is used in function get_all_tables() which fills
  I_S tables with data which are retrieved from frm files and storage engine.
  The flag means that I_S table uses optimization algorithm.
*/
#define OPTIMIZE_I_S_TABLE     (1 << 19)
/**
  This flag is used to instruct tdc_open_view() to check metadata version.
*/
#define CHECK_METADATA_VERSION (1 << 20)

/*
  The flag means that we need to process trigger files only.
*/
#define OPEN_TRIGGER_ONLY      (1 << 21)

/*
  Minimum length pattern before Turbo Boyer-Moore is used
  for SELECT "text" LIKE "%pattern%", excluding the two
  wildcards in class Item_func_like.
*/
#define MIN_TURBOBM_PATTERN_LEN 3

/* 
   Defines for binary logging.
   Do not decrease the value of BIN_LOG_HEADER_SIZE.
   Do not even increase it before checking code.
*/

#define BIN_LOG_HEADER_SIZE    4 

#define DEFAULT_KEY_CACHE_NAME "default"


/* Include prototypes for unireg */

#include "mysqld_error.h"
#include "structs.h"				/* All structs we need */
#include "sql_list.h"                           /* List<> */
#include "field.h"                              /* Create_field */

/*
  Types of values in the MariaDB extra2 frm segment.
  Each value is written as
    type:       1 byte
    length:     1 byte  (1..255) or \0 and 2 bytes.
    binary value of the 'length' bytes.

  Older MariaDB servers can ignore values of unknown types if
  the type code is less than 128 (EXTRA2_ENGINE_IMPORTANT).
  Otherwise older (but newer than 10.0.1) servers are required
  to report an error.
*/
enum extra2_frm_value_type {
  EXTRA2_TABLEDEF_VERSION=0,
  EXTRA2_DEFAULT_PART_ENGINE=1,
  EXTRA2_GIS=2,
  EXTRA2_APPLICATION_TIME_PERIOD=3,
  EXTRA2_PERIOD_FOR_SYSTEM_TIME=4,
  EXTRA2_INDEX_FLAGS=5,

#define EXTRA2_ENGINE_IMPORTANT 128

  EXTRA2_ENGINE_TABLEOPTS=128,
  EXTRA2_FIELD_FLAGS=129,
  EXTRA2_FIELD_DATA_TYPE_INFO=130,
  EXTRA2_PERIOD_WITHOUT_OVERLAPS=131,
};

enum extra2_field_flags {
  VERS_OPTIMIZED_UPDATE= 1 << INVISIBLE_MAX_BITS,
};

enum extra2_index_flags {
  EXTRA2_DEFAULT_INDEX_FLAGS,
  EXTRA2_IGNORED_KEY
};


static inline size_t extra2_read_len(const uchar **extra2, const uchar *end)
{
  size_t length= *(*extra2)++;
  if (length)
    return length;

  if ((*extra2) + 2 >= end)
    return 0;
  length= uint2korr(*extra2);
  (*extra2)+= 2;
  if (length < 256 || *extra2 + length > end)
    return 0;
  return length;
}

LEX_CUSTRING build_frm_image(THD *thd, const LEX_CSTRING &table,
                             HA_CREATE_INFO *create_info,
                             List<Create_field> &create_fields,
                             uint keys, KEY *key_info, handler *db_file);

#define FRM_HEADER_SIZE 64
#define FRM_FORMINFO_SIZE 288
#define FRM_MAX_SIZE (1024*1024)

static inline bool is_binary_frm_header(uchar *head)
{
  return head[0] == 254
      && head[1] == 1
      && head[2] >= FRM_VER
      && head[2] <= FRM_VER_CURRENT;
}

#endif

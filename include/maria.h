/* Copyright (C) 2006-2008 MySQL AB, 2008-2009 Sun Microsystems, Inc.
   Copyright (c) 2009, 2019, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/* This file should be included when using maria functions */

#ifndef _maria_h
#define _maria_h
#include <my_base.h>
#include <m_ctype.h>
#include "my_compare.h"
#include "ft_global.h"
#include <myisamchk.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define MARIA_UNIQUE_HASH_LENGTH	4
extern my_bool maria_delay_key_write;
uint maria_max_key_length(void);
#define maria_max_key_segments() HA_MAX_KEY_SEG

struct st_maria_bit_buff;
struct st_maria_page;
struct st_maria_s_param;
struct st_maria_share;
typedef struct st_maria_decode_tree  MARIA_DECODE_TREE;
typedef struct st_maria_handler MARIA_HA;
typedef struct st_maria_key MARIA_KEY;
typedef ulonglong MARIA_RECORD_POS;

typedef struct st_maria_keydef          /* Key definition with open & info */
{
  struct st_maria_share *share;         /* Pointer to base (set in open) */
  mysql_rwlock_t root_lock;                  /* locking of tree */
  uint16 keysegs;                       /* Number of key-segment */
  uint16 flag;                          /* NOSAME, PACK_USED */

  uint8 key_alg;                        /* BTREE, RTREE */
  uint8 key_nr;				/* key number (auto) */
  uint16 block_length;                  /* Length of keyblock (auto) */
  uint16 underflow_block_length;        /* When to execute underflow */
  uint16 keylength;                     /* Tot length of keyparts (auto) */
  uint16 minlength;                     /* min length of (packed) key (auto) */
  uint16 maxlength;                     /* max length of (packed) key (auto) */
  uint16 max_store_length;              /* Size to store key + overhead */
  uint32 write_comp_flag;		/* compare flag for write key (auto) */
  uint32 version;                       /* For concurrent read/write */
  uint32 ftkey_nr;                      /* full-text index number */

  HA_KEYSEG *seg, *end;
  struct st_mysql_ftparser *parser;     /* Fulltext [pre]parser */
  int (*bin_search)(const MARIA_KEY *key, const struct st_maria_page *page,
                    uint32 comp_flag, uchar **ret_pos, uchar *buff,
                    my_bool *was_last_key);
  uint (*get_key)(MARIA_KEY *key, uint page_flag, uint nod_flag,
                  uchar **page);
  uchar *(*skip_key)(MARIA_KEY *key, uint page_flag, uint nod_flag,
                     uchar *page);
  int (*pack_key)(const MARIA_KEY *key, uint nod_flag,
		  uchar *next_key, uchar *org_key, uchar *prev_key,
		  struct st_maria_s_param *s_temp);
  void (*store_key)(struct st_maria_keydef *keyinfo, uchar *key_pos,
		    struct st_maria_s_param *s_temp);
  my_bool (*ck_insert)(MARIA_HA *inf, MARIA_KEY *key);
  my_bool (*ck_delete)(MARIA_HA *inf, MARIA_KEY *klen);
  MARIA_KEY *(*make_key)(MARIA_HA *info, MARIA_KEY *int_key, uint keynr,
                         uchar *key, const uchar *record,
                         MARIA_RECORD_POS filepos, ulonglong trid);
} MARIA_KEYDEF;


typedef struct st_maria_unique_def	/* Segment definition of unique */
{
  uint16 keysegs;                       /* Number of key-segment */
  uint8 key;                            /* Mapped to which key */
  uint8 null_are_equal;
  HA_KEYSEG *seg, *end;
} MARIA_UNIQUEDEF;

/*
  Note that null markers should always be first in a row !
  When creating a column, one should only specify:
  type, length, null_bit and null_pos
*/

typedef struct st_maria_columndef		/* column information */
{
  enum en_fieldtype type;
  uint32 offset;				/* Offset to position in row */
  uint16 length;				/* length of field */
  uint16 column_nr;
  /* Intern variable (size of total storage area for the row) */
  uint16 fill_length;
  uint16 null_pos;				/* Position for null marker */
  uint16 empty_pos;                             /* Position for empty marker */
  uint8 null_bit;				/* If column may be NULL */
  /* Intern. Set if column should be zero packed (part of empty_bits) */
  uint8 empty_bit;

#ifndef NOT_PACKED_DATABASES
  void(*unpack)(struct st_maria_columndef *rec,
                struct st_maria_bit_buff *buff,
                uchar *start, uchar *end);
  enum en_fieldtype base_type;
  uint space_length_bits, pack_type;
  MARIA_DECODE_TREE *huff_tree;
#endif
} MARIA_COLUMNDEF;


typedef struct st_maria_create_info
{
  const char *index_file_name, *data_file_name; /* If using symlinks */
  ha_rows max_rows;
  ha_rows reloc_rows;
  ulonglong auto_increment;
  ulonglong data_file_length;
  ulonglong key_file_length;
  ulong s3_block_size;
  /* Size of null bitmap at start of row */
  uint null_bytes;
  uint old_options;
  uint compression_algorithm;
  enum data_file_type org_data_file_type;
  uint16 language;
  my_bool with_auto_increment, transactional, encrypted;
} MARIA_CREATE_INFO;

extern int maria_create(const char *name, enum data_file_type record_type,
                        uint keys, MARIA_KEYDEF *keydef,
                        uint columns, MARIA_COLUMNDEF *columndef,
                        uint uniques, MARIA_UNIQUEDEF *uniquedef,
                        MARIA_CREATE_INFO *create_info, uint flags);

#ifdef	__cplusplus
}
#endif
#endif


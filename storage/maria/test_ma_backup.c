/* Copyright (C) 2018, 2021, MariaDB corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/* Code for doing backups of Aria tables */

/******************************************************************************
  Testing ma_backup interface
  Table creation code is taken from ma_test1
******************************************************************************/

#define ROWS_IN_TEST 100000

#include "maria_def.h"
#include "ma_blockrec.h"                        /* PAGE_SUFFIX_SIZE */
#include "ma_checkpoint.h"
#include <aria_backup.h>

static int silent;
static int create_test_table(const char *table_name, int stage);
static int copy_table(const char *table_name, int stage);
static void create_record(uchar *record,uint rownr);

int main(int argc __attribute__((unused)), char *argv[])
{
  int error= 1;
  int i;
  char buff[FN_REFLEN];
#ifdef SAFE_MUTEX
  safe_mutex_deadlock_detector= 1;
#endif
  MY_INIT(argv[0]);
  maria_data_root= ".";

  /* Maria requires that we always have a page cache */
  if (maria_init() ||
      (init_pagecache(maria_pagecache, maria_block_size * 2000, 0, 0,
                      maria_block_size, 0, MY_WME) == 0) ||
      ma_control_file_open(TRUE, TRUE, TRUE) ||
      (init_pagecache(maria_log_pagecache,
                      TRANSLOG_PAGECACHE_SIZE, 0, 0,
                      TRANSLOG_PAGE_SIZE, 0, MY_WME) == 0) ||
      translog_init(maria_data_root, TRANSLOG_FILE_SIZE,
                    0, 0, maria_log_pagecache,
                    TRANSLOG_DEFAULT_FLAGS, 0) ||
      (trnman_init(0) || ma_checkpoint_init(0)))
  {
    fprintf(stderr, "Error in initialization\n");
    exit(1);
  }
  init_thr_lock();

  fn_format(buff, "test_copy", maria_data_root, "", MYF(0));

  for (i= 0; i < 5 ; i++)
  {
    printf("Stage: %d\n", i);
    fflush(stdout);
    if (create_test_table(buff, i))
      goto err;
    if (copy_table(buff, i))
      goto err;
  }
  error= 0;
  printf("test ok\n");
err:
  if (error)
    fprintf(stderr, "Test %i failed\n", i);
  maria_end();
  my_uuid_end();
  my_end(MY_CHECK_ERROR);
  exit(error);
}


/**
   Example of how to read an Aria table
*/

static int copy_table(const char *table_name, int stage)
{
  char old_name[FN_REFLEN];
  uchar *copy_buffer= 0;
  ARIA_TABLE_CAPABILITIES cap;
  ulonglong block;
  File org_file= -1;
  int error= 1;

  strxmov(old_name, table_name, ".MAI", NullS);

  if ((org_file= my_open(old_name,
                         O_RDONLY | O_SHARE | O_NOFOLLOW | O_CLOEXEC,
                         MYF(MY_WME))) < 0)
    goto err;
  if ((error= aria_get_capabilities(org_file, &cap)))
  {
    fprintf(stderr, "aria_get_capabilities failed:  %d\n", error);
    goto err;
  }

  printf("- Capabilities read. oneline_backup_safe: %d\n",
         cap.online_backup_safe);
  printf("- Copying index file\n");

  copy_buffer= my_malloc(PSI_NOT_INSTRUMENTED, cap.block_size, MYF(0));
  for (block= 0 ; ; block++)
  {
    if ((error= aria_read_index(org_file, &cap, block, copy_buffer) ==
         HA_ERR_END_OF_FILE))
      break;
    if (error)
    {
      fprintf(stderr, "aria_read_index failed:  %d\n", error);
      goto err;
    }
  }
  my_close(org_file, MYF(MY_WME));


  printf("- Copying data file\n");
  strxmov(old_name, table_name, ".MAD", NullS);
  if ((org_file= my_open(old_name, O_RDONLY | O_SHARE | O_NOFOLLOW | O_CLOEXEC,
                         MYF(MY_WME))) < 0)
    goto err;

  for (block= 0 ; ; block++)
  {
    size_t length;
    if ((error= aria_read_data(org_file, &cap, block, copy_buffer,
                               &length) == HA_ERR_END_OF_FILE))
      break;
    if (error)
    {
      fprintf(stderr, "aria_read_index failed:  %d\n", error);
      goto err;
    }
  }
  error= 0;

err:
  my_free(copy_buffer);
  if (org_file >= 0)
    my_close(org_file, MYF(MY_WME));
  if (error)
    fprintf(stderr, "Failed in copy_table stage: %d\n", stage);
  return error;
}


/* Code extracted from ma_test1.c */
#define MAX_REC_LENGTH 1024

static MARIA_COLUMNDEF recinfo[4];
static MARIA_KEYDEF keyinfo[10];
static HA_KEYSEG keyseg[10];
static HA_KEYSEG uniqueseg[10];


/**
   Create a test table and fill it with some data
*/

static int create_test_table(const char *table_name, int type_of_table)
{
  MARIA_HA *file;
  int i,error,uniques=0;
  int key_field=FIELD_SKIP_PRESPACE,extra_field=FIELD_SKIP_ENDSPACE;
  int key_type=HA_KEYTYPE_NUM;
  int create_flag=0;
  uint pack_seg=0, pack_keys= 0;
  uint key_length;
  uchar record[MAX_REC_LENGTH];
  MARIA_UNIQUEDEF uniquedef;
  MARIA_CREATE_INFO create_info;
  enum data_file_type record_type= DYNAMIC_RECORD;
  my_bool null_fields= 0, unique_key= 0;
  my_bool opt_unique= 0;
  my_bool transactional= 0;

  key_length= 12;
  switch (type_of_table) {
  case 0:
    break;
  case 1:
    create_flag|= HA_CREATE_CHECKSUM | HA_CREATE_PAGE_CHECKSUM;
    break;
  case 2:                                       /* transactional */
    create_flag|= HA_CREATE_CHECKSUM | HA_CREATE_PAGE_CHECKSUM;
    record_type= BLOCK_RECORD;
    transactional= 1;
    break;
  case 3:                                       /* transactional */
    create_flag|= HA_CREATE_CHECKSUM | HA_CREATE_PAGE_CHECKSUM;
    record_type= BLOCK_RECORD;
    transactional= 1;
    key_field=FIELD_VARCHAR;			/* varchar keys */
    extra_field= FIELD_VARCHAR;
    key_type= HA_KEYTYPE_VARTEXT1;
    pack_seg|= HA_VAR_LENGTH_PART;
    null_fields= 1;
    break;
  case 4:                                       /* transactional */
    create_flag|= HA_CREATE_CHECKSUM | HA_CREATE_PAGE_CHECKSUM;
    record_type= BLOCK_RECORD;
    transactional= 1;
    key_field=FIELD_BLOB;			/* blob key */
    extra_field= FIELD_BLOB;
    pack_seg|= HA_BLOB_PART;
    key_type= HA_KEYTYPE_VARTEXT1;
    break;
  }


  bzero((char*) recinfo,sizeof(recinfo));
  bzero((char*) &create_info,sizeof(create_info));

  /* First define 2 columns */
  create_info.null_bytes= 1;
  recinfo[0].type= key_field;
  recinfo[0].length= (key_field == FIELD_BLOB ? 4+portable_sizeof_char_ptr :
		      key_length);
  if (key_field == FIELD_VARCHAR)
    recinfo[0].length+= HA_VARCHAR_PACKLENGTH(key_length);
  recinfo[1].type=extra_field;
  recinfo[1].length= (extra_field == FIELD_BLOB ? 4 + portable_sizeof_char_ptr : 24);
  if (extra_field == FIELD_VARCHAR)
    recinfo[1].length+= HA_VARCHAR_PACKLENGTH(recinfo[1].length);
  recinfo[1].null_bit= null_fields ? 2 : 0;

  if (opt_unique)
  {
    recinfo[2].type=FIELD_CHECK;
    recinfo[2].length=MARIA_UNIQUE_HASH_LENGTH;
  }

  if (key_type == HA_KEYTYPE_VARTEXT1 &&
      key_length > 255)
    key_type= HA_KEYTYPE_VARTEXT2;

  /* Define a key over the first column */
  keyinfo[0].seg=keyseg;
  keyinfo[0].keysegs=1;
  keyinfo[0].block_length= 0;                   /* Default block length */
  keyinfo[0].key_alg=HA_KEY_ALG_BTREE;
  keyinfo[0].seg[0].type= key_type;
  keyinfo[0].seg[0].flag= pack_seg;
  keyinfo[0].seg[0].start=1;
  keyinfo[0].seg[0].length=key_length;
  keyinfo[0].seg[0].null_bit= null_fields ? 2 : 0;
  keyinfo[0].seg[0].null_pos=0;
  keyinfo[0].seg[0].language= default_charset_info->number;
  if (pack_seg & HA_BLOB_PART)
  {
    keyinfo[0].seg[0].bit_start=4;		/* Length of blob length */
  }
  keyinfo[0].flag = (uint8) (pack_keys | unique_key);

  if (opt_unique)
  {
    uint start;
    uniques=1;
    bzero((char*) &uniquedef,sizeof(uniquedef));
    bzero((char*) uniqueseg,sizeof(uniqueseg));
    uniquedef.seg=uniqueseg;
    uniquedef.keysegs=2;

    /* Make a unique over all columns (except first NULL fields) */
    for (i=0, start=1 ; i < 2 ; i++)
    {
      uniqueseg[i].start=start;
      start+=recinfo[i].length;
      uniqueseg[i].length=recinfo[i].length;
      uniqueseg[i].language= default_charset_info->number;
    }
    uniqueseg[0].type= key_type;
    uniqueseg[0].null_bit= null_fields ? 2 : 0;
    uniqueseg[1].type= HA_KEYTYPE_TEXT;
    if (extra_field == FIELD_BLOB)
    {
      uniqueseg[1].length=0;			/* The whole blob */
      uniqueseg[1].bit_start=4;			/* long blob */
      uniqueseg[1].flag|= HA_BLOB_PART;
    }
    else if (extra_field == FIELD_VARCHAR)
    {
      uniqueseg[1].flag|= HA_VAR_LENGTH_PART;
      uniqueseg[1].type= (HA_VARCHAR_PACKLENGTH(recinfo[1].length-1) == 1 ?
                          HA_KEYTYPE_VARTEXT1 : HA_KEYTYPE_VARTEXT2);
    }
  }
  else
    uniques=0;

  if (!silent)
    printf("- Creating Aria file\n");
  create_info.max_rows= 0;
  create_info.transactional= transactional;
  if (maria_create(table_name, record_type, 1, keyinfo,2+opt_unique,recinfo,
		uniques, &uniquedef, &create_info,
		create_flag))
    goto err;
  if (!(file=maria_open(table_name,2,HA_OPEN_ABORT_IF_LOCKED, 0)))
    goto err;
  if (!silent)
    printf("- Writing key:s\n");

  if (maria_begin(file))
    goto err;
  my_errno=0;
  for (i= 0 ; i < ROWS_IN_TEST ; i++)
  {
    create_record(record,i);
    if ((error=maria_write(file,record)))
      goto err;
  }

  if (maria_commit(file) | maria_close(file))
    goto err;
  printf("- Data copied\n");
  return 0;

err:
  printf("got error: %3d when using maria-database\n",my_errno);
  return 1;			/* skip warning */
}


static void create_key_part(uchar *key,uint rownr)
{
  if (keyinfo[0].seg[0].type == HA_KEYTYPE_NUM)
  {
    sprintf((char*) key,"%*d",keyinfo[0].seg[0].length,rownr);
  }
  else if (keyinfo[0].seg[0].type == HA_KEYTYPE_VARTEXT1 ||
           keyinfo[0].seg[0].type == HA_KEYTYPE_VARTEXT2)
  {						/* Alpha record */
    /* Create a key that may be easily packed */
    bfill(key,keyinfo[0].seg[0].length,rownr < 10 ? 'A' : 'B');
    sprintf((char*) key+keyinfo[0].seg[0].length-2,"%-2d",rownr % 100);
    if ((rownr & 7) == 0)
    {
      /* Change the key to force a unpack of the next key */
      bfill(key+3,keyinfo[0].seg[0].length-5,rownr < 10 ? 'a' : 'b');
    }
  }
  else
  {						/* Alpha record */
    if (keyinfo[0].seg[0].flag & HA_SPACE_PACK)
      sprintf((char*) key,"%-*d",keyinfo[0].seg[0].length,rownr);
    else
    {
      /* Create a key that may be easily packed */
      bfill(key,keyinfo[0].seg[0].length,rownr < 10 ? 'A' : 'B');
      sprintf((char*) key+keyinfo[0].seg[0].length-2,"%-2d",rownr % 100);
      if ((rownr & 7) == 0)
      {
	/* Change the key to force a unpack of the next key */
	key[1]= (rownr < 10 ? 'a' : 'b');
      }
    }
  }
}


static uchar blob_key[MAX_REC_LENGTH];
static uchar blob_record[MAX_REC_LENGTH+20*20];


static void create_record(uchar *record,uint rownr)
{
  uchar *pos;
  bzero((char*) record,MAX_REC_LENGTH);
  record[0]=1;					/* delete marker */
  if (rownr == 0 && keyinfo[0].seg[0].null_bit)
    record[0]|=keyinfo[0].seg[0].null_bit;	/* Null key */

  pos=record+1;
  if (recinfo[0].type == FIELD_BLOB)
  {
    size_t tmp;
    uchar *ptr;
    create_key_part(blob_key,rownr);
    tmp=strlen((char*) blob_key);
    int4store(pos,tmp);
    ptr=blob_key;
    memcpy(pos+4,&ptr,sizeof(char*));
    pos+=recinfo[0].length;
  }
  else if (recinfo[0].type == FIELD_VARCHAR)
  {
    size_t tmp, pack_length= HA_VARCHAR_PACKLENGTH(recinfo[0].length-1);
    create_key_part(pos+pack_length,rownr);
    tmp= strlen((char*) pos+pack_length);
    if (pack_length == 1)
      *(uchar*) pos= (uchar) tmp;
    else
      int2store(pos,tmp);
    pos+= recinfo[0].length;
  }
  else
  {
    create_key_part(pos,rownr);
    pos+=recinfo[0].length;
  }
  if (recinfo[1].type == FIELD_BLOB)
  {
    size_t tmp;
    uchar *ptr;;
    sprintf((char*) blob_record,"... row: %d", rownr);
    strappend((char*) blob_record, rownr % MAX_REC_LENGTH,'x');
    tmp=strlen((char*) blob_record);
    int4store(pos,tmp);
    ptr=blob_record;
    memcpy(pos+4,&ptr,sizeof(char*));
  }
  else if (recinfo[1].type == FIELD_VARCHAR)
  {
    size_t tmp, pack_length= HA_VARCHAR_PACKLENGTH(recinfo[1].length-1);
    sprintf((char*) pos+pack_length, "... row: %d", rownr);
    tmp= strlen((char*) pos+pack_length);
    if (pack_length == 1)
      *pos= (uchar) tmp;
    else
      int2store(pos,tmp);
  }
  else
  {
    sprintf((char*) pos,"... row: %d", rownr);
    strappend((char*) pos,recinfo[1].length,' ');
  }
}

#include "ma_check_standalone.h"

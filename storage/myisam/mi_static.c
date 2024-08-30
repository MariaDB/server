/* Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.

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
  Static variables for MyISAM library. All definied here for easy making of
  a shared library
*/

#ifndef MY_GLOBAL_INCLUDED
#include "myisamdef.h"
#endif

LIST	*myisam_open_list=0;
uchar	myisam_file_magic[]=
{ (uchar) 254, (uchar) 254,'\007', '\001', };
uchar	myisam_pack_file_magic[]=
{ (uchar) 254, (uchar) 254,'\010', '\002', };
char * myisam_log_filename=(char*) "myisam.log";
File	myisam_log_file= -1;
uint	myisam_quick_table_bits=9;
ulong	myisam_block_size= MI_KEY_BLOCK_LENGTH;		/* Best by test */
my_bool myisam_flush=0, myisam_delay_key_write=0, myisam_single_user=0;
#if !defined(DONT_USE_RW_LOCKS)
ulong myisam_concurrent_insert= 2;
#else
ulong myisam_concurrent_insert= 0;
#endif
ulonglong myisam_max_temp_length= MAX_FILE_SIZE;
ulong    myisam_data_pointer_size=4;
ulonglong    myisam_mmap_size= SIZE_T_MAX, myisam_mmap_used= 0;
my_bool (*mi_killed)(MI_INFO *)= mi_killed_standalone;

/*
  read_vec[] is used for converting between P_READ_KEY.. and SEARCH_
  Position is , == , >= , <= , > , <
*/

uint myisam_read_vec[]=
{
  SEARCH_FIND, SEARCH_FIND | SEARCH_BIGGER, SEARCH_FIND | SEARCH_SMALLER,
  SEARCH_NO_FIND | SEARCH_BIGGER, SEARCH_NO_FIND | SEARCH_SMALLER,
  SEARCH_FIND | SEARCH_PREFIX, SEARCH_LAST, SEARCH_LAST | SEARCH_SMALLER,
  MBR_CONTAIN, MBR_INTERSECT, MBR_WITHIN, MBR_DISJOINT, MBR_EQUAL
};

uint myisam_readnext_vec[]=
{
  SEARCH_BIGGER, SEARCH_BIGGER, SEARCH_SMALLER, SEARCH_BIGGER, SEARCH_SMALLER,
  SEARCH_BIGGER, SEARCH_SMALLER, SEARCH_SMALLER
};

PSI_memory_key mi_key_memory_MYISAM_SHARE;
PSI_memory_key mi_key_memory_MI_INFO;
PSI_memory_key mi_key_memory_MI_INFO_ft1_to_ft2;
PSI_memory_key mi_key_memory_MI_INFO_bulk_insert;
PSI_memory_key mi_key_memory_record_buffer;
PSI_memory_key mi_key_memory_FTB;
PSI_memory_key mi_key_memory_FT_INFO;
PSI_memory_key mi_key_memory_FTPARSER_PARAM;
PSI_memory_key mi_key_memory_ft_memroot;
PSI_memory_key mi_key_memory_ft_stopwords;
PSI_memory_key mi_key_memory_MI_SORT_PARAM;
PSI_memory_key mi_key_memory_MI_SORT_PARAM_wordroot;
PSI_memory_key mi_key_memory_SORT_FT_BUF;
PSI_memory_key mi_key_memory_SORT_KEY_BLOCKS;
PSI_memory_key mi_key_memory_filecopy;
PSI_memory_key mi_key_memory_SORT_INFO_buffer;
PSI_memory_key mi_key_memory_MI_DECODE_TREE;
PSI_memory_key mi_key_memory_MYISAM_SHARE_decode_tables;
PSI_memory_key mi_key_memory_preload_buffer;
PSI_memory_key mi_key_memory_stPageList_pages;
PSI_memory_key mi_key_memory_keycache_thread_var;

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key mi_key_mutex_MYISAM_SHARE_intern_lock,
  mi_key_mutex_MI_SORT_INFO_mutex, mi_key_mutex_MI_CHECK_print_msg;

static PSI_mutex_info all_myisam_mutexes[]=
{
  { &mi_key_mutex_MI_SORT_INFO_mutex, "MI_SORT_INFO::mutex", 0},
  { &mi_key_mutex_MYISAM_SHARE_intern_lock, "MYISAM_SHARE::intern_lock", 0},
  { &mi_key_mutex_MI_CHECK_print_msg, "MI_CHECK::print_msg", 0}
};

PSI_rwlock_key mi_key_rwlock_MYISAM_SHARE_key_root_lock,
  mi_key_rwlock_MYISAM_SHARE_mmap_lock;

static PSI_rwlock_info all_myisam_rwlocks[]=
{
  { &mi_key_rwlock_MYISAM_SHARE_key_root_lock, "MYISAM_SHARE::key_root_lock", 0},
  { &mi_key_rwlock_MYISAM_SHARE_mmap_lock, "MYISAM_SHARE::mmap_lock", 0}
};

PSI_cond_key mi_key_cond_MI_SORT_INFO_cond;

static PSI_cond_info all_myisam_conds[]=
{
  { &mi_key_cond_MI_SORT_INFO_cond, "MI_SORT_INFO::cond", 0}
};

PSI_file_key mi_key_file_datatmp, mi_key_file_dfile, mi_key_file_kfile,
  mi_key_file_log;

static PSI_file_info all_myisam_files[]=
{
  { & mi_key_file_datatmp, "data_tmp", 0},
  { & mi_key_file_dfile, "dfile", 0},
  { & mi_key_file_kfile, "kfile", 0},
  { & mi_key_file_log, "log", 0}
};

PSI_thread_key mi_key_thread_find_all_keys;

static PSI_thread_info all_myisam_threads[]=
{
  { &mi_key_thread_find_all_keys, "find_all_keys", 0},
};

static PSI_memory_info all_myisam_memory[]=
{
  { &mi_key_memory_MYISAM_SHARE, "MYISAM_SHARE", 0},
  { &mi_key_memory_MI_INFO, "MI_INFO", 0},
  { &mi_key_memory_MI_INFO_ft1_to_ft2, "MI_INFO::ft1_to_ft2", 0},
  { &mi_key_memory_MI_INFO_bulk_insert, "MI_INFO::bulk_insert", 0},
  { &mi_key_memory_record_buffer, "record_buffer", 0},
  { &mi_key_memory_FTB, "FTB", 0},
  { &mi_key_memory_FT_INFO, "FT_INFO", 0},
  { &mi_key_memory_FTPARSER_PARAM, "FTPARSER_PARAM", 0},
  { &mi_key_memory_ft_memroot, "ft_memroot", 0},
  { &mi_key_memory_ft_stopwords, "ft_stopwords", 0},
  { &mi_key_memory_MI_SORT_PARAM, "MI_SORT_PARAM", 0},
  { &mi_key_memory_MI_SORT_PARAM_wordroot, "MI_SORT_PARAM::wordroot", 0},
  { &mi_key_memory_SORT_FT_BUF, "SORT_FT_BUF", 0},
  { &mi_key_memory_SORT_KEY_BLOCKS, "SORT_KEY_BLOCKS", 0},
  { &mi_key_memory_filecopy, "filecopy", 0},
  { &mi_key_memory_SORT_INFO_buffer, "SORT_INFO::buffer", 0},
  { &mi_key_memory_MI_DECODE_TREE, "MI_DECODE_TREE", 0},
  { &mi_key_memory_MYISAM_SHARE_decode_tables, "MYISAM_SHARE::decode_tables", 0},
  { &mi_key_memory_preload_buffer, "preload_buffer", 0},
  { &mi_key_memory_stPageList_pages, "stPageList::pages", 0},
  { &mi_key_memory_keycache_thread_var, "keycache_thread_var", 0}
};

void init_myisam_psi_keys()
{
  const char* category= "myisam";
  int count;

  count= array_elements(all_myisam_mutexes);
  mysql_mutex_register(category, all_myisam_mutexes, count);

  count= array_elements(all_myisam_rwlocks);
  mysql_rwlock_register(category, all_myisam_rwlocks, count);

  count= array_elements(all_myisam_conds);
  mysql_cond_register(category, all_myisam_conds, count);

  count= array_elements(all_myisam_files);
  mysql_file_register(category, all_myisam_files, count);

  count= array_elements(all_myisam_threads);
  mysql_thread_register(category, all_myisam_threads, count);

  count= array_elements(all_myisam_memory);
  mysql_memory_register(category, all_myisam_memory, count);
}
#endif /* HAVE_PSI_INTERFACE */


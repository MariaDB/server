/* Copyright (c) 2000, 2011, Oracle and/or its affiliates.
   Copyright (c) 2009, 2019, MariaDB Corporation.

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
  Static variables for mysys library. All definied here for easy making of
  a shared library
*/

#include "mysys_priv.h"
#include "my_static.h"
#include "my_alarm.h"


PSI_memory_key key_memory_DYNAMIC_STRING;
PSI_memory_key key_memory_IO_CACHE;
PSI_memory_key key_memory_KEY_CACHE;
PSI_memory_key key_memory_LIST;
PSI_memory_key key_memory_MY_BITMAP_bitmap;
PSI_memory_key key_memory_MY_DIR;
PSI_memory_key key_memory_MY_STAT;
PSI_memory_key key_memory_MY_TMPDIR_full_list;
PSI_memory_key key_memory_QUEUE;
PSI_memory_key key_memory_SAFE_HASH_ENTRY;
PSI_memory_key key_memory_THD_ALARM;
PSI_memory_key key_memory_TREE;
PSI_memory_key key_memory_charset_file;
PSI_memory_key key_memory_charset_loader;
PSI_memory_key key_memory_defaults;
PSI_memory_key key_memory_lf_dynarray;
PSI_memory_key key_memory_lf_node;
PSI_memory_key key_memory_lf_slist;
PSI_memory_key key_memory_max_alloca;
PSI_memory_key key_memory_my_compress_alloc;
PSI_memory_key key_memory_my_err_head;
PSI_memory_key key_memory_my_file_info;
PSI_memory_key key_memory_pack_frm;
PSI_memory_key key_memory_charsets;
PSI_memory_key key_memory_new;

#ifdef _WIN32
PSI_memory_key key_memory_win_SECURITY_ATTRIBUTES;
PSI_memory_key key_memory_win_PACL;
PSI_memory_key key_memory_win_IP_ADAPTER_ADDRESSES;
#endif /* _WIN32 */

	/* from my_init */
char *home_dir=0;
char *mysql_data_home= (char*) ".";
const char      *my_progname= NULL, *my_progname_short= NULL;
char		curr_dir[FN_REFLEN]= {0},
		home_dir_buff[FN_REFLEN]= {0};
ulong		my_stream_opened=0,my_tmp_file_created=0;
ulong           my_file_total_opened= 0;
int		my_umask=0664, my_umask_dir=0777;
#ifdef _WIN32
SECURITY_ATTRIBUTES my_dir_security_attributes= {sizeof(SECURITY_ATTRIBUTES),NULL,FALSE};
#endif
myf             my_global_flags= 0;
#ifndef DBUG_OFF
my_bool         my_assert= 1;
#endif
my_bool         my_assert_on_error= 0;
struct st_my_file_info my_file_info_default[MY_NFILE];
uint   my_file_limit= MY_NFILE;
int32           my_file_opened=0;
struct st_my_file_info *my_file_info= my_file_info_default;

	/* From mf_brkhant */
int			my_dont_interrupt=0;
volatile int		_my_signals=0;
struct st_remember _my_sig_remember[MAX_SIGNALS]={{0,0}};

	/* from mf_reccache.c */
ulong my_default_record_cache_size=RECORD_CACHE_SIZE;

	/* from soundex.c */
				/* ABCDEFGHIJKLMNOPQRSTUVWXYZ */
				/* :::::::::::::::::::::::::: */
const char *soundex_map=	  "01230120022455012623010202";

	/* from my_malloc */
USED_MEM* my_once_root_block=0;			/* pointer to first block */
uint	  my_once_extra=ONCE_ALLOC_INIT;	/* Memory to alloc / block */

	/* from my_alarm */
int volatile my_have_got_alarm=0;	/* declare variable to reset */
ulong my_time_to_wait_for_lock=2;	/* In seconds */

	/* from errors.c */
#ifdef SHARED_LIBRARY
const char *globerrs[GLOBERRS];		/* my_error_messages is here */
#endif
void (*error_handler_hook)(uint error, const char *str, myf MyFlags)=
  my_message_stderr;
void (*fatal_error_handler_hook)(uint error, const char *str, myf MyFlags)=
  my_message_stderr;

static void proc_info_dummy(void *a __attribute__((unused)),
                            const PSI_stage_info *b __attribute__((unused)),
                            PSI_stage_info *c __attribute__((unused)),
                            const char *d __attribute__((unused)),
                            const char *e __attribute__((unused)),
                            const unsigned int f __attribute__((unused)))
{
  return;
}

/* this is to be able to call set_thd_proc_info from the C code */
void (*proc_info_hook)(void *, const PSI_stage_info *, PSI_stage_info *,
                       const char *, const char *, const unsigned int)= proc_info_dummy;
void (*debug_sync_C_callback_ptr)(MYSQL_THD, const char *, size_t)= 0;

	/* How to disable options */
my_bool my_disable_locking=0;
my_bool my_disable_sync=0;
my_bool my_disable_async_io=0;
my_bool my_disable_flush_key_blocks=0;
my_bool my_disable_symlinks=0;
my_bool my_disable_copystat_in_redel=0;

/* Typelib by all clients */
const char *sql_protocol_names_lib[] =
{ "TCP", "SOCKET", "PIPE", NullS };

TYPELIB sql_protocol_typelib ={ array_elements(sql_protocol_names_lib) - 1, "",
                                sql_protocol_names_lib, NULL };

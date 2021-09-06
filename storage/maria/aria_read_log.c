/* Copyright (C) 2007 MySQL AB
   Copyright (C) 2010 Monty Program Ab
   Copyright (C) 2020 MariaDB Corporation Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "maria_def.h"
#include "ma_recovery.h"
#include <my_getopt.h>

#define LOG_FLAGS 0

static const char *load_default_groups[]= { "aria_read_log",0 };
static void get_options(int *argc,char * * *argv);
#ifndef DBUG_OFF
#if defined(_WIN32)
const char *default_dbug_option= "d:t:O,\\aria_read_log.trace";
#else
const char *default_dbug_option= "d:t:o,/tmp/aria_read_log.trace";
#endif
#endif /* DBUG_OFF */
static my_bool opt_display_only, opt_apply, opt_silent, opt_apply_undo;
static my_bool opt_check, opt_start_from_checkpoint;
static my_bool opt_print_aria_log_control;
static const char *opt_tmpdir;
static ulong opt_translog_buffer_size;
static ulonglong opt_page_buffer_size;
static ulonglong opt_start_from_lsn, opt_lsn_redo_end, opt_lsn_undo_end;
static char *start_from_lsn_buf, *lsn_redo_end_buf, *lsn_undo_end_buf;
static MY_TMPDIR maria_chk_tmpdir;

/*
  Get lsn from file number and offset
  Format supported:
  ulonglong
  uint,0xhex
*/

static ulonglong get_lsn(const char *lsn_str)
{
  ulong file;
  ulong pos;
  if (sscanf(lsn_str, " %lu,0x%lx", &file, &pos) == 2)
    return MAKE_LSN(file, pos);
  if (sscanf(lsn_str, " %lu", &pos) == 1)
    return (ulonglong) pos;
  return ~(ulonglong) 0;                        /* Error */
}

static my_bool get_lsn_arg(const char *lsn_string, ulonglong *lsn,
                           const char *name)
{
  ulonglong value;
  value= get_lsn(lsn_string);
  if (value != ~(ulonglong) 0)
  {
    *lsn= value;
    return 0;
  }
  fprintf(stderr,
          "Wrong value '%s' for option %s. Value should be in format: "
          "number,0xhexnumber\n",
          lsn_string, name);
  return 1;
}


int main(int argc, char **argv)
{
  LSN lsn;
  char **default_argv;
  uint warnings_count;
  MY_INIT(argv[0]);

  maria_data_root= ".";
  sf_leaking_memory=1; /* don't report memory leaks on early exits */
  load_defaults_or_exit("my", load_default_groups, &argc, &argv);
  default_argv= argv;
  get_options(&argc, &argv);

  maria_in_recovery= TRUE;

  if (maria_init())
  {
    fprintf(stderr, "Can't init Aria engine (%d)\n", errno);
    goto err;
  }
  maria_block_size= 0;                          /* Use block size from file */
  if (opt_print_aria_log_control)
  {
    if (print_aria_log_control())
      goto err;
    goto end;
  }
  /* we don't want to create a control file, it MUST exist */
  if (ma_control_file_open(FALSE, TRUE, TRUE))
  {
    fprintf(stderr, "Can't open control file (%d)\n", errno);
    goto err;
  }
  if (last_logno == FILENO_IMPOSSIBLE)
  {
    fprintf(stderr, "Can't find any log\n");
    goto err;
  }
  if (init_pagecache(maria_pagecache, (size_t)opt_page_buffer_size, 0, 0,
                     maria_block_size, 0, MY_WME) == 0)
  {
    fprintf(stderr, "Got error in init_pagecache() (errno: %d)\n", errno);
    goto err;
  }
  /*
    If log handler does not find the "last_logno" log it will return error,
    which is good.
    But if it finds a log and this log was crashed, it will create a new log,
    which is useless. TODO: start log handler in read-only mode.
  */
  if (init_pagecache(maria_log_pagecache, opt_translog_buffer_size,
                     0, 0, TRANSLOG_PAGE_SIZE, 0, MY_WME) == 0 ||
      translog_init(maria_data_root, TRANSLOG_FILE_SIZE,
                    0, 0, maria_log_pagecache, TRANSLOG_DEFAULT_FLAGS,
                    opt_display_only))
  {
    fprintf(stderr, "Can't init loghandler (%d)\n", errno);
    goto err;
  }

  if (opt_display_only)
    printf("You are using --display-only, NOTHING will be written to disk\n");

  lsn= translog_first_lsn_in_log();
  if (lsn == LSN_ERROR)
  {
    fprintf(stderr, "Opening transaction log failed\n");
    goto end;
  }
  if (lsn == LSN_IMPOSSIBLE)
  {
     fprintf(stdout, "The transaction log is empty\n");
  }
  if (opt_start_from_checkpoint && !opt_start_from_lsn &&
      last_checkpoint_lsn != LSN_IMPOSSIBLE)
  {
    lsn= LSN_IMPOSSIBLE;             /* LSN set in maria_apply_log() */
    fprintf(stdout, "Starting from checkpoint " LSN_FMT "\n",
            LSN_IN_PARTS(last_checkpoint_lsn));
  }
  else
    fprintf(stdout, "The transaction log starts from lsn " LSN_FMT "\n",
            LSN_IN_PARTS(lsn));

  if (opt_start_from_lsn)
  {
    if (opt_start_from_lsn < (ulonglong) lsn)
    {
      fprintf(stderr, "start_from_lsn is too small. Aborting\n");
      maria_end();
      goto err;
    }
    lsn= (LSN) opt_start_from_lsn;
    fprintf(stdout, "Starting reading log from lsn " LSN_FMT "\n",
            LSN_IN_PARTS(lsn));
  }

  fprintf(stdout, "TRACE of the last aria_read_log\n");
  if (maria_apply_log(lsn, opt_lsn_redo_end, opt_lsn_undo_end,
                      opt_apply ?  MARIA_LOG_APPLY :
                      (opt_check ? MARIA_LOG_CHECK :
                       MARIA_LOG_DISPLAY_HEADER), opt_silent ? NULL : stdout,
                      FALSE, FALSE, &warnings_count))
    goto err;
  if (warnings_count == 0)
    fprintf(stdout, "%s: SUCCESS\n", my_progname_short);
  else
    fprintf(stdout, "%s: DOUBTFUL (%u warnings, check previous output)\n",
            my_progname_short, warnings_count);

end:
  maria_end();
  free_tmpdir(&maria_chk_tmpdir);
  free_defaults(default_argv);
  my_end(0);
  sf_leaking_memory=0;
  exit(0);
  return 0;				/* No compiler warning */

err:
  /* don't touch anything more, in case we hit a bug */
  fprintf(stderr, "%s: FAILED\n", my_progname_short);
  free_tmpdir(&maria_chk_tmpdir);
  free_defaults(default_argv);
  exit(1);
}


#include "ma_check_standalone.h"

enum options_mc {
  OPT_CHARSETS_DIR=256, OPT_FORCE_CRASH, OPT_TRANSLOG_BUFFER_SIZE
};

static struct my_option my_long_options[] =
{
  {"apply", 'a',
   "Apply log to tables: modifies tables! you should make a backup first! "
   " Displays a lot of information if not run with --silent",
   (uchar **) &opt_apply, (uchar **) &opt_apply, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"character-sets-dir", OPT_CHARSETS_DIR,
   "Directory where character sets are.",
   (char**) &charsets_dir, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"check", 'c',
   "if --display-only, check if record is fully readable (for debugging)",
   (uchar **) &opt_check, (uchar **) &opt_check, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
#ifndef DBUG_OFF
  {"debug", '#', "Output debug log. Often the argument is 'd:t:o,filename'.",
   0, 0, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"force-crash", OPT_FORCE_CRASH, "Force crash after # recovery events",
   &maria_recovery_force_crash_counter, 0,0, GET_ULONG, REQUIRED_ARG,
   0, 0, ~(long) 0, 0, 0, 0},
#endif
  {"help", '?', "Display this help and exit.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"display-only", 'd', "display brief info read from records' header",
   &opt_display_only, &opt_display_only, 0, GET_BOOL,
   NO_ARG,0, 0, 0, 0, 0, 0},
  { "end-lsn", 'e', "Alias for lsn-redo-end",
    &lsn_redo_end_buf, &lsn_redo_end_buf, 0, GET_STR, REQUIRED_ARG, 0, 0,
    0, 0, 0, 0 },
  { "lsn-redo-end", 'e', "Stop applying at this lsn during redo. If "
    "this option is used UNDO:s will not be applied unless --lsn-undo-end is "
    "given", &lsn_redo_end_buf,
    &lsn_redo_end_buf, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "lsn-undo-end", 'E', "Stop applying undo after this lsn has been applied",
    &lsn_undo_end_buf, &lsn_undo_end_buf, 0, GET_STR, REQUIRED_ARG, 0, 0,
    0, 0, 0, 0 },
  {"aria-log-dir-path", 'h',
    "Path to the directory where to store transactional log",
    (char **) &maria_data_root, (char **) &maria_data_root, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  { "page-buffer-size", 'P',
    "The size of the buffer used for index blocks for Aria tables",
    &opt_page_buffer_size, &opt_page_buffer_size, 0,
    GET_ULL, REQUIRED_ARG, PAGE_BUFFER_INIT,
    PAGE_BUFFER_INIT, SIZE_T_MAX, MALLOC_OVERHEAD, (long) IO_SIZE, 0},
  { "print-log-control-file", 'l',
    "Print the content of the aria_log_control_file",
    &opt_print_aria_log_control, &opt_print_aria_log_control, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  { "start-from-lsn", 'o', "Start reading log from this lsn",
    &opt_start_from_lsn, &opt_start_from_lsn,
    0, GET_ULL, REQUIRED_ARG, 0, 0, ~(longlong) 0, 0, 0, 0 },
  {"start-from-checkpoint", 'C', "Start applying from last checkpoint",
   &opt_start_from_checkpoint, &opt_start_from_checkpoint, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"silent", 's', "Print less information during apply/undo phase",
   &opt_silent, &opt_silent, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"tables-to-redo", 'T',
   "List of tables separated with , that we should apply REDO on. Use this if you only want to recover some tables",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"tmpdir", 't', "Path for temporary files. Multiple paths can be specified, "
   "separated by "
#if defined( _WIN32)
   "semicolon (;)"
#else
   "colon (:)"
#endif
   , (char**) &opt_tmpdir, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  { "translog-buffer-size", OPT_TRANSLOG_BUFFER_SIZE,
    "The size of the buffer used for transaction log for Aria tables",
    &opt_translog_buffer_size, &opt_translog_buffer_size, 0,
    GET_ULONG, REQUIRED_ARG, (long) TRANSLOG_PAGECACHE_SIZE,
    1024L*1024L, (long) ~(ulong) 0, (long) MALLOC_OVERHEAD,
    (long) IO_SIZE, 0},
  {"undo", 'u',
   "Apply UNDO records to tables. (disable with --disable-undo). "
   "Will be automatically set if lsn-undo-end is used",
   (uchar **) &opt_apply_undo, (uchar **) &opt_apply_undo, 0,
   GET_BOOL, NO_ARG, 1, 0, 0, 0, 0, 0},
  {"verbose", 'v', "Print more information during apply/undo phase",
   &maria_recovery_verbose, &maria_recovery_verbose, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Print version and exit.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

static void print_version(void)
{
  printf("%s Ver 1.5 for %s on %s\n",
              my_progname_short, SYSTEM_TYPE, MACHINE_TYPE);
}


static void usage(void)
{
  print_version();
  puts("Copyright (C) 2007 MySQL AB, 2009-2011 Monty Program Ab, 2020 MariaDB Corporation");
  puts("This software comes with ABSOLUTELY NO WARRANTY. This is free software,");
  puts("and you are welcome to modify and redistribute it under the GPL license\n");

  puts("Display or apply log records from a Aria transaction log");
  puts("found in the current directory (for now)");
#ifndef IDENTICAL_PAGES_AFTER_RECOVERY
  puts("\nNote: Aria is compiled without -DIDENTICAL_PAGES_AFTER_RECOVERY\n"
       "which means that the table files are not byte-to-byte identical to\n"
       "files created during normal execution. This should be ok, except for\n"
       "test scripts that tries to compare files before and after recovery.");
#endif
  printf("\nUsage: %s OPTIONS [-d | -a] -h `aria_log_directory`\n",
         my_progname_short);
  printf("or\n");
  printf("Usage: %s OPTIONS -h `aria_log_directory` "
         "--print-log-control-file\n\n",
         my_progname_short);

  my_print_help(my_long_options);
  print_defaults("my", load_default_groups);
  my_print_variables(my_long_options);
}


static uchar* my_hash_get_string(const uchar *record, size_t *length,
                                my_bool first __attribute__ ((unused)))
{
  *length= (size_t) (strcend((const char*) record,',')- (const char*) record);
  return (uchar*) record;
}


static my_bool
get_one_option(const struct my_option *opt,
               const char *argument,
               const char *filename __attribute__((unused)))
{
  switch (opt->id) {
  case '?':
    usage();
    exit(0);
  case 'V':
    print_version();
    exit(0);
  case 'E':
    opt_apply_undo= TRUE;
    break;
  case 'T':
  {
    char *pos;
    if (!my_hash_inited(&tables_to_redo))
    {
      my_hash_init2(PSI_INSTRUMENT_ME, &tables_to_redo, 16, &my_charset_bin,
                    16, 0, 0, my_hash_get_string, 0, 0, HASH_UNIQUE);
    }
    do
    {
      pos= strcend(argument, ',');
      if (pos != argument)                      /* Skip empty strings */
        my_hash_insert(&tables_to_redo, (uchar*) argument);
      argument= pos+1;
    } while (*(pos++));
    break;
  }
#ifndef DBUG_OFF
  case '#':
    DBUG_SET_INITIAL(argument ? argument : default_dbug_option);
    break;
#endif
  }
  return 0;
}

static void get_options(int *argc,char ***argv)
{
  int ho_error;
  my_bool need_help= 0, need_abort= 0;

  if ((ho_error=handle_options(argc, argv, my_long_options, get_one_option)))
    exit(ho_error);

  if (start_from_lsn_buf)
  {
    if (get_lsn_arg(start_from_lsn_buf, &opt_start_from_lsn,
                    "start-from-lsn"))
      need_abort= 1;
  }
  if (lsn_redo_end_buf)
  {
    if (get_lsn_arg(lsn_redo_end_buf, &opt_lsn_redo_end,
                    "lsn-redo-end"))
      need_abort= 1;
  }
  if (lsn_undo_end_buf)
  {
    if (get_lsn_arg(lsn_undo_end_buf, &opt_lsn_undo_end,
                    "lsn-undo-end"))
      need_abort= 1;
  }

  if (!opt_apply)
    opt_apply_undo= FALSE;
  if (!opt_apply_undo)
    opt_lsn_undo_end= LSN_MAX;

  if (*argc > 0)
  {
    need_help= 1;
    fprintf(stderr, "Too many arguments given\n");
  }
  if ((opt_display_only + opt_apply + opt_print_aria_log_control) != 1)
  {
    need_abort= 1;
    fprintf(stderr,
            "You must use one and only one of the options 'display-only', \n"
            "'print-log-control-file' and 'apply'\n");
  }

  if (need_help || need_abort)
  {
    fflush(stderr);
    if (need_help)
      usage();
    exit(1);
  }
  if (init_tmpdir(&maria_chk_tmpdir, opt_tmpdir))
    exit(1);
  maria_tmpdir= &maria_chk_tmpdir;
}

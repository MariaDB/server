/* Copyright (C) 2019 MariaDB corporation

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

/*
  Allow copying of Aria tables to and from S3 and also delete them from S3
*/

#include <my_global.h>
#include <m_string.h>
#include "maria_def.h"
#include <aria_backup.h>
#include <my_getopt.h>
#include <my_check_opt.h>
#include <mysys_err.h>
#include <mysqld_error.h>
#include <zlib.h>
#include <libmarias3/marias3.h>
#include "s3_func.h"

static const char *op_types[]= {"to_s3", "from_s3", "delete_from_s3", NullS};
static TYPELIB op_typelib= {array_elements(op_types)-1,"", op_types, NULL};
#define OP_IMPOSSIBLE array_elements(op_types)

static const char *load_default_groups[]= { "aria_s3_copy", 0 };
static const char *opt_s3_access_key, *opt_s3_secret_key;
static const char *opt_s3_region="eu-north-1";
static const char *opt_s3_host_name= DEFAULT_AWS_HOST_NAME;
static const char *opt_database;
static const char *opt_s3_bucket="MariaDB";
static my_bool opt_compression, opt_verbose, opt_force, opt_s3_debug;
static my_bool opt_s3_use_http;
static ulong opt_operation= OP_IMPOSSIBLE, opt_protocol_version= 1;
static ulong opt_block_size;
static ulong opt_s3_port;
static char **default_argv=0;
static ms3_st *global_s3_client= 0;


static struct my_option my_long_options[] =
{
  {"help", '?', "Display this help and exit.", 0, 0, 0, GET_NO_ARG, NO_ARG, 0,
   0, 0, 0, 0, 0},
  {"s3_access_key", 'k', "AWS access key ID",
   (char**) &opt_s3_access_key, (char**) &opt_s3_access_key, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"s3_region", 'r', "AWS region",
   (char**) &opt_s3_region, (char**) &opt_s3_region, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"s3_secret_key", 'K', "AWS secret access key ID",
   (char**) &opt_s3_secret_key, (char**) &opt_s3_secret_key, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"s3_bucket", 'b', "AWS prefix for tables",
   (char**) &opt_s3_bucket, (char**) &opt_s3_bucket, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"s3_host_name", 'h', "Host name to S3 provider",
   (char**) &opt_s3_host_name, (char**) &opt_s3_host_name, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"s3_port", 'p', "Port number to connect to (0 means use default)",
   (char**) &opt_s3_port, (char**) &opt_s3_port, 0, GET_ULONG, REQUIRED_ARG,
   0, 0, 65536, 0, 1, 0 },
  {"s3_use_http", 'P', "If true, force use of HTTP protocol",
   (char**) &opt_s3_use_http, (char**) &opt_s3_use_http,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"compress", 'c', "Use compression", &opt_compression, &opt_compression,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"op", 'o', "Operation to execute. One of 'from_s3', 'to_s3' or "
   "'delete_from_s3'",
   &opt_operation, &opt_operation, &op_typelib,
   GET_ENUM, REQUIRED_ARG, OP_IMPOSSIBLE, 0, 0, 0, 0, 0},
  {"database", 'd',
   "Database for copied table (second prefix). "
   "If not given, the directory of the table file is used",
   &opt_database, &opt_database, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"s3_block_size", 'B', "Block size for data/index blocks in s3",
   &opt_block_size, &opt_block_size, 0, GET_ULONG, REQUIRED_ARG,
   4*1024*1024, 64*1024, 16*1024*1024, MALLOC_OVERHEAD, 1024, 0 },
  {"s3_protocol_version", 'L',
   "Protocol used to communication with S3. One of \"Auto\", \"Amazon\" or \"Original\".",
   &opt_protocol_version, &opt_protocol_version, &s3_protocol_typelib,
   GET_ENUM, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"force", 'f', "Force copy even if target exists",
   &opt_force, &opt_force, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"verbose", 'v', "Write more information", &opt_verbose, &opt_verbose,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Print version and exit.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
#ifndef DBUG_OFF
  {"debug", '#', "Output debug log. Often this is 'd:t:o,filename'.",
   0, 0, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"s3_debug",0, "Output debug log from marias3 to stdout",
  &opt_s3_debug, &opt_s3_debug, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
};


static bool get_database_from_path(char *to, size_t to_length, const char *path);


static void print_version(void)
{
  printf("%s  Ver 1.0 for %s on %s\n", my_progname, SYSTEM_TYPE,
	 MACHINE_TYPE);
}

static void usage(void)
{
  print_version();
  puts("\nThis software comes with NO WARRANTY: "
       " see the PUBLIC for details.\n");
  puts("Copy an Aria table to and from s3");
  printf("Usage: %s --aws-access-key=# --aws-secret-access-key=# --aws-region=# "
         "--op=(from_s3 | to_s3 | delete_from_s3) [OPTIONS] tables[.MAI]\n",
         my_progname_short);
  print_defaults("my", load_default_groups);
  puts("");
  my_print_help(my_long_options);
  my_print_variables(my_long_options);
}


ATTRIBUTE_NORETURN static void my_exit(int exit_code)
{
  if (global_s3_client)
  {
    ms3_deinit(global_s3_client);
    global_s3_client= 0;
  }
  free_defaults(default_argv);
  s3_deinit_library();
  my_end(MY_CHECK_ERROR);
  exit(exit_code);
}

extern "C" my_bool get_one_option(const struct my_option *opt
                                  __attribute__((unused)),
                                  const char *argument, const char *filename)
{
  switch (opt->id) {
  case 'V':
    print_version();
    my_exit(0);
  case '?':
    usage();
    my_exit(0);
  case '#':
    DBUG_SET_INITIAL(argument ? argument : "d:t:o,/tmp/aria_s3_copy.trace");
    break;
  }
  return 0;
}


static void get_options(int *argc, char ***argv)
{
  int ho_error;

  load_defaults_or_exit("my", load_default_groups, argc, argv);
  default_argv= *argv;

  if ((ho_error=handle_options(argc, argv, my_long_options, get_one_option)))
    my_exit(ho_error);

  if (*argc == 0)
  {
    usage();
    my_exit(-1);
  }

  if (!opt_s3_access_key)
  {
    fprintf(stderr, "--aws-access-key was not given\n");
    my_exit(-1);
  }
  if (!opt_s3_secret_key)
  {
    fprintf(stderr, "--aws-secret-access-key was not given\n");
    my_exit(-1);
  }
  if (opt_operation == OP_IMPOSSIBLE)
  {
    fprintf(stderr, "You must specify an operation with --op=[from_s3|to_s3|delete_from_s3]\n");
    my_exit(-1);
  }
  if (opt_s3_debug)
    ms3_debug();

} /* get_options */


int main(int argc, char** argv)
{
  MY_INIT(argv[0]);
  get_options(&argc,(char***) &argv);
  size_t block_size= opt_block_size;

  s3_init_library();
  if (!(global_s3_client= ms3_init(opt_s3_access_key,
                                   opt_s3_secret_key,
                                   opt_s3_region, opt_s3_host_name)))
  {
    fprintf(stderr, "Can't open connection to S3, error: %d %s", errno,
            ms3_error(errno));
    my_exit(1);
  }

  ms3_set_option(global_s3_client, MS3_OPT_BUFFER_CHUNK_SIZE, &block_size);

  if (opt_protocol_version)
  {
    uint8_t protocol_version= (uint8_t) opt_protocol_version;
    ms3_set_option(global_s3_client, MS3_OPT_FORCE_PROTOCOL_VERSION,
                   &protocol_version);
  }
  if (opt_s3_port)
  {
    int port= (int) opt_s3_port;
    ms3_set_option(global_s3_client, MS3_OPT_PORT_NUMBER, &port);
  }
  if (opt_s3_use_http)
    ms3_set_option(global_s3_client, MS3_OPT_USE_HTTP, NULL);


  for (; *argv ; argv++)
  {
    char database[FN_REFLEN], table_name[FN_REFLEN], *path;
    const char *db;

    path= *argv;

    fn_format(table_name, path, "", "", MY_REPLACE_DIR | MY_REPLACE_EXT);

    /* Get database from option, path or current directory */
    if (!(db= opt_database))
    {
      if (get_database_from_path(database, sizeof(database), path))
      {
        fprintf(stderr, "Aborting copying of %s\n", path);
        my_exit(-1);
      }
      db= database;
    }

    switch (opt_operation) {
    case 0:
      /* Don't copy .frm file for partioned table */
      if (aria_copy_to_s3(global_s3_client, opt_s3_bucket, path,
                          db, table_name, opt_block_size, opt_compression,
                          opt_force, opt_verbose, !strstr(table_name, "#P#")))
      {
        fprintf(stderr, "Aborting copying of %s\n", path);
        my_exit(-1);
      }
      break;
    case 1:
      if (aria_copy_from_s3(global_s3_client, opt_s3_bucket, path,
                          db, opt_compression, opt_force, opt_verbose))
      {
        fprintf(stderr, "Aborting copying of %s\n", path);
        my_exit(-1);
      }
      break;
    case 2:
      if (aria_delete_from_s3(global_s3_client, opt_s3_bucket, db,
                              table_name, opt_verbose))
      {
        fprintf(stderr, "Aborting copying of %s\n", path);
        my_exit(-1);
      }
      break;
    }
  }
  my_exit(0);
  return 0;
}


/**
  Calculate database name base on path of Aria file

  @return 0 ok
  @return 1 error
*/

static bool get_database_from_path(char *to, size_t to_length,
                                   const char *path)
{
  S3_INFO s3;
  if (!set_database_and_table_from_path(&s3, path))
  {
    strmake(to, s3.database.str, MY_MIN(s3.database.length, to_length-1));
    return 0;
  }

  if (my_getwd(to, to_length-1, MYF(MY_WME)))
    return 1;
  return get_database_from_path(to, to_length, to);
}


#include "ma_check_standalone.h"

/*
  Declare all symbols from libmyisam.a, to ensure that we don't have
  to include the library as it pulls in ha_myisam.cc
*/

const char *ft_boolean_syntax= 0;
ulong ft_min_word_len=0, ft_max_word_len=0;
const HA_KEYSEG ft_keysegs[FT_SEGS]= {
{
  0,                                            /* charset  */
  HA_FT_WLEN,                                   /* start */
  0,                                            /* null_pos */
  0,                                            /* Bit pos */
  HA_VAR_LENGTH_PART | HA_PACK_KEY,             /* flag */
  HA_FT_MAXBYTELEN,                             /* length */
  63,                                           /* language (will be overwritten
) */
  HA_KEYTYPE_VARTEXT2,                          /* type */
  0,                                            /* null_bit */
  2, 0                                          /* bit_start, bit_length */
},
{
  0, 0, 0, 0, HA_NO_SORT, HA_FT_WLEN, 63, HA_FT_WTYPE, 0, 0, 0
}
};

struct st_mysql_ftparser ft_default_parser=
{
  MYSQL_FTPARSER_INTERFACE_VERSION, 0, 0, 0
};

C_MODE_START
int is_stopword(const char *word, size_t len) { return 0; }
C_MODE_END

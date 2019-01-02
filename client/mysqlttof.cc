/*
   Copyright (c) 2001, 2013, Oracle and/or its affiliates.
   Copyright (c) 2010, 2018, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

/* Convert identifier to filename */

#include "mariadb.h"
#include "client_priv.h"

#define LS_VERSION "1.0"

static my_bool debug_info_flag = 0;
static my_bool debug_check_flag = 0;
static uint my_end_arg;

static my_bool opt_reverse = 0;
static char *opt_charset = 0;

static CHARSET_INFO *charset_info= &my_charset_latin1;

static struct my_option long_options[] =
{
  {"reverse", 'r', "Convert filename to identifier.",
   &opt_reverse, &opt_reverse, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"character-set", 'c',
   "Set the character set.", &opt_charset,
   &opt_charset, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

static void usage();

int main(int argc, char *argv[]) {
  MY_INIT(argv[0]);

  CHARSET_INFO* system_charset_info= &my_charset_utf8_general_ci;
  uint errors, length;
  char *from = argv[argc-1];
  char to[FN_REFLEN + 1];
  const size_t to_length = sizeof(to) - 1;

  if (debug_info_flag)
    my_end_arg= MY_CHECK_ERROR | MY_GIVE_INFO;
  if (debug_check_flag)
    my_end_arg= MY_CHECK_ERROR;

  if (handle_options(&argc, &argv, long_options, NULL) || argc == 0)
  {
    usage();
    my_end(my_end_arg);
    exit(1);
  }

  if (opt_charset)
  {
    CHARSET_INFO *new_cs = get_charset_by_csname(opt_charset, MY_CS_PRIMARY, MYF(MY_WME));
    if (new_cs)
    {
      charset_info = new_cs;
    }
    else
    {
      puts("Charset is not found");
      my_end(my_end_arg);
      exit(1);
    }
  }

  if (opt_reverse)
  {
    length= strconvert(&my_charset_filename, from, FN_REFLEN,
                charset_info,  to, to_length, &errors);
  }
  else
  {
    length= strconvert(charset_info, from, FN_REFLEN,
                      &my_charset_filename, to, to_length, &errors);
  }

  if (unlikely(!length || errors))
  {
    my_end(my_end_arg);
    exit(1);
  }
  else
  {
    puts(to);
    my_end(my_end_arg);
    exit(0);
  }
} /* main */

static void usage(void)
{
  printf("%s Ver %s Distrib %s for %s on %s", my_progname, LS_VERSION,
    MYSQL_SERVER_VERSION, SYSTEM_TYPE, MACHINE_TYPE);
  puts("Utility program for the mariadb .");
  puts("Usage:");
  printf("%s identifier", my_progname);
  puts("");
  printf("%s -r filename", my_progname);
  puts("");
  my_print_help(long_options);
} /* usage */

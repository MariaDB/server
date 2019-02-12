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

/*
   Charactor set conversion utility
*/

#include "mariadb.h"
#include "client_priv.h"

#define CONV_VERSION "1.0"

static my_bool debug_info_flag = 0;
static my_bool debug_check_flag = 0;
static uint my_end_arg;

static char *opt_charset_from = 0;
static char *opt_charset_to = 0;
static my_bool opt_continue = 0;

static CHARSET_INFO *charset_info_from= &my_charset_latin1;
static CHARSET_INFO *charset_info_to= &my_charset_latin1;

static struct my_option long_options[] =
{
  {"from", 'f', "Specifies the encoding of the input.", &opt_charset_from,
   &opt_charset_from, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"to", 't', "Specifies the encoding of the output.", &opt_charset_to,
   &opt_charset_to, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"continue", 'c', "When this option is given, characters that cannot be "
   "converted are silently discarded, instead of leading to a conversion error.",
   &opt_continue, &opt_continue, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

static void usage();
static int load_charset(const char *charset_name, const CHARSET_INFO **charset_info);
static int convert(FILE *infile);

int main(int argc, char *argv[]) {
  MY_INIT(argv[0]);

  if (debug_info_flag)
    my_end_arg= MY_CHECK_ERROR | MY_GIVE_INFO;
  if (debug_check_flag)
    my_end_arg= MY_CHECK_ERROR;

  if (handle_options(&argc, &argv, long_options, NULL))
  {
    usage();
    my_end(my_end_arg);
    exit(1);
  }

  if (opt_charset_from)
  {
    if (load_charset(opt_charset_from, &charset_info_from))
    {
      puts("From charset is not found");
      my_end(my_end_arg);
      exit(1);
    }
  }

  if (opt_charset_to)
  {
    if(load_charset(opt_charset_to, &charset_info_to))
    {
      puts("To charset is not found");
      my_end(my_end_arg);
      exit(1);
    }
  }

  if (isatty(fileno(stdin)))
  {
    if (argc == 0)
    {
      usage();
      my_end(my_end_arg);
      exit(1);
    }

    for(int i = 0; i < argc; i++)
    {
      const char *filename = argv[i];

      FILE *fp;
      if ((fp = fopen(filename, "r")) == NULL)
      {
        printf("can't open file %s", filename);
        my_end(my_end_arg);
        exit(1);
      }

      if (convert(fp))
      {
        my_end(my_end_arg);
        exit(1);
      }

      fclose(fp);
    }
  }
  else
  {
    if (convert(stdin))
    {
      my_end(my_end_arg);
      exit(1);
    }
  }

  my_end(my_end_arg);
  exit(0);
} /* main */

static void usage(void)
{
  printf("%s Ver %s Distrib %s for %s on %s\n", my_progname, CONV_VERSION,
    MYSQL_SERVER_VERSION, SYSTEM_TYPE, MACHINE_TYPE);
  puts("Charactor set conversion utility for MariaDB");
  puts("Usage:");
  printf("%s [-f encoding] [-t encoding] [inputfile ...]\n", my_progname);
  my_print_help(long_options);
} /* usage */

static int load_charset(const char *charset_name, const CHARSET_INFO **charset_info)
{
  CHARSET_INFO *cs = get_charset_by_csname(charset_name, MY_CS_PRIMARY, MYF(MY_WME));
  if (cs)
  {
    *charset_info = cs;
    return 0;
  }
  else
  {
    return 1;
  }
} /* load_charset */

static int convert(FILE *infile)
{
  uint errors, length;
  char from[FN_REFLEN + 1], to[FN_REFLEN + 1];

  while (fgets(from, sizeof(from), infile) != NULL)
  {
    if (strchr(from, '\n'))
    {
      size_t pos_lf = strlen(from) - 1;
      size_t pos_cr = pos_lf - 1;
      from[pos_lf] = '\0';
#ifdef _WIN32
      if (from[pos_cr] == '\r')
        from[strlen(from) - 2] = '\0';
#endif
    }

    if (strlen(from) == 0)
    {
      puts("");
      continue;
    }

    length= strconvert(charset_info_from, from, FN_REFLEN,
                    charset_info_to, to, FN_REFLEN, &errors);

    if (unlikely(!length || errors) && !opt_continue)
      return 1;
    else
      puts(to);
  }

  return 0;
} /* convert */

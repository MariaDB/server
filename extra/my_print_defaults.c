/*
   Copyright (c) 2000, 2011, Oracle and/or its affiliates

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

/*
**  print_default.c:
**  Print all parameters in a default file that will be given to some program.
**
**  Written by Monty
*/

#include <my_global.h>
#include <my_sys.h>
#include <m_string.h>
#include <my_getopt.h>
#include <my_default.h>
#include <mysql_version.h>

#define load_default_groups mysqld_groups
#include <mysqld_default_groups.h>
#undef load_default_groups

my_bool opt_mysqld;

const char *config_file="my";			/* Default config file */
uint verbose= 0, opt_defaults_file_used= 0;
const char *default_dbug_option="d:t:o,/tmp/my_print_defaults.trace";

static struct my_option my_long_options[] =
{
#ifdef DBUG_OFF
  {"debug", '#', "This is a non-debug version. Catch this and exit",
   0,0, 0, GET_DISABLED, OPT_ARG, 0, 0, 0, 0, 0, 0},
#else
  {"debug", '#', "Output debug log", (char**) &default_dbug_option,
   (char**) &default_dbug_option, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"defaults-file", 'c',
   "Read this file only, do not read global or per-user config "
   "files; should be the first option",
   (char**) &config_file, (char*) &config_file, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
  {"defaults-extra-file", 'e',
   "Read this file after the global config file and before the config "
   "file in the users home directory; should be the first option",
   (void *)&my_defaults_extra_file, (void *)&my_defaults_extra_file, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"defaults-group-suffix", 'g',
   "In addition to the given groups, read also groups with this suffix",
   (char**) &my_defaults_group_suffix, (char**) &my_defaults_group_suffix,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"mysqld", 0, "Read the same set of groups that the mysqld binary does.",
   &opt_mysqld, &opt_mysqld, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"no-defaults", 'n', "Return an empty string (useful for scripts).",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"help", '?', "Display this help message and exit.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"verbose", 'v', "Increase the output level",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Output version information and exit.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

static void cleanup_and_exit(int exit_code) __attribute__ ((noreturn));
static void cleanup_and_exit(int exit_code)
{
  my_end(0);
  exit(exit_code);
}

static void version()
{
  printf("%s  Ver 1.7 for %s at %s\n",my_progname,SYSTEM_TYPE, MACHINE_TYPE);
}


static void usage() __attribute__ ((noreturn));
static void usage()
{
  version();
  puts("This software comes with ABSOLUTELY NO WARRANTY. This is free software,\nand you are welcome to modify and redistribute it under the GPL license\n");
  puts("Displays the options from option groups of option files, which is useful to see which options a particular tool will use");
  printf("Usage: %s [OPTIONS] [groups]\n", my_progname);
  my_print_help(my_long_options);
  my_print_default_files(config_file);
  my_print_variables(my_long_options);
  printf("\nExample usage:\n%s --defaults-file=example.cnf client client-server mysql\n", my_progname);
  cleanup_and_exit(0);
}


static my_bool
get_one_option(const struct my_option *opt __attribute__((unused)),
	       const char *argument __attribute__((unused)),
               const char *filename __attribute__((unused)))
{
  switch (opt->id) {
    case 'c':
      opt_defaults_file_used= 1;
      break;
    case 'n':
      cleanup_and_exit(0);
    case 'I':
    case '?':
      usage();
    case 'v':
      verbose++;
      break;
    case 'V':
      version();
      /* fall through */
    case '#':
      DBUG_PUSH(argument ? argument : default_dbug_option);
      break;
  }
  return 0;
}


static int get_options(int *argc,char ***argv)
{
  int ho_error;

  if ((ho_error=handle_options(argc, argv, my_long_options, get_one_option)))
    exit(ho_error);

  return 0;
}

static char *make_args(const char *s1, const char *s2)
{
  char *s= malloc(strlen(s1) + strlen(s2) + 1);
  strmov(strmov(s, s1), s2);
  return s;
}

int main(int argc, char **argv)
{
  int count= 0, error, no_defaults= 0;
  char **load_default_groups= 0, *tmp_arguments[6];
  char **argument, **arguments, **org_argv;
  int nargs, i= 0;
  MY_INIT(argv[0]);

  org_argv= argv;
  if (*argv && !strcmp(*argv, "--no-defaults"))
  {
    argv++;
    ++count;
    no_defaults= 1;
  }
  /* Copy program name and --no-defaults if present*/
  arguments= tmp_arguments;
  memcpy((char*) arguments, (char*) org_argv, (++count)*sizeof(*org_argv));
  arguments[count]= 0;

  /* Check out the args */
  if (get_options(&argc,&argv))
    cleanup_and_exit(1);

  if (!no_defaults)
  {
    if (opt_defaults_file_used)
     arguments[count++]= make_args("--defaults-file=", config_file);
    if (my_defaults_extra_file)
      arguments[count++]= make_args("--defaults-extra-file=",
                                  my_defaults_extra_file);
    if (my_defaults_group_suffix)
      arguments[count++]= make_args("--defaults-group-suffix=",
                                  my_defaults_group_suffix);
    arguments[count]= 0;
  }

  nargs= argc + 1;
  if (opt_mysqld)
    nargs+= array_elements(mysqld_groups);

  if (nargs < 2)
    usage();

  load_default_groups=(char**) my_malloc(PSI_NOT_INSTRUMENTED,
                                         nargs*sizeof(char*), MYF(MY_WME));
  if (!load_default_groups)
    exit(1);
  if (opt_mysqld)
  {
    for (; mysqld_groups[i]; i++)
      load_default_groups[i]= (char*) mysqld_groups[i];
  }
  memcpy(load_default_groups + i, argv, (argc + 1) * sizeof(*argv));
  if ((error= load_defaults(config_file, (const char **) load_default_groups,
			   &count, &arguments)))
  {
    my_end(0);
    if (error == 4)
      return 0;
    if (verbose && opt_defaults_file_used)
    {
      if (error == 1)
	fprintf(stderr, "WARNING: Defaults file '%s' not found!\n",
		config_file);
      /* This error is not available now. For the future */
      if (error == 2)
	fprintf(stderr, "WARNING: Defaults file '%s' is not a regular file!\n",
		config_file);
    }
    return 2;
  }

  for (argument= arguments+1 ; *argument ; argument++)
    puts(*argument);
  my_free(load_default_groups);
  free_defaults(arguments);
  my_end(0);
  exit(0);
}

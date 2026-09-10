/*
   Copyright (c) 2011, 2015, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335  USA
*/

#define VER "1.0"
#include <my_global.h>
#include <m_string.h>
#include <mysql.h>
#include <my_getopt.h>
#include <my_dir.h>
#include <mysql_version.h>
#include <welcome_copyright_notice.h>

#define STR(s) _STR(s)
#define _STR(s) #s

/*
  The build system defines INSTALL_LAYOUT_RPM or INSTALL_LAYOUT_DEB for the
  packaged builds, and neither of them for a binary tarball.
*/
#if defined(INSTALL_LAYOUT_RPM)
#define INSTALL_METHOD_NAME "rpm"
#elif defined(INSTALL_LAYOUT_DEB)
#define INSTALL_METHOD_NAME "deb"
#else
#define INSTALL_METHOD_NAME "tarball"
#endif

/*
  On rpm and deb installations install/uninstall delegate to the system
  package manager. Tarball installations manage plugin files themselves,
  so none of the delegation code applies (and neither do its unix-only
  process primitives).
*/
#if defined(INSTALL_LAYOUT_RPM) || defined(INSTALL_LAYOUT_DEB)
#define PKG_DELEGATION 1
#include <sys/wait.h>
#elif defined(_WIN32)
#include <direct.h>
#endif

#ifndef PKG_DELEGATION
#include <zlib.h>
#include <curl/curl.h>
#include <mysql/service_sha2.h>
#endif

/* Global variables. */
static uint my_end_arg= 0;
static uint opt_verbose=0;
static my_bool opt_dry_run= 0;
static char *opt_file= 0;
static char *opt_sha256= 0;
static char *opt_base_url= 0;
static uint opt_no_defaults= 0;
static uint opt_print_defaults= 0;
static char *opt_datadir=0, *opt_basedir=0,
            *opt_plugin_dir=0, *opt_plugin_ini=0,
            *opt_mysqld=0, *opt_my_print_defaults=0, *opt_lc_messages_dir;
static char bootstrap[FN_REFLEN];


/* plugin struct */
struct st_plugin
{
  const char *name;           /* plugin name */
  const char *so_name;        /* plugin so (library) name */
  const char *components[16]; /* components to load */
} plugin_data;


/* Options */
static struct my_option my_long_options[] =
{
  {"help", '?', "Display this help and exit.", 0, 0, 0, GET_NO_ARG, NO_ARG,
    0, 0, 0, 0, 0, 0},
  {"basedir", 'b', "The basedir for the server.",
    0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"datadir", 'd', "The datadir for the server.",
    0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"plugin-dir", 'p', "The plugin dir for the server.",
    0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"plugin-ini", 'i', "Read plugin information from configuration file "
   "specified instead of from <plugin-dir>/<plugin_name>.ini.",
    0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"dry-run", 0, "Print the commands that install and uninstall would run, "
   "without running them.",
    &opt_dry_run, &opt_dry_run, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"file", 0, "Install the plugin from this local tarball instead of "
   "downloading it.",
    &opt_file, &opt_file, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"base-url", 0, "Plugin repository URL. Overrides the default download "
   "location for tarball installations.",
    &opt_base_url, &opt_base_url, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"sha256", 0, "Expected SHA-256 checksum of the tarball, as published "
   "beside it. Refuse to install if it does not match.",
    &opt_sha256, &opt_sha256, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"no-defaults", 'n', "Do not read values from configuration file.",
    0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"print-defaults", 'P', "Show default values from configuration file.",
    0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"mysqld", 'm', "Path to mysqld executable. Example: /sbin/temp1/mysql/bin",
    0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"my-print-defaults", 'f', "Path to my_print_defaults executable. "
   "Example: /source/temp11/extra",
    0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"lc-messages-dir", 'l', "The error messages dir for the server. ",
    0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"verbose", 'v',
    "More verbose output; you can use this multiple times to get even more "
    "verbose output.",
    0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Output version information and exit.", 0, 0, 0, GET_NO_ARG,
    NO_ARG, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};


/* Methods */
static int process_options(int argc, char *argv[], char *operation);
static int check_access();
static int find_tool(const char *tool_name, char *tool_path);
static int find_plugin(char *tp_path);
static int build_bootstrap_file(char *operation, char *bootstrap);
static int dump_bootstrap_file(char *bootstrap_file);
static int bootstrap_server(char *server_path, char *bootstrap_file);
static void usage(void);
static int run_new_command(int argc, char **argv);
static int validate_plugin_name(const char *name);
static int is_legacy_syntax(int argc, char **argv);
static int detect_install_method(char *basedir, size_t basedir_size);
static int do_search(const char *term);
static int do_install(const char *name, const char *basedir);
static int do_uninstall(const char *name, const char *basedir);
static my_bool get_one_option(const struct my_option *, const char *,
                              const char *);


int main(int argc,char *argv[])
{
  int error= 0;
  char tp_path[FN_REFLEN];
  char server_path[FN_REFLEN];
  char operation[16];

  MY_INIT(argv[0]);
  sf_leaking_memory=1; /* don't report memory leaks on early exits */
  plugin_data.name= 0; /* initialize name                          */

  /* Route only positional arguments, never values belonging to options. */
  if (handle_options(&argc, &argv, my_long_options, get_one_option))
  {
    my_end(my_end_arg);
    return 1;
  }
  if (!is_legacy_syntax(argc, argv))
  {
    error= run_new_command(argc, argv);
    my_end(my_end_arg);
    exit(error);
  }

  if (opt_dry_run)
  {
    fprintf(stderr, "ERROR: --dry-run is not supported with ENABLE/DISABLE.\n");
    my_end(my_end_arg);
    return 1;
  }

  /*
    Parse and validate legacy options, check that configured paths exist,
    locate mysqld and the plugin library, then write the bootstrap SQL.
    Stop if any step fails.
  */
  if ((error= process_options(argc, argv, operation)) ||
      (error= check_access()) ||
      (error= find_tool("mysqld" FN_EXEEXT, server_path)) ||
      (error= find_plugin(tp_path)) ||
      (error= build_bootstrap_file(operation, bootstrap)))
    goto exit;

  /* Dump the bootstrap file if --verbose specified. */
  if (opt_verbose && ((error= dump_bootstrap_file(bootstrap))))
    goto exit;

  /* Start the server in bootstrap mode and execute bootstrap commands */
  error= bootstrap_server(server_path, bootstrap);

exit:
  /* Remove file */
  my_delete(bootstrap, MYF(0));
  if (opt_verbose && error == 0)
  {
    printf("# Operation succeeded.\n");
  }

  my_end(my_end_arg);
  exit(error ? 1 : 0);
  return 0;        /* No compiler warnings */
}


/**
  Create an empty temporary file and return its name.

  @param[out]  filename   The file name of the temporary file
  @param[in]   ext        An extension for the file (optional)

  @retval int error = 1, success = 0
*/

static int make_tempfile(char *filename, const char *ext)
{
  int fd= 0;

  if ((fd= create_temp_file(filename, NullS, ext, 0, MYF(MY_WME))) < 0)
  {
    fprintf(stderr, "ERROR: Cannot generate temporary file. Error code: %d.\n",
            fd);
    return 1;
  }
  my_close(fd, MYF(0));
  return 0;
}


/**
  Get the value of an option from a string read from my_print_defaults output.

  @param[in]  line   The line (string) read from the file
  @param[in]  item   The option to search for (e.g. --datadir)

  @returns NULL if not found, string containing value if found
*/

static char *get_value(char *line, const char *item)
{
  char *destination= 0;
  int item_len= (int)strlen(item);
  int line_len = (int)strlen(line);

  if ((strncasecmp(line, item, item_len) == 0))
  {
    int start= 0;
    char *s= 0;

    s = line + item_len + 1;
    destination= my_strndup(PSI_NOT_INSTRUMENTED, s, line_len - start, MYF(MY_FAE));
    destination[line_len - item_len - 2]= 0;
  }
  return destination;
}


/**
  Run a shell command with popen(). With --verbose, read the pipe and copy
  the command output to stdout.

  @param[in]  cmd   The command to execute.
  @param[in]  mode  Read mode for popen() ("r" at all call sites).

  @return The pclose() status, or -1 if popen() fails.
*/

static int run_command(char* cmd, const char *mode)
{
  char buf[512]= {0};
  FILE *res_file;
  int error;

  if (!(res_file= popen(cmd, mode)))
    return -1;

  if (opt_verbose)
  {
    while (fgets(buf, sizeof(buf), res_file))
    {
      fprintf(stdout, "%s", buf);
    }
  }
  error= pclose(res_file);
  return error;
}


#ifdef _WIN32
/**
  Check to see if there are spaces in a path.

  @param[in]  path  The Windows path to examine.

  @retval int spaces found = 1, no spaces = 0
*/
static int has_spaces(const char *path)
{
  if (strchr(path, ' ') != NULL)
    return 1;
  return 0;
}


/**
  Convert a Unix path to a Windows path.

  @param[in]  argument  The path whose separators will be converted.

  @returns string containing path with / changed to \\
*/
static char *convert_path(const char *argument)
{
  /* Convert / to \\ to make Windows paths */
  char *winfilename= my_strdup(PSI_NOT_INSTRUMENTED, argument, MYF(MY_FAE));
  char *pos, *end;
  size_t length= strlen(argument);

  for (pos= winfilename, end= pos+length ; pos < end ; pos++)
  {
    if (*pos == '/')
    {
      *pos= '\\';
    }
  }
  return winfilename;
}


/**
  Add quotes if the path has spaces in it.

  @param[in]  path  The Windows path to examine.

  @returns A copy of the path, enclosed in double quotes if it contains spaces.
*/
static char *add_quotes(const char *path)
{
  char windows_cmd_friendly[FN_REFLEN];

  if (has_spaces(path))
    snprintf(windows_cmd_friendly, sizeof(windows_cmd_friendly),
             "\"%s\"", path);
  else
    snprintf(windows_cmd_friendly, sizeof(windows_cmd_friendly),
             "%s", path);
  return my_strdup(PSI_NOT_INSTRUMENTED, windows_cmd_friendly, MYF(MY_FAE));
}
#endif


/**
  Read server defaults using my_print_defaults --mysqld.

  Fill unset --datadir, --basedir, --plugin-dir, and --lc-messages-dir
  values. This function does not read --plugin-ini from option files.

  @retval int error = 1, success = 0
*/

static int get_default_values()
{
  char tool_path[FN_REFLEN];
  char defaults_cmd[FN_REFLEN];
  char defaults_file[FN_REFLEN];
  char line[FN_REFLEN];
  int error= 0;
  int ret= 0;
  FILE *file= 0;

  memset(tool_path, 0, FN_REFLEN);
  if ((error= find_tool("my_print_defaults" FN_EXEEXT, tool_path)))
    goto exit;
  else
  {
    if ((error= make_tempfile(defaults_file, "txt")))
      goto exit;

#ifdef _WIN32
    {
      char *format_str= 0;

      if (has_spaces(tool_path) || has_spaces(defaults_file))
        format_str = "\"%s --mysqld > %s\"";
      else
        format_str = "%s --mysqld > %s";

      snprintf(defaults_cmd, sizeof(defaults_cmd), format_str,
               add_quotes(tool_path), add_quotes(defaults_file));
      if (opt_verbose)
      {
        printf("# my_print_defaults found: %s\n", tool_path);
      }
    }
#else
    snprintf(defaults_cmd, sizeof(defaults_cmd),
             "%s --mysqld > %s", tool_path, defaults_file);
#endif

    /* Execute the command */
    if (opt_verbose)
    {
      printf("# Command: %s\n", defaults_cmd);
    }
    error= run_command(defaults_cmd, "r");
    if (error)
    {
      fprintf(stderr, "ERROR: my_print_defaults failed. Error code: %d.\n",
              ret);
      goto exit;
    }
    /* Now open the file and read the defaults we want. */
    file= fopen(defaults_file, "r");
    if (file == NULL)
    {
      fprintf(stderr, "ERROR: failed to open file %s: %s.\n", defaults_file,
              strerror(errno));
      goto exit;
    }
    while (fgets(line, FN_REFLEN, file) != NULL)
    {
      char *value= 0;

      if ((opt_datadir == 0) && ((value= get_value(line, "--datadir"))))
      {
        opt_datadir= my_strdup(PSI_NOT_INSTRUMENTED, value, MYF(MY_FAE));
      }
      if ((opt_basedir == 0) && ((value= get_value(line, "--basedir"))))
      {
        opt_basedir= my_strdup(PSI_NOT_INSTRUMENTED, value, MYF(MY_FAE));
      }
      if ((opt_plugin_dir == 0) && ((value= get_value(line, "--plugin_dir")) ||
          (value= get_value(line, "--plugin-dir"))))
      {
        opt_plugin_dir= my_strdup(PSI_NOT_INSTRUMENTED, value, MYF(MY_FAE));
      }
      if ((opt_lc_messages_dir == 0) &&
          ((value= get_value(line, "--lc_messages_dir")) ||
          (value= get_value(line, "--lc_messages-dir")) ||
          (value= get_value(line, "--lc-messages_dir")) ||
          (value= get_value(line, "--lc-messages-dir"))))
      {
        opt_lc_messages_dir= my_strdup(PSI_NOT_INSTRUMENTED, value, MYF(MY_FAE));
      }

    }
  }
exit:
  if (file)
  {
    fclose(file);
    /* Remove file */
    my_delete(defaults_file, MYF(0));
  }
  return error;
}


/**
  Print usage.
*/

static void usage(void)
{
  print_version();
  puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2011"));
  puts("Manage MariaDB plugins across package managers and binary distributions.");
  printf("\nUsage:\n");
  printf("  %s search [<plugin_name>]\n", my_progname);
  printf("  %s install <plugin_name>\n", my_progname);
  printf("  %s uninstall <plugin_name>\n\n", my_progname);
  printf("Legacy syntax (deprecated, kept for backward compatibility):\n");
  printf("  %s [options] <plugin> ENABLE|DISABLE\n\nOptions:\n", my_progname);
  my_print_help(my_long_options);
  puts("\n");
}


/**
  Print current path option values, including command-line values parsed
  so far. Fill unset server paths via get_default_values() before printing.
*/

static void print_default_values(void)
{
  printf("%s would have been started with the following arguments:\n",
         my_progname);
  get_default_values();
  if (opt_datadir)
  {
    printf("--datadir=%s ", opt_datadir);
  }
  if (opt_basedir)
  {
    printf("--basedir=%s ", opt_basedir);
  }
  if (opt_plugin_dir)
  {
    printf("--plugin_dir=%s ", opt_plugin_dir);
  }
  if (opt_plugin_ini)
  {
    printf("--plugin_ini=%s ", opt_plugin_ini);
  }
  if (opt_mysqld)
  {
    printf("--mysqld=%s ", opt_mysqld);
  }
  if (opt_my_print_defaults)
  {
    printf("--my_print_defaults=%s ", opt_my_print_defaults);
  }
  if (opt_lc_messages_dir)
  {
    printf("--lc_messages_dir=%s ", opt_lc_messages_dir);
  }
  printf("\n");
}


/**
  Handle a parsed option.

  @param[in]  opt        The option being processed.
  @param[in]  argument   The argument value to process.
  @param[in]  filename   Unused option-file name.
*/

static my_bool
get_one_option(const struct my_option *opt,
               const char *argument,
               const char *filename __attribute__((unused)))
{
  switch(opt->id) {
  case 'n':
    opt_no_defaults++;
    break;
  case 'P':
    opt_print_defaults++;
    print_default_values();
    break;
  case 'v':
    opt_verbose++;
    break;
  case 'V':
    print_version();
    exit(0);
    break;
  case '?':
  case 'I':          /* Info */
    usage();
    exit(0);
  case 'd':
    opt_datadir= my_strdup(PSI_NOT_INSTRUMENTED, argument, MYF(MY_FAE));
    break;
  case 'b':
    opt_basedir= my_strdup(PSI_NOT_INSTRUMENTED, argument, MYF(MY_FAE));
    break;
  case 'p':
    opt_plugin_dir= my_strdup(PSI_NOT_INSTRUMENTED, argument, MYF(MY_FAE));
    break;
  case 'i':
    opt_plugin_ini= my_strdup(PSI_NOT_INSTRUMENTED, argument, MYF(MY_FAE));
    break;
  case 'm':
    opt_mysqld= my_strdup(PSI_NOT_INSTRUMENTED, argument, MYF(MY_FAE));
    break;
  case 'f':
    opt_my_print_defaults= my_strdup(PSI_NOT_INSTRUMENTED, argument, MYF(MY_FAE));
    break;
  case 'l':
    opt_lc_messages_dir= my_strdup(PSI_NOT_INSTRUMENTED, argument, MYF(MY_FAE));
    break;

  }
  return 0;
}


/**
  Check whether my_stat() succeeds for a path.

  @param[in]  filename  File to locate.

  @retval 1 The path could be stat'ed.
  @retval 0 The stat call failed.
*/

static int file_exists(char * filename)
{
  MY_STAT stat_arg;

  if (!my_stat(filename, &stat_arg, MYF(0)))
  {
    return 0;
  }
  return 1;
}


/**
  Look for a file in the given subdirectory of base_path.

  @param[in]  base_path  Original path to use.
  @param[in]  tool_name  Name of the tool to locate.
  @param[in]  subdir     The sub directory to search.
  @param[out] tool_path  If tool found, return complete path.

  @retval 1 File found; tool_path contains its path.
  @retval 0 File not found.
*/

static int search_dir(const char *base_path, const char *tool_name,
                      const char *subdir, char *tool_path)
{
  char new_path[FN_REFLEN];
  char source_path[FN_REFLEN];

  safe_strcpy(source_path, sizeof(source_path), base_path);
  safe_strcat(source_path, sizeof(source_path), subdir);
  fn_format(new_path, tool_name, source_path, "", MY_UNPACK_FILENAME);
  if (file_exists(new_path))
  {
    strcpy(tool_path, new_path);
    return 1;
  }
  return 0;
}


/**
  Look for a file in base_path and its known tool subdirectories.

  @param[in]  base_path  Original path to use.
  @param[in]  tool_name  Name of the tool to locate.
  @param[out] tool_path  If tool found, return complete path.

  @retval 1 File found; tool_path contains its path.
  @retval 0 File not found.
*/

static int search_paths(const char *base_path, const char *tool_name,
                        char *tool_path)
{
  int i= 0;

  static const char *paths[]= {
    "", "/share/",  "/scripts/", "/bin/", "/sbin/", "/libexec/",
    "/mysql/", "/sql/",
  };
  for (i = 0 ; i < (int)array_elements(paths); i++)
  {
    if (search_dir(base_path, tool_name, paths[i], tool_path))
    {
      return 1;
    }
  }
  return 0;
}


/**
  Read --plugin-ini, or config_file under opt_plugin_dir, into plugin_data.

  @retval int error = 1, success = 0
*/

static int load_plugin_data(char *plugin_name, char *config_file)
{
  FILE *file_ptr;
  char path[FN_REFLEN];
  char line[1024];
  const char *reason= 0;
  char *res;
  int i= -1;

  if (opt_plugin_ini == 0)
  {
    fn_format(path, config_file, opt_plugin_dir, "", MYF(0));
    opt_plugin_ini= my_strdup(PSI_NOT_INSTRUMENTED, path, MYF(MY_FAE));
  }
  if (!file_exists(opt_plugin_ini))
  {
    reason= "File does not exist.";
    goto error;
  }

  file_ptr= fopen(opt_plugin_ini, "r");
  if (file_ptr == NULL)
  {
    reason= "Cannot open file.";
    goto error;
  }

  /* save name */
  plugin_data.name= my_strdup(PSI_NOT_INSTRUMENTED, plugin_name, MYF(MY_WME));

  /* Read plugin components */
  while (i < 16)
  {
    size_t line_len;

    res= fgets(line, sizeof(line), file_ptr);
    line_len= strlen(line);

    /* Strip the trailing newline. */
    if (line[line_len - 1] == '\n')
      line[line_len - 1]= '\0';

    if (res == NULL)
    {
      if (i < 1)
      {
        reason= "Bad format in plugin configuration file.";
        fclose(file_ptr);
        goto error;
      }
      break;
    }
    if ((line[0] == '#') || (line[0] == '\n')) /* skip comment and blank lines */
    {
      continue;
    }
    if (i == -1) /* if first pass, read this line as so_name */
    {
      /* Add proper file extension for soname */
      if (safe_strcpy_truncated(line + line_len - 1, sizeof line, FN_SOEXT))
      {
        reason= "Plugin name too long.";
        fclose(file_ptr);
        goto error;
      }
      /* save so_name */
      plugin_data.so_name= my_strdup(PSI_NOT_INSTRUMENTED, line, MYF(MY_WME|MY_ZEROFILL));
      i++;
    }
    else
    {
      if (line_len > 0)
      {
        plugin_data.components[i]= my_strdup(PSI_NOT_INSTRUMENTED, line, MYF(MY_WME));
        i++;
      }
      else
      {
        plugin_data.components[i]= NULL;
      }
    }
  }

  fclose(file_ptr);
  return 0;

error:
  fprintf(stderr, "ERROR: Cannot read plugin config file %s. %s\n",
          plugin_name, reason);
  return 1;
}


/**
  Validate required options and read the plugin configuration.
  On success, operation contains the supplied ENABLE or DISABLE argument,
  preserving its case.

  @param[in]  argc       The number of arguments.
  @param[in]  argv       The arguments.
  @param[out] operation  The operation chosen (enable|disable)

  @retval int error = 1, success = 0
*/

static int check_options(int argc, char **argv, char *operation)
{
  int i= 0;                    /* loop counter */
  int num_found= 0;            /* number of options found (shortcut loop) */
  char config_file[FN_REFLEN+1]; /* configuration file name */
  char plugin_name[FN_REFLEN+1]; /* plugin name */

  /* Form prefix strings for the options. */
  const char *basedir_prefix = "--basedir=";
  size_t basedir_len= strlen(basedir_prefix);
  const char *datadir_prefix = "--datadir=";
  size_t datadir_len= strlen(datadir_prefix);
  const char *plugin_dir_prefix = "--plugin_dir=";
  size_t plugin_dir_len= strlen(plugin_dir_prefix);

  *plugin_name= '\0';
  for (i = 0; i < argc && num_found < 5; i++)
  {

    if (!argv[i])
    {
      continue;
    }
    if ((strcasecmp(argv[i], "ENABLE") == 0) ||
        (strcasecmp(argv[i], "DISABLE") == 0))
    {
      strcpy(operation, argv[i]);
      num_found++;
    }
    else if ((strncasecmp(argv[i], basedir_prefix, basedir_len) == 0) &&
             !opt_basedir)
    {
      opt_basedir= my_strndup(PSI_NOT_INSTRUMENTED, argv[i]+basedir_len,
                              strlen(argv[i])-basedir_len, MYF(MY_FAE));
      num_found++;
    }
    else if ((strncasecmp(argv[i], datadir_prefix, datadir_len) == 0) &&
             !opt_datadir)
    {
      opt_datadir= my_strndup(PSI_NOT_INSTRUMENTED, argv[i]+datadir_len,
                              strlen(argv[i])-datadir_len, MYF(MY_FAE));
      num_found++;
    }
    else if ((strncasecmp(argv[i], plugin_dir_prefix, plugin_dir_len) == 0) &&
             !opt_plugin_dir)
    {
      opt_plugin_dir= my_strndup(PSI_NOT_INSTRUMENTED, argv[i]+plugin_dir_len,
                                 strlen(argv[i])-plugin_dir_len, MYF(MY_FAE));
      num_found++;
    }
    /* Treat the remaining argument as the plugin name. */
    else
    {
      if (safe_strcpy_truncated(plugin_name, sizeof(plugin_name)-1, argv[i]) ||
          safe_strcpy_truncated(config_file, sizeof(config_file)-1, argv[i]) ||
          safe_strcat(config_file, sizeof(config_file), ".ini"))
      {
        fprintf(stderr, "ERROR: argument is too long.\n");
        return 1;
      }
    }
  }

  if (!opt_basedir)
  {
    fprintf(stderr, "ERROR: Missing --basedir option.\n");
    return 1;
  }

  if (!opt_datadir)
  {
    fprintf(stderr, "ERROR: Missing --datadir option.\n");
    return 1;
  }

  if (!opt_plugin_dir)
  {
    fprintf(stderr, "ERROR: Missing --plugin_dir option.\n");
    return 1;
  }
  /* If a plugin was specified, read the config file. */
  else if (strlen(plugin_name) > 0)
  {
    if (load_plugin_data(plugin_name, config_file))
    {
      return 1;
    }
    if (strcasecmp(plugin_data.name, plugin_name) != 0)
    {
      fprintf(stderr, "ERROR: plugin name requested does not match config "
              "file data.\n");
      return 1;
    }
  }
  else
  {
    fprintf(stderr, "ERROR: No plugin specified.\n");
    return 1;
  }

  if ((strlen(operation) == 0))
  {
    fprintf(stderr, "ERROR: missing operation. Please specify either "
            "'<plugin> ENABLE' or '<plugin> DISABLE'.\n");
    return 1;
  }

  return 0;
}


/**
  Read defaults unless disabled and validate the already-parsed legacy inputs.
  Handle informational options without enabling or disabling a plugin.

  @param[in]   argc       Count of arguments
  @param[in]   argv       Array of arguments
  @param[out]  operation  Operation (ENABLE or DISABLE)

  @return 0 on success, an option error code, or -1 to stop processing.
*/

static int process_options(int argc, char *argv[], char *operation)
{
  int error= 0;

  /* If the print defaults option used, exit. */
  if (opt_print_defaults)
    return -1;

  /* Add a trailing directory separator if not present */
  if (opt_basedir)
  {
    size_t basedir_len= strlength(opt_basedir);
    if (opt_basedir[basedir_len - 1] != FN_LIBCHAR ||
        opt_basedir[basedir_len - 1] != FN_LIBCHAR2)
    {
      char buff[FN_REFLEN];
      if (basedir_len + 2 > FN_REFLEN)
        return -1;

      memcpy(buff, opt_basedir, basedir_len);
      buff[basedir_len]= '/';
      buff[basedir_len + 1]= '\0';

      my_free(opt_basedir);
      opt_basedir= my_strdup(PSI_NOT_INSTRUMENTED, buff, MYF(MY_FAE));
    }
  }

  /*
    Read defaults unless --no-defaults was given; stop on a reported error.
  */
  if (!opt_no_defaults && ((error= get_default_values())))
    return -1;

  /*
    Validate required options and the operation, and read --plugin-ini or
    <plugin-dir>/<plugin_name>.ini.
  */
  operation[0]= '\0';
  if ((error= check_options(argc, argv, operation)))
    return error;

  if (opt_verbose)
  {
    printf("#    basedir = %s\n", opt_basedir);
    printf("# plugin_dir = %s\n", opt_plugin_dir);
    printf("#    datadir = %s\n", opt_datadir);
    printf("# plugin_ini = %s\n", opt_plugin_ini);
    if (opt_lc_messages_dir != 0)
      printf("# lc_messages_dir = %s\n", opt_lc_messages_dir);
  }

  return 0;
}


/**
  Check that configured paths exist using F_OK.
  This does not check file types or read, write, or execute permissions.

  @return 0 if all existence checks succeed, otherwise the my_access() error.
*/

static int check_access()
{
  int error= 0;

  if ((error= my_access(opt_basedir, F_OK)))
  {
    fprintf(stderr, "ERROR: Cannot access basedir at '%s'.\n",
            opt_basedir);
    goto exit;
  }
  if ((error= my_access(opt_plugin_dir, F_OK)))
  {
    fprintf(stderr, "ERROR: Cannot access plugin_dir at '%s'.\n",
            opt_plugin_dir);
    goto exit;
  }
  if ((error= my_access(opt_datadir, F_OK)))
  {
    fprintf(stderr, "ERROR: Cannot access datadir at '%s'.\n",
            opt_datadir);
    goto exit;
  }
  if (opt_plugin_ini && (error= my_access(opt_plugin_ini, F_OK)))
  {
    fprintf(stderr, "ERROR: Cannot access plugin config file at '%s'.\n",
            opt_plugin_ini);
    goto exit;
  }
  if (opt_mysqld && (error= my_access(opt_mysqld, F_OK)))
  {
    fprintf(stderr, "ERROR: Cannot access mariadbd path '%s'.\n",
            opt_mysqld);
    goto exit;
  }
  if (opt_my_print_defaults && (error= my_access(opt_my_print_defaults, F_OK)))
  {
    fprintf(stderr, "ERROR: Cannot access my-print-defaults path '%s'.\n",
            opt_my_print_defaults);
    goto exit;
  }
  if (opt_lc_messages_dir && (error= my_access(opt_lc_messages_dir, F_OK)))
  {
    fprintf(stderr, "ERROR: Cannot access lc-messages-dir path '%s'.\n",
            opt_lc_messages_dir);
    goto exit;
  }

exit:
  return error;
}


/**
  Locate the tool and form tool path.

  @param[in]  tool_name  Name of the tool to locate.
  @param[out] tool_path  If tool found, return complete path.

  @retval int error = 1, success = 0
*/

static int find_tool(const char *tool_name, char *tool_path)
{
  int i= 0;

  const char *paths[]= {
    opt_mysqld, opt_basedir, opt_my_print_defaults, "/usr",
    "/usr/local/mysql", "/usr/sbin", "/usr/share", "/extra", "/extra/debug",
    "/extra/release", "/bin", "/usr/bin", "/mysql/bin"
  };
  for (i= 0; i < (int)array_elements(paths); i++)
  {
    if (paths[i] && (search_paths(paths[i], tool_name, tool_path)))
      goto found;
  }
  fprintf(stderr, "WARNING: Cannot find %s.\n", tool_name);
  return 1;
found:
  if (opt_verbose)
    printf("# Found tool '%s' as '%s'.\n", tool_name, tool_path);
  return 0;
}


/**
  Find the library named by plugin_data.so_name under opt_plugin_dir.

  @param[out] tp_path   The path to the plugin library.

  @retval int error = 1, success = 0
*/

static int find_plugin(char *tp_path)
{
  /* Check for existence of plugin */
  fn_format(tp_path, plugin_data.so_name, opt_plugin_dir, "", MYF(0));
  if (!file_exists(tp_path))
  {
    fprintf(stderr, "ERROR: The plugin library is missing or in a different"
            " location.\n");
    return 1;
  }
  else if (opt_verbose)
  {
    printf("# Found plugin '%s' as '%s'\n", plugin_data.name, tp_path);
  }
  return 0;
}


/**
  Build the bootstrap file.

  Create a new file and populate it with SQL commands to ENABLE or DISABLE
  the plugin via REPLACE and DELETE operations on the mysql.plugin table.

  @param[in]  operation  The type of operation (ENABLE or DISABLE)
  @param[out] bootstrap  Buffer receiving the temporary SQL file name

  @retval int error = 1, success = 0
*/

static int build_bootstrap_file(char *operation, char *bootstrap)
{
  int error= 0;
  FILE *file= 0;

  /*
    Use plugin_data from the configuration file to write the SQL.
    REPLACE updates existing component rows if the library name changes.
    The caller runs mysqld in bootstrap mode to apply these table changes.
  */
  if ((error= make_tempfile(bootstrap, "sql")))
  {
    /* Fail if we cannot create a temporary file for the bootstrap commands. */
    fprintf(stderr, "ERROR: Cannot create bootstrap file.\n");
    goto exit;
  }
  if ((file= fopen(bootstrap, "w+")) == NULL)
  {
    fprintf(stderr, "ERROR: Cannot open bootstrap file for writing.\n");
    error= 1;
    goto exit;
  }
  if (strcasecmp(operation, "enable") == 0)
  {
    int i= 0;
    fprintf(file, "REPLACE INTO mysql.plugin VALUES ");
    for (i= 0; i < (int)array_elements(plugin_data.components); i++)
    {
      /* stop when we read the end of the symbol list - marked with NULL */
      if (plugin_data.components[i] == NULL)
      {
        break;
      }
      if (i > 0)
      {
        fprintf(file, ", ");
      }
      fprintf(file, "('%s','%s')",
              plugin_data.components[i], plugin_data.so_name);
    }
    fprintf(file, ";\n");
    if (opt_verbose)
    {
      printf("# Enabling %s...\n", plugin_data.name);
    }
  }
  else
  {
    fprintf(file,
            "DELETE FROM mysql.plugin WHERE dl = '%s';", plugin_data.so_name);
    if (opt_verbose)
    {
      printf("# Disabling %s...\n", plugin_data.name);
    }
  }

exit:
  fclose(file);
  return error;
}


/**
  Print the first line of the bootstrap file, up to the query buffer limit.

  @param[in]  bootstrap_file  Name of bootstrap file to read

  @retval int error = 1, success = 0
*/

static int dump_bootstrap_file(char *bootstrap_file)
{
  char *ret= 0;
  int error= 0;
  char query_str[512];
  FILE *file= 0;

  if ((file= fopen(bootstrap_file, "r")) == NULL)
  {
    fprintf(stderr, "ERROR: Cannot open bootstrap file for reading.\n");
    error= 1;
    goto exit;
  }
  ret= fgets(query_str, 512, file);
  if (ret == 0)
  {
    fprintf(stderr, "ERROR: Cannot read bootstrap file.\n");
    error= 1;
    goto exit;
  }
  printf("# Query: %s\n", query_str);

exit:
  if (file)
  {
    fclose(file);
  }
  return error;
}


/**
  Run mysqld in bootstrap mode with the SQL file redirected to standard input.
  The SQL updates mysql.plugin to control plugin loading on later normal
  server starts. On non-Windows platforms, --no-defaults skips option files.

  @param[in]  server_path     Path to server executable
  @param[in]  bootstrap_file  Name of bootstrap file to read

  @return The status from run_command().
*/

static int bootstrap_server(char *server_path, char *bootstrap_file)
{
  char bootstrap_cmd[FN_REFLEN]= {0};
  char lc_messages_dir_str[FN_REFLEN]= {0};
  int error= 0;

#ifdef _WIN32
  char *format_str= 0;
  const char *verbose_str= NULL;
#endif

  if (opt_lc_messages_dir != NULL)
    snprintf(lc_messages_dir_str, sizeof(lc_messages_dir_str), "--lc-messages-dir=%s",
             opt_lc_messages_dir);

#ifdef _WIN32
  if (opt_verbose)
    verbose_str= "--console";
  else
    verbose_str= "";

  if (has_spaces(opt_datadir) || has_spaces(opt_basedir) ||
      has_spaces(bootstrap_file) || has_spaces(lc_messages_dir_str))
    format_str= "\"%s %s --bootstrap --datadir=%s --basedir=%s %s <%s\"";
  else
    format_str= "%s %s --bootstrap --datadir=%s --basedir=%s %s <%s";

  snprintf(bootstrap_cmd, sizeof(bootstrap_cmd), format_str,
           add_quotes(convert_path(server_path)), verbose_str,
           add_quotes(opt_datadir), add_quotes(opt_basedir),
           add_quotes(lc_messages_dir_str), add_quotes(bootstrap_file));
#else
  snprintf(bootstrap_cmd, sizeof(bootstrap_cmd),
           "%s --no-defaults --bootstrap --datadir=%s --basedir=%s %s"
           " <%s", server_path, opt_datadir, opt_basedir, lc_messages_dir_str, bootstrap_file);
#endif

  /* Execute the command */
  if (opt_verbose)
  {
    printf("# Command: %s\n", bootstrap_cmd);
  }
  error= run_command(bootstrap_cmd, "r");
  if (error)
    fprintf(stderr,
            "ERROR: Unexpected result from bootstrap. Error code: %d.\n",
            error);

  return error;
}


/**
  Detect the legacy "<plugin> ENABLE|DISABLE" command line syntax.

  Options have already been parsed; only positional arguments remain.

  @param[in]  argc  The number of arguments.
  @param[in]  argv  The arguments.

  @retval int legacy syntax = 1, new syntax = 0
*/

static int is_legacy_syntax(int argc, char **argv)
{
  int i;

  for (i= 0; i < argc; i++)
  {
    /*
      Whichever keyword comes first decides, so that "search enable" is a
      search for the word enable, while "myplugin ENABLE" stays the
      deprecated syntax.
    */
    if (strcmp(argv[i], "search") == 0 ||
        strcmp(argv[i], "install") == 0 ||
        strcmp(argv[i], "uninstall") == 0)
      return 0;
    if (strcasecmp(argv[i], "ENABLE") == 0 ||
        strcasecmp(argv[i], "DISABLE") == 0)
      return 1;
  }
  return 0;
}


/**
  Check that a plugin name contains only safe characters.

  The name is later used to construct package names and file paths, so
  only lower case ASCII letters, digits, '_' and '-' are accepted. The name is
  expected to be normalized to lower case before this check.

  @param[in]  name  The normalized plugin name.

  @retval int error = 1, success = 0
*/

static int validate_plugin_name(const char *name)
{
  const char *p;

  if (*name == '\0')
  {
    fprintf(stderr, "ERROR: plugin name cannot be empty.\n");
    return 1;
  }
  for (p= name; *p; p++)
  {
    if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') &&
        *p != '_' && *p != '-')
    {
      fprintf(stderr, "ERROR: invalid character '%c' in plugin name. "
              "Use only [a-z0-9_-].\n", *p);
      return 1;
    }
  }
  return 0;
}

/**
  Check the argv[0]-derived location against the compiled install layout.

  The installation method is known at build time, so only the location has
  to be checked. It is taken from argv[0] and not from the server, as one
  machine can have several server installations.

  @param[out]  basedir       The base directory, empty for rpm and deb,
                             where the package manager owns the files.
  @param[in]   basedir_size  The size of the basedir buffer.

  @retval int error = 1, success = 0
*/

static int detect_install_method(char *basedir, size_t basedir_size)
{
  char self_path[FN_REFLEN], real_path[FN_REFLEN], real_dir[FN_REFLEN];
  size_t length;
#if !defined(INSTALL_LAYOUT_RPM) && !defined(INSTALL_LAYOUT_DEB)
  char plugin_dir[FN_REFLEN];
  char *slash;
#endif

  /*
    my_path() searches PATH when argv[0] is a bare program name. The path
    is resolved afterwards, so that a symbolic link, like the one for the
    old mysql_plugin name, does not hide where the tool is installed.
  */
  my_path(self_path, my_progname, "");
  if (!self_path[0] ||
      safe_strcat(self_path, sizeof(self_path), base_name(my_progname)) ||
      my_realpath(real_path, self_path, MYF(0)))
  {
    fprintf(stderr, "ERROR: cannot resolve the location of '%s'.\n",
            my_progname);
    return 1;
  }

  dirname_part(real_dir, real_path, &length);

  length= strlen(real_dir);
  while (length > 1 && (real_dir[length - 1] == FN_LIBCHAR ||
                        real_dir[length - 1] == FN_LIBCHAR2))
    real_dir[--length]= '\0';

#if defined(INSTALL_LAYOUT_RPM) || defined(INSTALL_LAYOUT_DEB)
  if (strcmp(real_dir, STR(INSTALL_BINDIRABS)) != 0)
  {
    fprintf(stderr, "ERROR: this is a %s build, but it runs from '%s' "
            "instead of '%s', so it is not part of a %s installation.\n",
            INSTALL_METHOD_NAME, real_dir, STR(INSTALL_BINDIRABS),
            INSTALL_METHOD_NAME);
    return 1;
  }
  basedir[0]= '\0';
#else
  /* The base directory is one level above the directory of the tool. */
  safe_strcpy(basedir, basedir_size, real_dir);
  slash= strrchr(basedir, FN_LIBCHAR);
  if (!slash)
    slash= strrchr(basedir, FN_LIBCHAR2);
  if (!slash)
  {
    fprintf(stderr, "ERROR: cannot determine the MariaDB base directory "
            "from '%s'.\n", real_dir);
    return 1;
  }
  *slash= '\0';

  safe_strcpy(plugin_dir, sizeof(plugin_dir), basedir);
  safe_strcat(plugin_dir, sizeof(plugin_dir), "/" STR(INSTALL_PLUGINDIR));
  if (!file_exists(plugin_dir))
  {
    fprintf(stderr, "ERROR: '%s' does not look like a MariaDB installation, "
            "'%s' not found.\n", basedir, plugin_dir);
    return 1;
  }
#endif
  return 0;
}


#ifdef PKG_DELEGATION

/**
  Pick the package manager to delegate to.

  On deb installations it is always apt-get (the script-stable interface,
  unlike apt). On rpm installations dnf and zypper manage the same rpm
  database, so whichever is present is usable; dnf is tried first.

  @retval const char*  the program name, or NULL with an error printed
*/

static const char *get_package_manager(void)
{
#if defined(INSTALL_LAYOUT_DEB)
  return "apt-get";
#else
  char dir[FN_REFLEN];
  if (find_file_in_path(dir, "dnf"))
    return "dnf";
  if (find_file_in_path(dir, "zypper"))
    return "zypper";
  fprintf(stderr, "ERROR: no package manager found: neither dnf nor zypper "
          "is in PATH.\n");
  return NULL;
#endif
}


/**
  Require root for native package changes, but allow non-root dry-runs.

  @param[in]  verb  The command name, for the error message.

  @retval int error = 1, success = 0
*/

static int check_root(const char *verb)
{
  if (!opt_dry_run && geteuid() != 0)
  {
    fprintf(stderr, "ERROR: '%s' requires root privileges. "
            "Run as root or with sudo.\n", verb);
    return 1;
  }
  return 0;
}


/**
  Run a command and wait for it to finish.

  Execute directly without shell expansion. The package manager still
  interprets its own arguments. The child inherits the standard streams: the
  package manager talks to the user directly, including its own
  confirmation prompts and progress output.

  @param[in]  cmd_argv  NULL-terminated argument vector.

  @retval int  the command exit code, 127 if it could not be run
*/

static int run_argv(char **cmd_argv)
{
  pid_t pid;
  int status;

  /*
    --dry-run only stops the commands that change the system. The queries
    that read the package database still run, so that what is printed is
    what would really be executed, package names resolved and all.
  */
  if (opt_dry_run)
  {
    int i;
    for (i= 0; cmd_argv[i]; i++)
      printf("%s%s", i ? " " : "", cmd_argv[i]);
    printf("\n");
    return 0;
  }

  fflush(stdout);
  fflush(stderr);
  if ((pid= fork()) < 0)
  {
    fprintf(stderr, "ERROR: cannot fork: %s.\n", strerror(errno));
    return 127;
  }
  if (pid == 0)
  {
    execvp(cmd_argv[0], cmd_argv);
    fprintf(stderr, "ERROR: cannot run '%s': %s.\n", cmd_argv[0],
            strerror(errno));
    _exit(127);
  }
  while (waitpid(pid, &status, 0) < 0)
  {
    if (errno != EINTR)
    {
      fprintf(stderr, "ERROR: cannot wait for '%s': %s.\n", cmd_argv[0],
              strerror(errno));
      return 127;
    }
  }
  if (WIFSIGNALED(status))
  {
    fprintf(stderr, "ERROR: '%s' was terminated by signal %d.\n",
            cmd_argv[0], WTERMSIG(status));
    return 127;
  }
  return WEXITSTATUS(status);
}


/**
  Run a command and capture its standard output.

  Standard error stays on the terminal, unless quiet_stderr is set, for
  commands whose failure is an expected answer and not an error. The pipe
  is read to the end, so that the child never blocks writing.

  @param[in]   cmd_argv      NULL-terminated argument vector.
  @param[out]  out           Initialized string, replaced by the output.
  @param[in]   quiet_stderr  Discard the command's standard error.

  @retval int  the command exit code, 127 if it could not be run
*/

static int run_argv_capture(char **cmd_argv, DYNAMIC_STRING *out,
                            int quiet_stderr)
{
  char buf[4096];
  int fds[2];
  pid_t pid;
  int status;
  ssize_t n;
  my_bool oom= FALSE;

  dynstr_set(out, "");
  if (pipe(fds))
  {
    fprintf(stderr, "ERROR: cannot create a pipe: %s.\n", strerror(errno));
    return 127;
  }
  fflush(stdout);
  fflush(stderr);
  if ((pid= fork()) < 0)
  {
    fprintf(stderr, "ERROR: cannot fork: %s.\n", strerror(errno));
    close(fds[0]);
    close(fds[1]);
    return 127;
  }
  if (pid == 0)
  {
    dup2(fds[1], STDOUT_FILENO);
    /* Metadata parsers consume stable field labels, not translated output. */
    if (setenv("LC_ALL", "C", 1))
      _exit(127);
    if (quiet_stderr)
    {
      int devnull= open("/dev/null", O_WRONLY);
      if (devnull >= 0)
        dup2(devnull, fileno(stderr));
    }
    close(fds[0]);
    close(fds[1]);
    execvp(cmd_argv[0], cmd_argv);
    fprintf(stderr, "ERROR: cannot run '%s': %s.\n", cmd_argv[0],
            strerror(errno));
    _exit(127);
  }
  close(fds[1]);
  while ((n= read(fds[0], buf, sizeof(buf))))
  {
    if (n < 0)
    {
      if (errno == EINTR)
        continue;
      break;
    }
    /* keep reading after a failed append, so the child can still finish */
    if (!oom)
      oom= dynstr_append_mem(out, buf, (size_t) n);
  }
  close(fds[0]);
  while (waitpid(pid, &status, 0) < 0)
  {
    if (errno != EINTR)
    {
      fprintf(stderr, "ERROR: cannot wait for '%s': %s.\n", cmd_argv[0],
              strerror(errno));
      return 127;
    }
  }
  if (oom)
  {
    fprintf(stderr, "ERROR: out of memory reading the output of '%s'.\n",
            cmd_argv[0]);
    return 127;
  }
  if (n < 0)
  {
    fprintf(stderr, "ERROR: cannot read the output of '%s'.\n", cmd_argv[0]);
    return 127;
  }
  if (WIFSIGNALED(status))
    return 127;
  return WEXITSTATUS(status);
}


#define PLUGIN_PREFIX "mariadb-plugin-"
#define PLUGIN_PREFIX_LEN (sizeof(PLUGIN_PREFIX) - 1)
#define PACKAGE_NAME_SIZE (PLUGIN_PREFIX_LEN + NAME_CHAR_LEN + 1)


/**
  Build the distribution-independent package name: mariadb-plugin-
  followed by the plugin name, which is already validated and lowercased.

  @param[out]  to    Buffer for the package name.
  @param[in]   size  Size of the buffer.
  @param[in]   name  The normalized plugin name.
*/

static void build_package_name(char *to, size_t size, const char *name)
{
  safe_strcpy(to, size, PLUGIN_PREFIX);
  safe_strcat(to, size, name);
}


/*
  Search: RPM queries mariadb-plugin-* capabilities; DEB queries package
  names with that prefix. Both determine installed state through a
  different query. The results are normalized into plugin_list and
  printed in one format, so the user sees plugin names as install expects
  them, never the distribution's own package names.
*/

struct plugin_entry
{
  char name[NAME_CHAR_LEN + 1];  /* uniform name, prefix stripped */
  char package[NAME_CHAR_LEN + 1];  /* real package name, rpm only */
  char description[160];
  int installed;
};

static DYNAMIC_ARRAY plugin_list;
static DYNAMIC_STRING search_output;

#define PLUGIN_AT(i) (dynamic_element(&plugin_list, (i), struct plugin_entry *))


static struct plugin_entry *find_plugin_entry(const char *name)
{
  size_t i;

  for (i= 0; i < plugin_list.elements; i++)
    if (strcmp(PLUGIN_AT(i)->name, name) == 0)
      return PLUGIN_AT(i);
  return NULL;
}


/**
  Add a plugin to the result list, or return the existing entry with the
  same uniform name. The prefix is stripped from the stored name.

  The list reallocates, so the entry is only valid until the next one.

  @param[in]  package_name  The uniform package name, mariadb-plugin-x.

  @retval struct plugin_entry*  the entry, NULL when out of memory
*/

static struct plugin_entry *add_plugin_entry(const char *package_name)
{
  struct plugin_entry e, *found;
  const char *name= package_name + PLUGIN_PREFIX_LEN;

  if ((found= find_plugin_entry(name)))
    return found;
  bzero(&e, sizeof(e));
  safe_strcpy(e.name, sizeof(e.name), name);
  if (insert_dynamic(&plugin_list, &e))
    return NULL;
  return PLUGIN_AT(plugin_list.elements - 1);
}


static int cmp_plugin_entries(const void *a, const void *b)
{
  return strcmp(((const struct plugin_entry *) a)->name,
                ((const struct plugin_entry *) b)->name);
}


/**
  Print the collected plugins that match the search term.

  @param[in]  term  Substring to match against plugin names, "" for all.

  @retval int  no matches = 1, matches printed = 0
*/

static int print_search_results(const char *term)
{
  size_t i, width= 0, matches= 0;

  sort_dynamic(&plugin_list, cmp_plugin_entries);
  for (i= 0; i < plugin_list.elements; i++)
  {
    if (*term && !strstr(PLUGIN_AT(i)->name, term))
      continue;
    matches++;
    if (strlen(PLUGIN_AT(i)->name) > width)
      width= strlen(PLUGIN_AT(i)->name);
  }
  if (!matches)
  {
    if (*term)
      printf("No plugins matching '%s' found.\n", term);
    else
      printf("No plugins found.\n");
    return 1;
  }
  for (i= 0; i < plugin_list.elements; i++)
  {
    struct plugin_entry *e= PLUGIN_AT(i);

    if (*term && !strstr(e->name, term))
      continue;
    printf("%-*s  %-9s  %s\n", (int) width, e->name,
           e->installed ? "installed" : "available", e->description);
  }
  return 0;
}


#ifndef INSTALL_LAYOUT_DEB

static struct plugin_entry *find_plugin_by_package(const char *package)
{
  size_t i;

  for (i= 0; i < plugin_list.elements; i++)
    if (strcmp(PLUGIN_AT(i)->package, package) == 0)
      return PLUGIN_AT(i);
  return NULL;
}


/**
  Parse dnf repoquery output in the format
    @@@<package>|<summary>
    <one provided capability per line>
  into the result list. The uniform name is one of the capabilities, so
  no name mapping is needed in the tool.

  @param[in]  output     The captured repoquery output, modified in place.
  @param[in]  installed  Mark the found plugins as installed.
*/

static int parse_dnf_records(char *output, int installed)
{
  struct plugin_entry *e= NULL;
  char *line, *next, *sep;
  char package[NAME_CHAR_LEN + 1], summary[160];

  package[0]= summary[0]= '\0';
  for (line= output; line && *line; line= next)
  {
    if ((next= strchr(line, '\n')))
      *next++= '\0';
    if (strncmp(line, "@@@", 3) == 0)
    {
      line+= 3;
      if ((sep= strchr(line, '|')))
        *sep++= '\0';
      safe_strcpy(package, sizeof(package), line);
      safe_strcpy(summary, sizeof(summary), sep ? sep : "");
      continue;
    }
    /* a capability line; the version part after the name is irrelevant */
    if ((sep= strchr(line, ' ')))
      *sep= '\0';
    if (strncmp(line, PLUGIN_PREFIX, PLUGIN_PREFIX_LEN) != 0 ||
        !package[0])
      continue;
    if (!(e= add_plugin_entry(line)))
      return 1;
    safe_strcpy(e->package, sizeof(e->package), package);
    if (!e->description[0])
      safe_strcpy(e->description, sizeof(e->description), summary);
    if (installed)
      e->installed= 1;
  }
  return 0;
}


static int search_dnf(void)
{
  char *repo_argv[]= {
    (char *) "dnf", (char *) "-q", (char *) "repoquery",
    (char *) "--whatprovides", (char *) PLUGIN_PREFIX "*",
    (char *) "--qf", (char *) "@@@%{name}|%{summary}\\n%{provides}\\n", 0 };
  char *inst_argv[]= {
    (char *) "dnf", (char *) "-q", (char *) "repoquery",
    (char *) "--installed", (char *) "--whatprovides",
    (char *) PLUGIN_PREFIX "*",
    (char *) "--qf", (char *) "@@@%{name}|%{summary}\\n%{provides}\\n", 0 };
  int error;

  if ((error= run_argv_capture(repo_argv, &search_output, 0)))
    return error;
  if (parse_dnf_records(search_output.str, 0))
    return 1;

  /* same query against the installed packages only, for the status */
  if ((error= run_argv_capture(inst_argv, &search_output, 0)))
    return error;
  return parse_dnf_records(search_output.str, 1);
}


/**
  Parse "zypper --xmlout search --provides" solvable lines, e.g.
  <solvable status="not-installed" name="X" summary="Y" kind="package"/>.
  zypper never reports which capability matched, so the uniform names are
  filled in afterwards by search_zypper_names().

  @param[in]  output  The captured zypper output, modified in place.
*/

static int parse_zypper_solvables(char *output)
{
  struct plugin_entry e;
  char *line, *next, *val, *end;

  for (line= output; line && *line; line= next)
  {
    if ((next= strchr(line, '\n')))
      *next++= '\0';
    if (!strstr(line, "<solvable ") ||
        !(val= strstr(line, " name=\"")))
      continue;
    /* keyed by the real package name until the uniform name is known */
    bzero(&e, sizeof(e));
    val+= 7;
    if ((end= strchr(val, '"')))
      *end= '\0';
    safe_strcpy(e.package, sizeof(e.package), val);
    if (end)
      *end= '"';
    e.installed= (val= strstr(line, " status=\"")) &&
                 strncmp(val + 9, "installed", 9) == 0;
    if ((val= strstr(line, " summary=\"")))
    {
      val+= 10;
      if ((end= strchr(val, '"')))
        *end= '\0';
      safe_strcpy(e.description, sizeof(e.description), val);
    }
    if (insert_dynamic(&plugin_list, &e))
      return 1;
  }
  return 0;
}


/**
  Fill in the uniform names with one "zypper info --provides" call for
  all found packages. Output has "Name : X" headers followed by indented
  capability lines. Packages that end up without a uniform name are
  dropped from the list.
*/

static int search_zypper_names(void)
{
  struct plugin_entry *e= NULL;
  char **cmd_argv;
  char *line, *next, *cap, *sep;
  size_t i, n= 0;
  int error;

  if (!(cmd_argv= (char **) my_malloc(PSI_NOT_INSTRUMENTED,
                                      (plugin_list.elements + 6) *
                                      sizeof(char *), MYF(MY_WME))))
    return 1;
  cmd_argv[n++]= (char *) "zypper";
  cmd_argv[n++]= (char *) "-n";
  cmd_argv[n++]= (char *) "-q";
  cmd_argv[n++]= (char *) "info";
  cmd_argv[n++]= (char *) "--provides";
  for (i= 0; i < plugin_list.elements; i++)
    cmd_argv[n++]= PLUGIN_AT(i)->package;
  cmd_argv[n]= 0;
  error= run_argv_capture(cmd_argv, &search_output, 0);
  my_free(cmd_argv);
  if (error)
    return 1;

  /* nothing is added below, so the entry a header selects stays valid */
  for (line= search_output.str; line && *line; line= next)
  {
    if ((next= strchr(line, '\n')))
      *next++= '\0';
    if (strncmp(line, "Name", 4) == 0 && (sep= strchr(line, ':')))
    {
      for (sep++; *sep == ' '; sep++) ;
      e= find_plugin_by_package(sep);
      continue;
    }
    for (cap= line; *cap == ' '; cap++) ;
    if (cap == line || !e ||
        strncmp(cap, PLUGIN_PREFIX, PLUGIN_PREFIX_LEN) != 0)
      continue;
    if ((sep= strchr(cap, ' ')))
      *sep= '\0';
    safe_strcpy(e->name, sizeof(e->name), cap + PLUGIN_PREFIX_LEN);
  }

  /* drop packages whose uniform name never showed up */
  for (i= 0; i < plugin_list.elements; )
  {
    if (PLUGIN_AT(i)->name[0])
      i++;
    else
      delete_dynamic_element(&plugin_list, i);
  }
  return 0;
}


static int search_zypper(void)
{
  char *cmd_argv[7];
  int error;

  cmd_argv[0]= (char *) "zypper";
  cmd_argv[1]= (char *) "-n";
  cmd_argv[2]= (char *) "--xmlout";
  cmd_argv[3]= (char *) "search";
  cmd_argv[4]= (char *) "--provides";
  cmd_argv[5]= (char *) PLUGIN_PREFIX "*";
  cmd_argv[6]= 0;
  /* zypper exits with 104 when nothing matches: an answer, not an error */
  error= run_argv_capture(cmd_argv, &search_output, 0);
  if (error && error != 104)
    return error;
  if (parse_zypper_solvables(search_output.str))
    return 1;
  if (plugin_list.elements && search_zypper_names())
    return 1;
  return 0;
}

#else /* INSTALL_LAYOUT_DEB */

/**
  Parse "apt-cache search" output, "<package> - <description>" per line,
  into the result list. deb package names are already the uniform names.

  @param[in]  output  The captured apt-cache output, modified in place.
*/

static int parse_apt_records(char *output)
{
  struct plugin_entry *e;
  char *line, *next, *sep;

  for (line= output; line && *line; line= next)
  {
    if ((next= strchr(line, '\n')))
      *next++= '\0';
    if ((sep= strstr(line, " - ")))
      *sep= '\0';
    if (strncmp(line, PLUGIN_PREFIX, PLUGIN_PREFIX_LEN) != 0)
      continue;
    if (!(e= add_plugin_entry(line)))
      return 1;
    if (sep && !e->description[0])
      safe_strcpy(e->description, sizeof(e->description), sep + 3);
  }
  return 0;
}


static int search_apt(void)
{
  char *cmd_argv[6];
  char *line, *next, *sep;
  struct plugin_entry *e;
  int error;

  cmd_argv[0]= (char *) "apt-cache";
  cmd_argv[1]= (char *) "search";
  cmd_argv[2]= (char *) "--names-only";
  cmd_argv[3]= (char *) "^" PLUGIN_PREFIX;
  cmd_argv[4]= 0;
  if ((error= run_argv_capture(cmd_argv, &search_output, 0)))
    return error;
  if (parse_apt_records(search_output.str))
    return 1;

  /*
    dpkg-query exits 1 with no output when the pattern matches no packages.
    Execution/capture failures and dpkg operational errors must propagate.
  */
  cmd_argv[0]= (char *) "dpkg-query";
  cmd_argv[1]= (char *) "-W";
  cmd_argv[2]= (char *) "-f=${Package} ${db:Status-Status}\n";
  cmd_argv[3]= (char *) PLUGIN_PREFIX "*";
  cmd_argv[4]= 0;
  error= run_argv_capture(cmd_argv, &search_output, 1);
  if (error)
    return error == 1 && !search_output.length ? 0 : error;
  for (line= search_output.str; line && *line; line= next)
  {
    if ((next= strchr(line, '\n')))
      *next++= '\0';
    if (!(sep= strchr(line, ' ')))
      continue;
    *sep++= '\0';
    if (strcmp(sep, "installed") == 0 &&
        strncmp(line, PLUGIN_PREFIX, PLUGIN_PREFIX_LEN) == 0)
    {
      if (!(e= add_plugin_entry(line)))
        return 1;
      e->installed= 1;
    }
  }
  return 0;
}

#endif /* INSTALL_LAYOUT_DEB */

#endif /* PKG_DELEGATION */


/**
  Search for plugins.

  RPM searches provided capabilities; DEB searches package names with the
  mariadb-plugin- prefix. Results are shown as plugin names, not the
  distribution's own package names. Needs no root.

  @param[in]  term     Substring to match, empty to list all plugins.
  @retval int error or no matches = 1, matches printed = 0
*/

static int do_search(const char *term)
{
#ifdef PKG_DELEGATION
  int error;

  if (my_init_dynamic_array(PSI_NOT_INSTRUMENTED, &plugin_list,
                            sizeof(struct plugin_entry), 32, 32, MYF(MY_WME)))
    return 1;
  if (init_dynamic_string(&search_output, "", 16 * 1024, 16 * 1024))
  {
    delete_dynamic(&plugin_list);
    return 1;
  }
#ifdef INSTALL_LAYOUT_DEB
  error= search_apt();
#else
  {
    const char *pm= get_package_manager();
    error= pm ? (strcmp(pm, "dnf") == 0 ? search_dnf() : search_zypper()) : 1;
  }
#endif
  if (!error)
    error= print_search_results(term);
  else
    fprintf(stderr, "ERROR: could not determine plugin availability or "
            "installed state.\n");
  dynstr_free(&search_output);
  delete_dynamic(&plugin_list);
  return error ? 1 : 0;
#else
  printf("search: not available for %s installations yet, the plugin "
         "index does not exist\n", INSTALL_METHOD_NAME);
  return 1;
#endif
}


#ifndef PKG_DELEGATION

/*
  On tarball installations nothing tracks what a plugin put on disk, so
  install writes a manifest and uninstall acts strictly on it: the header
  lines describe the plugin, each "file:" or "dir:" line is one path,
  relative to the basedir, that install created and uninstall removes.
*/

#define MANIFEST_SUBDIR ".mariadb-plugin"
#define KV_LINE_SIZE 1024
#define PLUGIN_BASE_URL ""
#define PLUGIN_INDEX "plugins.index"

struct manifest_entry
{
  char path[FN_REFLEN];
  my_bool is_dir;
};

enum plugin_file_operation {PLUGIN_OPEN, PLUGIN_DELETE, PLUGIN_MKDIR,
                            PLUGIN_RMDIR, PLUGIN_CHECK_DIR};

static int valid_relative_path(const char *path);


static int build_full_path(char *to, size_t size, const char *basedir,
                           const char *rel)
{
  if (safe_strcpy_truncated(to, size, basedir) ||
      safe_strcat(to, size, "/") || safe_strcat(to, size, rel))
  {
    fprintf(stderr, "ERROR: path is too long: '%s/%s'.\n", basedir, rel);
    return 1;
  }
  return 0;
}


/* Resolve parents without following links. The basedir itself is trusted. */
static int plugin_file_op(const char *basedir, const char *rel,
                          enum plugin_file_operation op, int flags)
{
  char full[FN_REFLEN];
  int result= -1, saved_errno;
#ifdef _WIN32
  HANDLE parents[FN_REFLEN / 2 + 1];
  size_t count= 0;
  char *p;
#else
  int parent= -1, next;
  const char *part= rel, *slash;
  char component[FN_REFLEN];
  size_t len;
#endif

  if (!valid_relative_path(rel) ||
      build_full_path(full, sizeof(full), basedir, rel))
  {
    errno= my_errno= EINVAL;
    return -1;
  }
#ifdef _WIN32
  /* Deny writes and renames to parent directories during the operation. */
  for (p= full + strlen(basedir); ; p++)
  {
    char end= *p;
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE handle;
    if (end != '/' && end != '\0')
      continue;
    if (!end && op != PLUGIN_CHECK_DIR &&
        (op != PLUGIN_OPEN || (flags & O_CREAT)))
      break;
    *p= '\0';
    handle= CreateFile(full, FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                       NULL, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                       NULL);
    *p= end;
    if (handle == INVALID_HANDLE_VALUE)
    {
      my_osmaperr(GetLastError());
      goto end;
    }
    if (count == array_elements(parents))
    {
      CloseHandle(handle);
      errno= ENAMETOOLONG;
      goto end;
    }
    parents[count++]= handle;
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        ((end || op == PLUGIN_CHECK_DIR) &&
         !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)))
    {
      errno= EACCES;
      goto end;
    }
    if (!end)
      break;
  }
  switch (op) {
  case PLUGIN_OPEN: result= my_open(full, flags, MYF(0)); break;
  case PLUGIN_DELETE: result= my_delete(full, MYF(0)); break;
  case PLUGIN_MKDIR: result= my_mkdir(full, 0755, MYF(0)); break;
  case PLUGIN_RMDIR: result= rmdir(full); break;
  case PLUGIN_CHECK_DIR: result= 0; break;
  }
#else
  parent= open(basedir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (parent < 0)
    goto end;
  while ((slash= strchr(part, '/')))
  {
    len= (size_t) (slash - part);
    if (len)
    {
      memcpy(component, part, len);
      component[len]= '\0';
      next= openat(parent, component,
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0)
        goto end;
      close(parent);
      parent= next;
    }
    part= slash + 1;
  }
  switch (op) {
  case PLUGIN_OPEN:
    result= openat(parent, part, flags | O_NOFOLLOW | O_CLOEXEC, my_umask);
    if (result >= 0)
      result= my_register_filename(result, full, FILE_BY_OPEN, 0, MYF(0));
    break;
  case PLUGIN_DELETE: result= unlinkat(parent, part, 0); break;
  case PLUGIN_MKDIR: result= mkdirat(parent, part, 0755); break;
  case PLUGIN_RMDIR: result= unlinkat(parent, part, AT_REMOVEDIR); break;
  case PLUGIN_CHECK_DIR:
    next= openat(parent, part,
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next >= 0)
    {
      close(next);
      result= 0;
    }
    break;
  }
#endif
end:
  saved_errno= errno;
#ifdef _WIN32
  while (count)
    CloseHandle(parents[--count]);
#else
  if (parent >= 0)
    close(parent);
#endif
  if (result < 0)
    errno= my_errno= saved_errno;
  return result;
}


static int build_manifest_path(char *to, size_t size, const char *basedir,
                               const char *name)
{
  if (build_full_path(to, size, basedir, MANIFEST_SUBDIR "/") ||
      safe_strcat(to, size, name) || safe_strcat(to, size, ".list"))
    return 1;
  return 0;
}


/**
  Check that a relative path stays inside the basedir.

  Used for manifest lines and archive entries alike, neither of which is
  trusted input: an absolute path or a ".." component would let install
  write, and uninstall delete, files the plugin never owned.

  @param[in]  path  The path, relative to the basedir.

  @retval int acceptable = 1, not = 0
*/

static int valid_relative_path(const char *path)
{
  const uchar *p;
  /* tar paths use '/', so a backslash only ever comes from hostile input */
  if (!*path || *path == '/' || strchr(path, '\\') || strchr(path, ':') ||
      strstr(path, ".."))
    return 0;
  /* a control character, above all a newline, would let a crafted entry
     name inject extra lines into the manifest and make uninstall act on
     files this plugin never installed */
  for (p= (const uchar *) path; *p; p++)
    if (*p < 0x20 || *p == 0x7f)
      return 0;
  return 1;
}


/* 1 = line, 0 = EOF, -1 = error. Long manifest headers remain readable. */
static int read_kv_line(FILE *file, const char *source, char *line,
                        my_bool manifest)
{
  size_t len= 0;
  int c, oversized= 0, invalid= 0;

  while ((c= fgetc(file)) != EOF && c != '\n')
  {
    if (c == '\0')
      invalid= 1;
    if (len < KV_LINE_SIZE - 1)
      line[len++]= (char) c;
    else
      oversized= 1;
  }
  line[len]= '\0';
  if (ferror(file) || invalid)
    goto corrupt;
  if (oversized)
  {
    if (!manifest || !strncmp(line, "dir: ", 5) ||
        !strncmp(line, "file: ", 6))
      goto corrupt;
    line[0]= '\0';
    return 1;
  }
  if (c == EOF && !len)
    return 0;
  if (len && line[len - 1] == '\r')
    line[--len]= '\0';
  return 1;

corrupt:
  fprintf(stderr, "ERROR: '%s' contains an invalid or oversized line, "
          "or could not be read.\n", source);
  return -1;
}


struct download_target
{
  FILE *file;
  size_t remaining;
};


static size_t download_write(char *data, size_t size, size_t count, void *arg)
{
  struct download_target *target= (struct download_target *) arg;
  size_t bytes;
  if (size && count > target->remaining / size)
    return 0;
  bytes= size * count;
  target->remaining-= bytes;
  return fwrite(data, 1, bytes, target->file);
}


static FILE *plugin_tmpfile(void)
{
  char name[FN_REFLEN];
  File fd= create_temp_file(name, NULL, "plugin", O_BINARY,
                            MYF(MY_WME | MY_TEMPORARY));
  FILE *file;
  if (fd < 0)
    return NULL;
  if (!(file= my_fdopen(fd, name, O_RDWR | O_BINARY, MYF(MY_WME))))
    my_close(fd, MYF(0));
  return file;
}


/* Verify and extract a private snapshot, not a replaceable local pathname. */
static FILE *copy_local_archive(const char *name)
{
  File fd;
  MY_STAT info;
  FILE *input, *output;
  uchar buf[8192];
  size_t n;
  int flags= O_RDONLY | O_BINARY, error= 0;
#ifndef _WIN32
  flags|= O_NONBLOCK;
#endif
  if ((fd= my_open(name, flags, MYF(MY_WME))) < 0)
    return NULL;
  if (my_fstat(fd, &info, MYF(MY_WME)) || !MY_S_ISREG(info.st_mode))
  {
    fprintf(stderr, "ERROR: '%s' is not a readable regular archive file.\n",
            name);
    my_close(fd, MYF(0));
    return NULL;
  }
  if (!(input= my_fdopen(fd, name, O_RDONLY | O_BINARY, MYF(MY_WME))))
  {
    my_close(fd, MYF(0));
    return NULL;
  }
  output= plugin_tmpfile();
  if (output)
  {
    while ((n= fread(buf, 1, sizeof(buf), input)) > 0)
      if (fwrite(buf, 1, n, output) != n)
      {
        error= 1;
        break;
      }
    if (error || ferror(input) || fflush(output) || fseek(output, 0, SEEK_SET))
    {
      fprintf(stderr, "ERROR: cannot copy archive '%s': %s.\n", name,
              strerror(errno));
      my_fclose(output, MYF(0));
      output= NULL;
    }
  }
  my_fclose(input, MYF(0));
  return output;
}


/* Keep downloads open and anonymous, including between verification passes. */
static FILE *download_file(const char *url, size_t limit)
{
  char detail[CURL_ERROR_SIZE]= "";
  struct download_target target;
  CURL *curl;
  CURLcode rc;
  long status= 0;

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
  {
    fprintf(stderr, "ERROR: cannot initialize libcurl.\n");
    return NULL;
  }
  if (!(curl= curl_easy_init()))
  {
    curl_global_cleanup();
    return NULL;
  }
  target.file= plugin_tmpfile();
  target.remaining= limit;
  if (!target.file)
    goto end;

#define DOWNLOAD_OPTION(option, value) \
  if ((rc= curl_easy_setopt(curl, option, value)) != CURLE_OK) goto failed

  DOWNLOAD_OPTION(CURLOPT_URL, url);
  DOWNLOAD_OPTION(CURLOPT_ERRORBUFFER, detail);
  DOWNLOAD_OPTION(CURLOPT_WRITEFUNCTION, download_write);
  DOWNLOAD_OPTION(CURLOPT_WRITEDATA, &target);
  DOWNLOAD_OPTION(CURLOPT_FAILONERROR, 1L);
  DOWNLOAD_OPTION(CURLOPT_FOLLOWLOCATION, 1L);
  DOWNLOAD_OPTION(CURLOPT_MAXREDIRS, 5L);
  DOWNLOAD_OPTION(CURLOPT_CONNECTTIMEOUT, 10L);
  DOWNLOAD_OPTION(CURLOPT_TIMEOUT, 300L);
  DOWNLOAD_OPTION(CURLOPT_LOW_SPEED_LIMIT, 1L);
  DOWNLOAD_OPTION(CURLOPT_LOW_SPEED_TIME, 30L);
  DOWNLOAD_OPTION(CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
  DOWNLOAD_OPTION(CURLOPT_PROTOCOLS_STR, "http,https");
  DOWNLOAD_OPTION(CURLOPT_REDIR_PROTOCOLS_STR,
                  strncmp(url, "https://", 8) ? "http,https" : "https");
#else
  DOWNLOAD_OPTION(CURLOPT_PROTOCOLS, (long) (CURLPROTO_HTTP | CURLPROTO_HTTPS));
  DOWNLOAD_OPTION(CURLOPT_REDIR_PROTOCOLS, (long) (strncmp(url, "https://", 8) ?
                  CURLPROTO_HTTP | CURLPROTO_HTTPS : CURLPROTO_HTTPS));
#endif
#undef DOWNLOAD_OPTION

  if ((rc= curl_easy_perform(curl)) != CURLE_OK ||
      (rc= curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status)) != CURLE_OK)
    goto failed;
  if (status != 200 || fflush(target.file) ||
      fseek(target.file, 0, SEEK_SET))
  {
    fprintf(stderr, "ERROR: download of '%s' failed (HTTP %ld or file I/O).\n",
            url, status);
    goto discard;
  }
  goto end;

failed:
  fprintf(stderr, "ERROR: cannot download '%s': %s.\n", url,
          detail[0] ? detail : curl_easy_strerror(rc));
discard:
  my_fclose(target.file, MYF(0));
  target.file= NULL;
end:
  curl_easy_cleanup(curl);
  curl_global_cleanup();
  return target.file;
}


static int build_download_url(char *url, size_t size, const char *file)
{
  const char *base= opt_base_url ? opt_base_url : PLUGIN_BASE_URL;
  const char *host, *p;
  size_t len= strlen(base);

  if (!len)
  {
    fprintf(stderr, "ERROR: no plugin repository configured; use "
            "--base-url=<URL> or --file=<tarball>.\n");
    return 1;
  }
  if (!strncmp(base, "https://", 8))
    host= base + 8;
  else if (!strncmp(base, "http://", 7))
    host= base + 7;
  else
    goto invalid;
  if (!*host || *host == '/' || strpbrk(host, "\\?#@"))
    goto invalid;
  for (p= base; *p; p++)
    if ((uchar) *p <= 0x20 || *p == 0x7f)
      goto invalid;
  if (safe_strcpy_truncated(url, size, base) ||
      (base[len - 1] != '/' && safe_strcat(url, size, "/")) ||
      safe_strcat(url, size, file))
  {
    fprintf(stderr, "ERROR: plugin repository URL is too long.\n");
    return 1;
  }
  return 0;

invalid:
  fprintf(stderr, "ERROR: --base-url must be an HTTP or HTTPS directory "
          "URL without credentials, a query or a fragment.\n");
  return 1;
}


struct index_entry
{
  char name[NAME_CHAR_LEN + 1];
  char version[64];
  char server[32];
  char platform[64];
  char arch[64];
  char file[FN_REFLEN];
  char sha256[65];
};


/* Records are separated by blank lines; this is not a general YAML parser. */
static int read_index(FILE *file, const char *source, const char *name,
                       struct index_entry *result)
{
  struct index_entry e;
  char line[KV_LINE_SIZE], server[32];
  const char *keys[]= {"name", "version", "server", "platform", "arch",
                       "file", "sha256"};
  char *values[]= {e.name, e.version, e.server, e.platform, e.arch,
                    e.file, e.sha256};
  size_t sizes[]= {sizeof(e.name), sizeof(e.version), sizeof(e.server),
                    sizeof(e.platform), sizeof(e.arch), sizeof(e.file),
                    sizeof(e.sha256)};
  uint seen= 0;
  int rc, found= 0;
  size_t i, len;

  my_snprintf(server, sizeof(server), "%u.%u", MYSQL_VERSION_ID / 10000,
              MYSQL_VERSION_ID / 100 % 100);
  bzero(&e, sizeof(e));
  for (;;)
  {
    rc= read_kv_line(file, source, line, FALSE);
    if (rc < 0)
      return 1;
    if (rc && line[0])
    {
      char *value= strchr(line, ':');
      if (!value || value == line || value[1] != ' ')
        goto invalid;
      *value++= '\0';
      while (*value == ' ')
        value++;
      len= strlen(value);
      while (len && value[len - 1] == ' ')
        value[--len]= '\0';
      for (i= 0; value[i]; i++)
        if ((uchar) value[i] < 0x20 || value[i] == 0x7f)
          goto invalid;
      for (i= 0; i < array_elements(keys); i++)
        if (!strcmp(line, keys[i]))
          break;
      if (i < array_elements(keys))
      {
        if (!len || (seen & (1U << i)) ||
            safe_strcpy_truncated(values[i], sizes[i], value))
          goto invalid;
        seen|= 1U << i;
      }
      else
        seen|= 128; /* Optional descriptive metadata. */
      continue;
    }
    if (seen)
    {
      if ((seen & 127) != 127 || validate_plugin_name(e.name))
        goto invalid;
      len= strlen(e.file);
      if (len < 7 || strcmp(e.file + len - 7, ".tar.gz") ||
          !valid_relative_path(e.file))
        goto invalid;
      for (i= 0; i < len; i++)
        if (!isalnum((uchar) e.file[i]) && e.file[i] != '.' &&
            e.file[i] != '_' && e.file[i] != '-')
          goto invalid;
      if (strlen(e.sha256) != 64)
        goto invalid;
      for (i= 0; i < 64; i++)
        if (!isxdigit((uchar) e.sha256[i]))
          goto invalid;
      if (!strcmp(e.name, name) && !strcmp(e.server, server) &&
          !strcmp(e.platform, SYSTEM_TYPE) && !strcmp(e.arch, MACHINE_TYPE))
      {
        if (found)
        {
          fprintf(stderr, "ERROR: multiple compatible entries for '%s' "
                  "in '%s'.\n", name, source);
          return 1;
        }
        *result= e;
        found= 1;
      }
      bzero(&e, sizeof(e));
      seen= 0;
    }
    if (!rc)
      break;
  }
  if (!found)
    fprintf(stderr, "ERROR: no compatible download for '%s' "
            "(server %s, %s, %s).\n", name, server, SYSTEM_TYPE, MACHINE_TYPE);
  return !found;

invalid:
  fprintf(stderr, "ERROR: invalid plugin record in '%s'.\n", source);
  return 1;
}


/**
  Read and validate all manifest entries before any deletion.

  @param[in]   manifest  Path of the manifest file.
  @param[out]  entries   Initialized array, filled with manifest_entry.

  @retval int error = 1, success = 0
*/
static int read_manifest(const char *basedir, const char *manifest,
                          DYNAMIC_ARRAY *entries)
{
  FILE *file;
  File fd;
  char line[KV_LINE_SIZE];
  struct manifest_entry e;
  const char *path;
  int rc= 0, error= 0;

  fd= plugin_file_op(basedir, manifest + strlen(basedir) + 1,
                     PLUGIN_OPEN, O_RDONLY | O_BINARY);
  if (fd < 0)
  {
    fprintf(stderr, "ERROR: cannot read '%s': %s.\n", manifest,
            strerror(errno));
    return 1;
  }
  if (!(file= my_fdopen(fd, manifest, O_RDONLY | O_BINARY, MYF(MY_WME))))
  {
    my_close(fd, MYF(0));
    return 1;
  }
  while (!error && (rc= read_kv_line(file, manifest, line, TRUE)) > 0)
  {
    e.is_dir= strncmp(line, "dir: ", 5) == 0;
    if (e.is_dir)
      path= line + 5;
    else if (strncmp(line, "file: ", 6) == 0)
      path= line + 6;
    else
      continue;  /* header lines; uninstall only consumes the paths */

    if (!valid_relative_path(path) || strlen(path) >= sizeof(e.path))
    {
      fprintf(stderr, "ERROR: unsafe path '%s' in '%s', nothing was "
              "removed.\n", path, manifest);
      error= 1;
      break;
    }
    safe_strcpy(e.path, sizeof(e.path), path);
    error= insert_dynamic(entries, &e);
  }
  my_fclose(file, MYF(0));
  return error || rc < 0;
}


/**
  Delete everything a manifest lists, then the manifest.

  Remove files before directories. Keep the manifest if file removal fails
  so the user can retry; nonempty directories are reported but retained.

  @param[in]  basedir   The base directory.
  @param[in]  manifest  Path of the manifest file.

  @retval int error = 1, success = 0
*/

static int manifest_remove(const char *basedir, const char *manifest)
{
  char full[FN_REFLEN];
  DYNAMIC_ARRAY entries;
  struct manifest_entry *e;
  size_t i;
  int failed= 0;

  if (my_init_dynamic_array(PSI_NOT_INSTRUMENTED, &entries,
                            sizeof(struct manifest_entry), 16, 16,
                            MYF(MY_WME)))
    return 1;
  if (read_manifest(basedir, manifest, &entries))
  {
    delete_dynamic(&entries);
    return 1;
  }

  for (i= 0; i < entries.elements; i++)
  {
    e= dynamic_element(&entries, i, struct manifest_entry *);
    if (e->is_dir)
      continue;
    if (build_full_path(full, sizeof(full), basedir, e->path))
    {
      /* an undeletable file must keep the manifest, or it is orphaned */
      failed= 1;
      continue;
    }
    if (opt_dry_run)
      printf("would delete %s\n", full);
    else if (plugin_file_op(basedir, e->path, PLUGIN_DELETE, 0))
    {
      /* a file someone already removed by hand must not block uninstall */
      if (my_errno == ENOENT)
        fprintf(stderr, "WARNING: '%s' was already gone.\n", full);
      else
      {
        fprintf(stderr, "ERROR: cannot delete '%s': %s.\n", full,
                strerror(my_errno));
        failed= 1;
      }
    }
  }

  /* directories in reverse manifest order, so children come before parents */
  for (i= entries.elements; i-- > 0; )
  {
    e= dynamic_element(&entries, i, struct manifest_entry *);
    if (!e->is_dir || build_full_path(full, sizeof(full), basedir, e->path))
      continue;
    if (opt_dry_run)
      printf("would remove directory %s\n", full);
    else if (plugin_file_op(basedir, e->path, PLUGIN_RMDIR, 0) &&
             errno != ENOENT)
      fprintf(stderr, "WARNING: directory '%s' was not removed: %s.\n", full,
              strerror(errno));
  }
  delete_dynamic(&entries);

  if (failed)
  {
    fprintf(stderr, "ERROR: not all files could be deleted; the manifest "
            "was kept, so uninstall can be run again.\n");
    return 1;
  }
  if (opt_dry_run)
  {
    printf("would delete %s\n", manifest);
    return 0;
  }
  if (plugin_file_op(basedir, manifest + strlen(basedir) + 1,
                     PLUGIN_DELETE, 0))
    return 1;
  /* the manifest directory goes with the last plugin; busy is fine */
  plugin_file_op(basedir, MANIFEST_SUBDIR, PLUGIN_RMDIR, 0);
  return 0;
}


/*
  The tool reads plugin tarballs itself instead of running tar: every entry
  is judged before anything is written, no tar binary is needed on the
  machine, and no command line is ever built from a user-chosen path.

  Supported entries are ustar headers with regular files and
  directories, plus the two ways a long path is spelled, GNU long-name
  entries and pax "path" records. Links and devices are refused.
*/

#define TAR_BLOCK 512

struct tar_reader
{
  gzFile gz;
  const char *file;
  ulonglong data_left;  /* unread bytes of the current entry, plus padding */
};

struct tar_entry
{
  char path[FN_REFLEN];
  ulonglong size;
  uint mode;
  my_bool is_dir;
};


static int tar_open(struct tar_reader *r, const char *file, FILE *contents)
{
  int fd;
  r->file= file;
  r->data_left= 0;
  r->gz= NULL;
  if (fseek(contents, 0, SEEK_SET))
    goto error;
  /* gzdopen takes a CRT descriptor, not a Windows mysys descriptor. */
#ifdef _WIN32
  fd= _dup(_fileno(contents));
#else
  fd= dup(fileno(contents));
#endif
  if (fd < 0)
    goto error;
  if (!(r->gz= gzdopen(fd, "rb")))
  {
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
    goto error;
  }
  return 0;

error:
  fprintf(stderr, "ERROR: cannot open '%s': %s.\n", file, strerror(errno));
  return 1;
}


static void tar_close(struct tar_reader *r)
{
  gzclose(r->gz);
}


static int tar_read_bytes(struct tar_reader *r, void *buf, size_t len)
{
  int n= gzread(r->gz, buf, (unsigned) len);
  if (n != (int) len)
  {
    int err;
    const char *msg= gzerror(r->gz, &err);
    fprintf(stderr, "ERROR: '%s' is truncated or not a gzip file%s%s.\n",
            r->file, err == Z_ERRNO || err == Z_OK ? "" : ": ",
            err == Z_ERRNO || err == Z_OK ? "" : msg);
    return 1;
  }
  return 0;
}


static int tar_skip_data(struct tar_reader *r)
{
  if (r->data_left && gzseek(r->gz, (z_off_t) r->data_left, SEEK_CUR) < 0)
  {
    fprintf(stderr, "ERROR: '%s' is truncated.\n", r->file);
    return 1;
  }
  r->data_left= 0;
  return 0;
}


/**
  Parse a tar numeric field: octal digits, terminated by NUL or space.

  @retval int error = 1, success = 0
*/

static int tar_number(const uchar *field, size_t len, ulonglong *out)
{
  ulonglong v= 0;
  size_t i;

  /* Only octal numeric fields are supported. */
  if (field[0] & 0x80)
    return 1;
  for (i= 0; i < len && field[i] == ' '; i++) ;
  for (; i < len && field[i] != '\0' && field[i] != ' '; i++)
  {
    if (field[i] < '0' || field[i] > '7')
      return 1;
    v= v * 8 + (field[i] - '0');
  }
  for (; i < len; i++)
    if (field[i] != '\0' && field[i] != ' ')
      return 1;
  *out= v;
  return 0;
}


static int tar_checksum_ok(const uchar *block)
{
  ulonglong stored;
  unsigned sum= 0;
  size_t i;

  if (tar_number(block + 148, 8, &stored))
    return 0;
  for (i= 0; i < TAR_BLOCK; i++)
    sum+= (i >= 148 && i < 156) ? ' ' : block[i];
  return sum == stored;
}


/**
  Read the next file or directory entry.

  Long-name and pax entries are consumed here and applied to the entry that
  follows them, so callers only ever see real files and directories. The
  entry's data is left unread; call tar_skip_data() before the next entry.

  @param[in]   r  The open reader.
  @param[out]  e  The entry.

  @retval int  1 = entry returned, 0 = end of archive, -1 = error
*/

static int tar_next(struct tar_reader *r, struct tar_entry *e)
{
  uchar block[TAR_BLOCK];
  char longname[FN_REFLEN];
  ulonglong size, mode;
  size_t len;
  char type;

  longname[0]= '\0';
  for (;;)
  {
    if (tar_skip_data(r) || tar_read_bytes(r, block, TAR_BLOCK))
      return -1;
    if (block[0] == '\0')
    {
      static const uchar zero[TAR_BLOCK]= {0};
      int n, err;
      if (longname[0] || memcmp(block, zero, TAR_BLOCK) ||
          tar_read_bytes(r, block, TAR_BLOCK) || memcmp(block, zero, TAR_BLOCK))
        goto bad_end;
      /* Consume padding and the gzip trailer, including its checksum. */
      while ((n= gzread(r->gz, block, TAR_BLOCK)) > 0)
        if (memcmp(block, zero, (size_t) n))
          goto bad_end;
      gzerror(r->gz, &err);
      if (n == 0 && err == Z_OK)
        return 0;
bad_end:
      fprintf(stderr, "ERROR: '%s' has an invalid archive ending.\n", r->file);
      return -1;
    }
    if (memcmp(block + 257, "ustar", 5) != 0 || !tar_checksum_ok(block))
    {
      fprintf(stderr, "ERROR: '%s' is not a valid tar archive.\n", r->file);
      return -1;
    }
    if (tar_number(block + 124, 12, &size) || tar_number(block + 100, 8, &mode))
    {
      fprintf(stderr, "ERROR: '%s' has a corrupt entry header.\n", r->file);
      return -1;
    }
    r->data_left= (size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
    type= block[156];

    if (type == 'L' || type == 'x')
    {
      /* the data of these entries names the entry after them */
      char *buf, *p, *end;
      int invalid= 0;
      if (size >= sizeof(longname) * 4)
      {
        fprintf(stderr, "ERROR: '%s' has an entry name that is too long.\n",
                r->file);
        return -1;
      }
      if (!(buf= (char *) my_malloc(PSI_NOT_INSTRUMENTED, (size_t) size + 1,
                                    MYF(MY_WME))))
        return -1;
      if (tar_read_bytes(r, buf, (size_t) size))
      {
        my_free(buf);
        return -1;
      }
      buf[size]= '\0';
      r->data_left-= size;
      if (type == 'L')
      {
        len= (size_t) size;
        if (len && buf[len - 1] == '\0')
          len--;
        if (!len || len >= sizeof(longname) || memchr(buf, '\0', len))
          invalid= 1;
        else
        {
          memcpy(longname, buf, len);
          longname[len]= '\0';
        }
      }
      else
      {
        /* PAX lengths include the digits, separator and terminating newline. */
        for (p= buf; p < buf + size; p= end)
        {
          char *key= p;
          len= 0;
          while (*key >= '0' && *key <= '9' && len <= size)
            len= len * 10 + (uint) (*key++ - '0');
          if (key == p || *key != ' ' || len > (size_t) (buf + size - p) ||
              len <= (size_t) (key - p) + 1)
          {
            invalid= 1;
            break;
          }
          end= p + len;
          key++;
          if (end[-1] != '\n' || memchr(key, '\0', (size_t) (end - key)) ||
              !memchr(key, '=', (size_t) (end - key - 1)))
          {
            invalid= 1;
            break;
          }
          if (end - key > 5 && !memcmp(key, "path=", 5))
          {
            len= (size_t) (end - key - 6);
            if (!len || len >= sizeof(longname))
            {
              invalid= 1;
              break;
            }
            memcpy(longname, key + 5, len);
            longname[len]= '\0';
          }
        }
      }
      my_free(buf);
      if (invalid)
      {
        fprintf(stderr, "ERROR: '%s' has an invalid or oversized extended "
                "header.\n", r->file);
        return -1;
      }
      continue;
    }
    if (type == 'g')  /* pax global header, carries nothing we use */
      continue;
    break;
  }

  switch (type) {
  case '0': case '\0': case '7':
    e->is_dir= FALSE;
    break;
  case '5':
    e->is_dir= TRUE;
    break;
  case '1': case '2':
    fprintf(stderr, "ERROR: '%s' contains a link, which plugin archives "
            "must not have.\n", r->file);
    return -1;
  default:
    fprintf(stderr, "ERROR: '%s' contains an entry of unsupported type "
            "'%c'.\n", r->file, type);
    return -1;
  }

  if (longname[0])
    safe_strcpy(e->path, sizeof(e->path), longname);
  else
  {
    /* ustar splits long paths into prefix (155) and name (100) */
    e->path[0]= '\0';
    if (block[345])
    {
      safe_strcpy_truncated(e->path, MY_MIN(sizeof(e->path), 156),
                            (char *) block + 345);
      safe_strcat(e->path, sizeof(e->path), "/");
    }
    len= strlen(e->path);
    safe_strcpy_truncated(e->path + len, MY_MIN(sizeof(e->path) - len, 101),
                          (char *) block + 0);
  }
  /* "./x" and "x/" spell the same thing; normalize before judging */
  while (strncmp(e->path, "./", 2) == 0)
    memmove(e->path, e->path + 2, strlen(e->path) - 1);
  len= strlen(e->path);
  while (len > 1 && e->path[len - 1] == '/')
    e->path[--len]= '\0';

  e->size= size;
  /* setuid and setgid bits from an archive are never honored */
  e->mode= (uint) mode & 0777;
  return 1;
}



/**
  Compute the SHA-256 of a file as 64 lower case hex digits.

  @retval int error = 1, success = 0
*/

static int file_sha256(const char *file, FILE *input, char *hex)
{
  uchar buf[8192], digest[32];
  void *ctx;
  size_t n, i;
  int error= 0;

  if (!(ctx= my_malloc(PSI_NOT_INSTRUMENTED, my_sha256_context_size(),
                       MYF(MY_WME))))
    return 1;
  my_sha256_init(ctx);
  while ((n= fread(buf, 1, sizeof(buf), input)) > 0)
    my_sha256_input(ctx, buf, n);
  if (ferror(input))
  {
    fprintf(stderr, "ERROR: cannot read '%s': %s.\n", file, strerror(errno));
    error= 1;
  }
  my_sha256_result(ctx, digest);
  my_free(ctx);
  for (i= 0; i < sizeof(digest); i++)
    sprintf(hex + 2 * i, "%02x", digest[i]);
  return error;
}


/**
  Compare the archive against the checksum the user was given for it.

  Case does not matter; whitespace and the "sha256:" prefix some sites
  print are tolerated. A mismatch means the file is not the one that was
  published, whatever the reason, so nothing is installed from it.

  @retval int error = 1, success = 0
*/

static int verify_sha256(const char *file, FILE *contents,
                         const char *expected, char *hex)
{
  const char *p= expected;
  size_t i;

  if (file_sha256(file, contents, hex))
    return 1;
  while (*p == ' ' || *p == '\t') p++;
  if (strncasecmp(p, "sha256:", 7) == 0)
    p+= 7;
  for (i= 0; i < 64 && p[i]; i++)
    if (tolower((uchar) p[i]) != hex[i])
      break;
  if (i != 64 || (p[64] && !isspace((uchar) p[64])))
  {
    fprintf(stderr, "ERROR: '%s' does not match the expected checksum.\n"
            "  expected: %s\n  actual:   %s\n", file, expected, hex);
    return 1;
  }
  return 0;
}

/**
  Read a whole archive, judging every entry, without writing anything.

  CPack wraps an archive's contents in one directory named after the
  archive file. When every entry lives under such a directory it is
  stripped from the paths and dropped from the list, so the remaining paths
  are relative to the basedir. Any other layout is taken as is: "lib/x" and
  "top/lib/x" cannot be told apart by shape, only by that name.

  @param[in]   file     Original archive name, used to identify its wrapper.
  @param[in]   contents Private archive snapshot.
  @param[out]  entries  Initialized array, filled with tar_entry.
  @param[out]  topdir   The stripped directory, "" when nothing was stripped.

  @retval int error = 1, success = 0
*/

static int tar_scan(const char *file, FILE *contents,
                     DYNAMIC_ARRAY *entries, char *topdir)
{
  struct tar_reader r;
  struct tar_entry e, *p;
  const char *base;
  my_bool have_topdir= TRUE;
  size_t i, len;
  int rc;

  if (tar_open(&r, file, contents))
    return 1;
  topdir[0]= '\0';
  base= file + dirname_length(file);
  while ((rc= tar_next(&r, &e)) > 0)
  {
    const char *slash;
    if (!valid_relative_path(e.path))
    {
      fprintf(stderr, "ERROR: '%s' contains the unsafe path '%s'.\n", file,
              e.path);
      rc= -1;
      break;
    }
    /* a top directory exists only if no entry sits beside it at the root */
    slash= strchr(e.path, '/');
    len= slash ? (size_t) (slash - e.path) : strlen(e.path);
    if (!slash && !e.is_dir)
      have_topdir= FALSE;
    if (!topdir[0])
      safe_strcpy_truncated(topdir, MY_MIN(FN_REFLEN, len + 1), e.path);
    else if (strlen(topdir) != len || strncmp(topdir, e.path, len) != 0)
      have_topdir= FALSE;
    if (insert_dynamic(entries, &e))
    {
      rc= -1;
      break;
    }
  }
  tar_close(&r);
  if (rc < 0)
    return 1;
  if (!entries->elements)
  {
    fprintf(stderr, "ERROR: '%s' is empty.\n", file);
    return 1;
  }
  len= strlen(topdir);
  if (!have_topdir || strncmp(base, topdir, len) != 0 ||
      (base[len] != '\0' && base[len] != '.'))
  {
    topdir[0]= '\0';
    return 0;
  }

  for (i= 0; i < entries->elements; )
  {
    p= dynamic_element(entries, i, struct tar_entry *);
    if (strlen(p->path) == len)  /* the top directory itself */
      delete_dynamic_element(entries, i);
    else
    {
      memmove(p->path, p->path + len + 1, strlen(p->path) - len);
      i++;
    }
  }
  if (!entries->elements)
  {
    fprintf(stderr, "ERROR: '%s' contains only an empty directory.\n", file);
    return 1;
  }
  return 0;
}


/**
  Append and flush one manifest record for rollback and later uninstall.

  @retval int error = 1, success = 0
*/

static int manifest_append(FILE *m, const char *key, const char *value)
{
  /* a newline in a value, such as the --file path written as "source",
     would be read back as a second manifest line: refuse it */
  if (strpbrk(value, "\r\n"))
  {
    fprintf(stderr, "ERROR: refusing to write a '%s' value that contains a "
            "newline into the manifest.\n", key);
    return 1;
  }
  if (fprintf(m, "%s: %s\n", key, value) < 0 || fflush(m))
  {
    fprintf(stderr, "ERROR: cannot write the manifest: %s.\n",
            strerror(errno));
    return 1;
  }
  return 0;
}


/**
  Copy one entry's data from the archive into a new file.

  Create exclusively relative to a checked parent directory.

  @retval int error = 1, success = 0
*/

static int tar_extract_file(struct tar_reader *r, struct tar_entry *e,
                            const char *basedir, const char *full, FILE *m,
                            const char *relpath)
{
  char buf[8192];
  ulonglong left= e->size;
  File fd;
  int error= 0;

  fd= plugin_file_op(basedir, relpath, PLUGIN_OPEN,
                     O_WRONLY | O_CREAT | O_EXCL | O_BINARY);
  if (fd < 0)
  {
    fprintf(stderr, "ERROR: cannot create '%s': %s.\n", full,
            strerror(my_errno));
    return 1;
  }
  /* Record ownership before writing contents so a failed write is tracked. */
  if (manifest_append(m, "file", relpath))
  {
    my_close(fd, MYF(0));
    plugin_file_op(basedir, relpath, PLUGIN_DELETE, 0);
    return 1;
  }
  while (left && !error)
  {
    size_t n= (size_t) MY_MIN(left, sizeof(buf));
    if (tar_read_bytes(r, buf, n) ||
        my_write(fd, (uchar *) buf, n, MYF(MY_WME | MY_NABP)))
      error= 1;
    left-= n;
    r->data_left-= n;
  }
#ifndef _WIN32
  /* Change the opened file, not a pathname that could have been replaced. */
  if (!error && e->mode && fchmod(fd, e->mode))
  {
    fprintf(stderr, "ERROR: cannot set permissions on '%s': %s.\n",
            full, strerror(errno));
    error= 1;
  }
#endif
  if (my_close(fd, MYF(MY_WME)))
    error= 1;
  return error;
}


/**
  Create the scanned entries and record ownership. Existing directories
  are checked and reused without recording ownership of them.

  The archive must list newly created parent directories before children.

  @retval int error = 1, success = 0
*/

static int tar_extract(const char *file, FILE *contents, const char *basedir,
                       const char *topdir, DYNAMIC_ARRAY *entries, FILE *m)
{
  struct tar_reader r;
  struct tar_entry e;
  char full[FN_REFLEN];
  size_t skip= topdir[0] ? strlen(topdir) + 1 : 0;
  size_t i;
  int rc;

  if (tar_open(&r, file, contents))
    return 1;
  /* the archive is walked again, in step with the approved entry list */
  for (i= 0; i < entries->elements; i++)
  {
    struct tar_entry *ok= dynamic_element(entries, i, struct tar_entry *);
    do
    {
      if ((rc= tar_next(&r, &e)) <= 0)
      {
        if (rc == 0)
          fprintf(stderr, "ERROR: '%s' changed while it was being read.\n",
                  file);
        tar_close(&r);
        return 1;
      }
    } while (strlen(e.path) < skip || strcmp(e.path + skip, ok->path) != 0);

    if (build_full_path(full, sizeof(full), basedir, ok->path))
      goto err;
    if (e.is_dir)
    {
      if (file_exists(full))
      {
        if (plugin_file_op(basedir, ok->path, PLUGIN_CHECK_DIR, 0))
        {
          fprintf(stderr, "ERROR: '%s' is not an accessible directory "
                  "without symlinks.\n", full);
          goto err;
        }
        continue;
      }
      if (plugin_file_op(basedir, ok->path, PLUGIN_MKDIR, 0))
      {
        fprintf(stderr, "ERROR: cannot create directory '%s': %s.\n", full,
                strerror(my_errno));
        goto err;
      }
      if (manifest_append(m, "dir", ok->path))
        goto err;
    }
    else
    {
      if (tar_extract_file(&r, &e, basedir, full, m, ok->path))
        goto err;
    }
  }
  tar_close(&r);
  return 0;

err:
  tar_close(&r);
  return 1;
}


/**
  Tell the user how to enable what was just installed. Install never
  edits the server configuration: a tarball installation has no conf.d
  and no convention for one, and the user may keep my.cnf anywhere.
*/

static void print_enable_instructions(const char *basedir,
                                      DYNAMIC_ARRAY *entries)
{
  size_t i, prefix= sizeof(STR(INSTALL_PLUGINDIR)) - 1;
  int pass, shown= 0;

  /* two passes: the INSTALL SONAME lines, then the plugin-load-add lines */
  for (pass= 0; pass < 2; pass++)
  {
    for (i= 0; i < entries->elements; i++)
    {
      struct tar_entry *e= dynamic_element(entries, i, struct tar_entry *);
      const char *name= e->path + prefix + 1, *ext;
      if (e->is_dir ||
          strncmp(e->path, STR(INSTALL_PLUGINDIR), prefix) != 0 ||
          e->path[prefix] != '/' || strchr(name, '/') ||
          !(ext= strstr(name, SO_EXT)))
        continue;
      if (pass == 0)
      {
        if (!shown++)
          printf("To enable it, either run in the server:\n");
        printf("  INSTALL SONAME '%.*s';\n", (int) (ext - name), name);
      }
      else
        printf("  plugin-load-add=%s\n", name);
    }
    if (pass == 0 && shown)
      printf("or add to your server configuration and restart:\n"
             "  [mariadb]\n");
  }
  if (!shown)
    printf("No plugin library was found under %s/%s; nothing to enable.\n",
           basedir, STR(INSTALL_PLUGINDIR));
}

#endif /* !PKG_DELEGATION */


/**
  Install a plugin.

  On rpm and deb installations the work is delegated to the system package
  manager, which resolves the uniform package name through its own real
  package names (via Provides on rpm). Its exit code is passed through.

  @param[in]  name     The normalized plugin name.
  @param[in]  basedir  The base directory, empty for packaged installations.

  @retval int error = nonzero, success = 0
*/

static int do_install(const char *name, const char *basedir)
{
#ifdef PKG_DELEGATION
  char package[PACKAGE_NAME_SIZE];
  const char *pm;
  char *cmd_argv[4];

  /* Do not silently replace a requested archive with a repository package. */
  if (opt_file || opt_sha256 || opt_base_url)
  {
    fprintf(stderr, "ERROR: --file, --sha256 and --base-url are only for tarball "
            "installations; on this system plugins are installed by the "
            "package manager.\n");
    return 1;
  }
  if (check_root("install"))
    return 1;
  if (!(pm= get_package_manager()))
    return 1;

  build_package_name(package, sizeof(package), name);
  cmd_argv[0]= (char *) pm;
  cmd_argv[1]= (char *) "install";
  cmd_argv[2]= package;
  cmd_argv[3]= 0;
  return run_argv(cmd_argv);
#else
  char manifest[FN_REFLEN], full[FN_REFLEN], topdir[FN_REFLEN];
  char sha256[65];
  DYNAMIC_ARRAY entries;
  struct tar_entry *e;
  struct index_entry remote;
  FILE *m= 0, *contents= NULL;
  char url[KV_LINE_SIZE * 2];
  const char *archive= opt_file, *source= opt_file, *expected= opt_sha256;
  size_t i;
  int error= 1;

  if ((opt_sha256 && !opt_file) || (opt_base_url && opt_file))
  {
    fprintf(stderr, "ERROR: --sha256 requires --file; --base-url cannot "
            "be combined with --file.\n");
    return 1;
  }
  if (build_manifest_path(manifest, sizeof(manifest), basedir, name))
    return 1;
  if (file_exists(manifest))
  {
    fprintf(stderr, "ERROR: plugin '%s' is already installed.\n", name);
    return 1;
  }
  if (!archive)
  {
    FILE *index;
    int invalid;
    if (build_download_url(url, sizeof(url), PLUGIN_INDEX))
      return 1;
    if (!(index= download_file(url, 8 * 1024 * 1024)))
      return 1;
    invalid= read_index(index, url, name, &remote);
    my_fclose(index, MYF(0));
    if (invalid || build_download_url(url, sizeof(url), remote.file))
      return 1;
    archive= remote.file;
    expected= remote.sha256;
    source= url;
    if (opt_dry_run)
    {
      printf("would download %s\nwould verify SHA-256 %s\n"
             "would install plugin '%s' into %s\n",
             source, expected, name, basedir);
      return 0;
    }
    printf("Downloading %s\n", source);
    if (!(contents= download_file(source, 1024 * 1024 * 1024)))
      return 1;
  }
  if (!contents && !(contents= copy_local_archive(archive)))
    return 1;
  if (expected)
  {
    if (verify_sha256(archive, contents, expected, sha256))
      goto close_archive;
  }
  else
  {
    safe_strcpy(sha256, sizeof(sha256), "unverified");
    fprintf(stderr, "WARNING: no --sha256 given, the tarball is not "
            "verified.\n");
  }
  if (my_init_dynamic_array(PSI_NOT_INSTRUMENTED, &entries,
                            sizeof(struct tar_entry), 64, 64, MYF(MY_WME)))
    goto close_archive;
  if (tar_scan(archive, contents, &entries, topdir))
    goto end;

  /*
    Everything is judged before anything is written: a file that already
    exists is refused, as it belongs to the server or to another plugin.
  */
  for (i= 0; i < entries.elements; i++)
  {
    e= dynamic_element(&entries, i, struct tar_entry *);
    if (!strncmp(e->path, MANIFEST_SUBDIR, sizeof(MANIFEST_SUBDIR) - 1) &&
        (e->path[sizeof(MANIFEST_SUBDIR) - 1] == '/' ||
         e->path[sizeof(MANIFEST_SUBDIR) - 1] == '\0'))
    {
      fprintf(stderr, "ERROR: archive entry '%s' uses the reserved manifest "
              "directory.\n", e->path);
      goto end;
    }
    if (build_full_path(full, sizeof(full), basedir, e->path))
      goto end;
    if (!e->is_dir && file_exists(full))
    {
      fprintf(stderr, "ERROR: '%s' already exists, refusing to overwrite "
              "it.\n", full);
      goto end;
    }
  }

  if (opt_dry_run)
  {
    for (i= 0; i < entries.elements; i++)
    {
      e= dynamic_element(&entries, i, struct tar_entry *);
      build_full_path(full, sizeof(full), basedir, e->path);
      if (e->is_dir && file_exists(full))
        continue;
      printf("would %s %s\n", e->is_dir ? "create directory" : "install",
              full);
    }
    printf("would write %s\n", manifest);
    error= 0;
    goto end;
  }

  /*
    Write ownership records during extraction so ordinary failures can use
    the same removal logic as uninstall. This is not a crash-atomic journal.
  */
  if (build_full_path(full, sizeof(full), basedir, MANIFEST_SUBDIR))
    goto end;
  if (!file_exists(full) &&
      plugin_file_op(basedir, MANIFEST_SUBDIR, PLUGIN_MKDIR, 0))
    goto end;
  /* my_fopen() would map these flags to fopen("w"), dropping O_EXCL and
     following a dangling symlink; my_open() honours O_EXCL, so the manifest
     is created only if the name does not already exist */
  {
    File mfd= plugin_file_op(basedir, manifest + strlen(basedir) + 1,
                             PLUGIN_OPEN,
                             O_WRONLY | O_CREAT | O_EXCL | O_BINARY);
    if (mfd < 0)
    {
      fprintf(stderr, "ERROR: cannot create '%s': %s.\n", manifest,
              strerror(my_errno));
      goto end;
    }
    if (!(m= my_fdopen(mfd, manifest, O_WRONLY, MYF(MY_WME))))
    {
      my_close(mfd, MYF(0));
      plugin_file_op(basedir, manifest + strlen(basedir) + 1, PLUGIN_DELETE, 0);
      goto end;
    }
  }
  {
    char today[16];
    struct tm *t;
    time_t now= time(0);
    t= localtime(&now);
    strftime(today, sizeof(today), "%Y-%m-%d", t);
    if (manifest_append(m, "name", name) ||
        manifest_append(m, "source", source) ||
        manifest_append(m, "sha256", sha256) ||
        manifest_append(m, "date", today) ||
        (topdir[0] && manifest_append(m, "topdir", topdir)))
      goto rollback;
  }
  if (tar_extract(archive, contents, basedir, topdir, &entries, m))
    goto rollback;
  my_fclose(m, MYF(0));
  m= 0;

  printf("Plugin '%s' installed into %s.\n", name, basedir);
  print_enable_instructions(basedir, &entries);
  error= 0;
  goto end;

rollback:
  if (m)
    my_fclose(m, MYF(0));
  fprintf(stderr, "ERROR: installation of '%s' failed, removing what was "
          "written.\n", name);
  manifest_remove(basedir, manifest);
end:
  delete_dynamic(&entries);
close_archive:
  if (contents)
    my_fclose(contents, MYF(0));
  return error;
#endif
}


/**
  Uninstall a plugin.

  On deb installations the packages carry the uniform name, so it is passed
  to apt-get directly. On rpm installations the uniform name is only a
  Provides alias of the real package name, and dnf 5 does not resolve
  "remove" arguments through Provides (dnf 4 and zypper do), so the alias
  is first translated by querying the rpm database. This also gives a
  clear error when the plugin is not installed.

  @param[in]  name     The normalized plugin name.
  @param[in]  basedir  The base directory, empty for packaged installations.

  @retval int error = nonzero, success = 0
*/

static int do_uninstall(const char *name, const char *basedir)
{
#ifdef PKG_DELEGATION
  char package[PACKAGE_NAME_SIZE];
  const char *pm;
  const char *target;
  char *cmd_argv[7];
  int error;
#ifdef INSTALL_LAYOUT_RPM
  DYNAMIC_STRING providers;
  char *nl;
#endif

  if (check_root("uninstall"))
    return 1;
  if (!(pm= get_package_manager()))
    return 1;

  build_package_name(package, sizeof(package), name);
  target= package;

#ifdef INSTALL_LAYOUT_RPM
  if (init_dynamic_string(&providers, "", 256, 256))
    return 1;
  cmd_argv[0]= (char *) "rpm";
  cmd_argv[1]= (char *) "-q";
  cmd_argv[2]= (char *) "--whatprovides";
  cmd_argv[3]= package;
  cmd_argv[4]= (char *) "--qf";
  cmd_argv[5]= (char *) "%{NAME}\n";
  cmd_argv[6]= 0;
  if (run_argv_capture(cmd_argv, &providers, 0) || !providers.length)
  {
    fprintf(stderr, "ERROR: plugin '%s' is not installed.\n", name);
    dynstr_free(&providers);
    return 1;
  }
  if (!(nl= strchr(providers.str, '\n')))
    nl= strend(providers.str);
  if (nl[0] && nl[1])
  {
    fprintf(stderr, "ERROR: several packages provide '%s':\n%s"
            "Remove the right one with the package manager directly.\n",
            package, providers.str);
    dynstr_free(&providers);
    return 1;
  }
  *nl= '\0';
  target= providers.str;
#endif

  cmd_argv[0]= (char *) pm;
  cmd_argv[1]= (char *) "remove";
  cmd_argv[2]= (char *) target;
  cmd_argv[3]= 0;
  error= run_argv(cmd_argv);
#ifdef INSTALL_LAYOUT_RPM
  dynstr_free(&providers);
#endif
  return error;
#else
  char manifest[FN_REFLEN];

  if (build_manifest_path(manifest, sizeof(manifest), basedir, name))
    return 1;
  if (!file_exists(manifest))
  {
    fprintf(stderr, "ERROR: plugin '%s' is not installed.\n", name);
    return 1;
  }
  if (manifest_remove(basedir, manifest))
    return 1;
  if (!opt_dry_run)
    printf("Plugin '%s' uninstalled from %s.\n", name, basedir);
  return 0;
#endif
}


/**
  Run the new package-manager style commands.

  Options have already been parsed by main. Validate the verb and name and
  dispatches to the appropriate command handler. The plugin name is
  normalized to lower case before validation.

  @param[in]  argc  The number of arguments.
  @param[in]  argv  The arguments.

  @retval int error = 1, success = 0
*/

static int run_new_command(int argc, char **argv)
{
  char name[NAME_CHAR_LEN + 1];
  char basedir[FN_REFLEN];
  const char *verb;
  size_t i, len;
  int is_search;

  /* --print-defaults only displays information; it must not fall through
     into an install or uninstall that changes the system */
  if (opt_print_defaults)
    return 0;

  if (argc < 1)
  {
    usage();
    return 1;
  }

  verb= argv[0];
  if (strcmp(verb, "search") != 0 && strcmp(verb, "install") != 0 &&
      strcmp(verb, "uninstall") != 0)
  {
    fprintf(stderr, "ERROR: unknown command '%s'.\n", verb);
    usage();
    return 1;
  }

  /* the search term is optional: without it every plugin is listed */
  is_search= strcmp(verb, "search") == 0;
  if (is_search ? argc > 2 : argc != 2)
  {
    fprintf(stderr, is_search ?
            "ERROR: '%s' takes at most one search term.\n" :
            "ERROR: '%s' requires exactly one plugin name.\n", verb);
    usage();
    return 1;
  }

  name[0]= '\0';
  if (argc == 2)
  {
    len= strlen(argv[1]);
    if (len > NAME_CHAR_LEN)
    {
      fprintf(stderr, "ERROR: plugin name is too long (max %d characters).\n",
              NAME_CHAR_LEN);
      return 1;
    }
    for (i= 0; i <= len; i++)
      name[i]= (char) tolower((unsigned char) argv[1][i]);

    if (validate_plugin_name(name))
      return 1;
  }

#ifdef INSTALL_LAYOUT_DEB
  /* APT interprets a trailing '-' on an install/remove operand as removal. */
  if (!is_search && name[strlen(name) - 1] == '-')
  {
    fprintf(stderr, "ERROR: plugin names ending in '-' cannot be passed "
            "to apt-get.\n");
    return 1;
  }
#endif

  if (detect_install_method(basedir, sizeof(basedir)))
    return 1;

  if (is_search)
    return do_search(name);
  if (strcmp(verb, "install") == 0)
    return do_install(name, basedir);
  return do_uninstall(name, basedir);
}

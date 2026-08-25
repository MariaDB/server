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
#endif

/* Global variables. */
static uint my_end_arg= 0;
static uint opt_verbose=0;
static my_bool opt_dry_run= 0;
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
static int do_search(const char *name, const char *basedir);
static int do_install(const char *name, const char *basedir);
static int do_uninstall(const char *name, const char *basedir);


int main(int argc,char *argv[])
{
  int error= 0;
  char tp_path[FN_REFLEN];
  char server_path[FN_REFLEN];
  char operation[16];

  MY_INIT(argv[0]);
  sf_leaking_memory=1; /* don't report memory leaks on early exits */
  plugin_data.name= 0; /* initialize name                          */

  /*
    The new package-manager style commands (search|install|uninstall) are
    handled in run_new_command(). The legacy "<plugin> ENABLE|DISABLE"
    syntax is recognized by scanning the raw arguments, before any option
    parsing, and continues through the original code path below unchanged,
    for backward compatibility.
  */
  if (!is_legacy_syntax(argc, argv))
  {
    error= run_new_command(argc, argv);
    my_end(my_end_arg);
    exit(error);
  }

  /*
    The following operations comprise the method for enabling or disabling
    a plugin. We begin by processing the command options then check the
    directories specified for --datadir, --basedir, --plugin-dir, and
    --plugin-ini (if specified). If the directories are Ok, we then look
    for the mysqld executable and the plugin soname. Finally, we build a
    bootstrap command file for use in bootstraping the server.

    If any step fails, the method issues an error message and the tool exits.

      1) Parse, execute, and verify command options.
      2) Check access to directories.
      3) Look for mysqld executable.
      4) Look for the plugin.
      5) Build a bootstrap file with commands to enable or disable plugin.

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
  Get a temporary file name.

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
  Run a command in a shell.

  This function will attempt to execute the command specified by using the
  popen() method to open a shell and execute the command passed and store the
  output in a result file. If the --verbose option was specified, it will open
  the result file and print the contents to stdout.

  @param[in]  cmd   The command to execute.
  @param[in]  mode  The mode for popen() (e.g. "r", "w", "rw")

  @return int error code or 0 for success.
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

  @param[in]  path  The Windows path to examine.

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

  @returns string containing escaped quotes if spaces found in path
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
  Get the default values from the my.cnf file.

  This method gets the default values for the following parameters:

  --datadir
  --basedir
  --plugin-dir
  --plugin-ini
  --lc-messages-dir

  These values are used if the user has not specified a value.

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
  Print the default values as read from the my.cnf file.

  This method displays the default values for the following parameters:

  --datadir
  --basedir
  --plugin-dir
  --plugin-ini
  --lc-messages-dir

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
  Process the arguments and identify an option and store its value.

  @param[in]  optid      The single character shortcut for the argument.
  @param[in]  my_option  Structure of legal options.
  @param[in]  argument   The argument value to process.
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
  Check to see if a file exists.

  @param[in]  filename  File to locate.

  @retval int file not found = 1, file found = 0
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
  Search a specific path and sub directory for a file name.

  @param[in]  base_path  Original path to use.
  @param[in]  tool_name  Name of the tool to locate.
  @param[in]  subdir     The sub directory to search.
  @param[out] tool_path  If tool found, return complete path.

  @retval int error = 1, success = 0
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
  Search known common paths and sub directories for a file name.

  @param[in]  base_path  Original path to use.
  @param[in]  tool_name  Name of the tool to locate.
  @param[out] tool_path  If tool found, return complete path.

  @retval int error = 1, success = 0
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
  Read the plugin ini file.

  This function attempts to read the plugin config file from the plugin_dir
  path saving the data in the the st_plugin structure. If the file is not
  found or the file cannot be read, an error is generated.

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

    /* strip /n */
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
  Check the options for validity.

  This function checks the arguments for validity issuing the appropriate
  error message if arguments are missing or invalid. On success, @operation
  is set to either "ENABLE" or "DISABLE".

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
    /* read the plugin config file and check for match against argument */
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
  Parse, execute, and verify command options.

  This method handles all of the option processing including the optional
  features for displaying data (--print-defaults, --help ,etc.) that do not
  result in an attempt to ENABLE or DISABLE of a plugin.

  @param[in]   arc        Count of arguments
  @param[in]   argv       Array of arguments
  @param[out]  operation  Operation (ENABLE or DISABLE)

  @retval int error = 1, success = 0, exit program = -1
*/

static int process_options(int argc, char *argv[], char *operation)
{
  int error= 0;

  /* Parse and execute command-line options */
  if ((error= handle_options(&argc, &argv, my_long_options, get_one_option)))
    return error;

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
    If the user did not specify the option to skip loading defaults from a
    config file and the required options are not present or there was an error
    generated when the defaults were read from the file, exit.
  */
  if (!opt_no_defaults && ((error= get_default_values())))
    return -1;

  /*
   Check to ensure required options are present and validate the operation.
   Note: this method also validates the plugin specified by attempting to
   read a configuration file named <plugin_name>.ini from the --plugin-dir
   or --plugin-ini location if the --plugin-ini option presented.
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
  Check access

  This method checks to ensure all of the directories (opt_basedir,
  opt_plugin_dir, opt_datadir, and opt_plugin_ini) are accessible by
  the user.

  @retval int error = 1, success = 0
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
  Find the plugin library.

  This function attempts to use the @c plugin_dir option passed on the
  command line to locate the plugin.

  @param[out] tp_path   The actual path to plugin with FN_SOEXT applied.

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

  param[in]  operation  The type of operation (ENABLE or DISABLE)
  param[out] bootstrap  A FILE* pointer

  @retval int error = 1, success = 0
*/

static int build_bootstrap_file(char *operation, char *bootstrap)
{
  int error= 0;
  FILE *file= 0;

  /*
    Perform plugin operation : ENABLE or DISABLE

    The following creates a temporary bootstrap file and populates it with
    the appropriate SQL commands for the operation. For ENABLE, REPLACE
    statements are created. For DISABLE, DELETE statements are created. The
    values for these statements are derived from the plugin_data read from the
    <plugin_name>.ini configuration file. Once the file is built, a call to
    mysqld is made in read only, bootstrap modes to read the SQL statements
    and execute them.

    Note: Replace was used so that if a user loads a newer version of a
          library with a different library name, the new library name is
          used for symbols that match.
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
  Dump bootstrap file.

  Read the contents of the bootstrap file and print it out.

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
  Bootstrap the server

  Create a command line sequence to launch mysqld in bootstrap mode. This
  will allow mysqld to launch a minimal server instance to read and
  execute SQL commands from a file piped in (the bootstrap file). We use
  the --no-defaults option to skip reading values from the config file.

  The bootstrap mode skips loading of plugins and many other subsystems.
  This allows the mysql_plugin tool to insert the correct rows into the
  mysql.plugin table (for ENABLE) or delete the rows (for DISABLE). Once
  the server is launched in normal mode, the plugin will be loaded
  (for ENABLE) or not loaded (for DISABLE). In this way, we avoid the
  (sometimes) complicated LOAD PLUGIN commands.

  @param[in]  server_path     Path to server executable
  @param[in]  bootstrap_file  Name of bootstrap file to read

  @retval int error = 1, success = 0
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

  The check is done on the raw arguments, before any option parsing, so
  that legacy invocations take the original code path unchanged.

  @param[in]  argc  The number of arguments.
  @param[in]  argv  The arguments.

  @retval int legacy syntax = 1, new syntax = 0
*/

static int is_legacy_syntax(int argc, char **argv)
{
  int i;

  for (i= 1; i < argc; i++)
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
  only lower case alphanumerics, '_' and '-' are accepted. The name is
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
    if (!isalnum((unsigned char) *p) && *p != '_' && *p != '-')
    {
      fprintf(stderr, "ERROR: invalid character '%c' in plugin name. "
              "Use only [a-z0-9_-].\n", *p);
      return 1;
    }
  }
  return 0;
}

/**
  Verify that the tool is part of the installation it was built for.

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
  safe_strcat(self_path, sizeof(self_path), base_name(my_progname));
  if (my_realpath(real_path, self_path, MYF(0)))
    safe_strcpy(real_path, sizeof(real_path), self_path);

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
  Refuse to continue without root privileges.

  The package manager would fail anyway, but only after a repository
  refresh, with an error that does not mention this tool.

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

  The command is executed directly, not through a shell, so the arguments
  cannot be reinterpreted. The child inherits the standard streams: the
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
  if (WIFSIGNALED(status))
    return 127;
  return WEXITSTATUS(status);
}


#define PLUGIN_PREFIX "mariadb-plugin-"
#define PLUGIN_PREFIX_LEN (sizeof(PLUGIN_PREFIX) - 1)
#define PACKAGE_NAME_SIZE (PLUGIN_PREFIX_LEN + NAME_CHAR_LEN + 1)


/**
  Build the distribution-independent package name (D2): mariadb-plugin-
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
  Search: every backend answers the same two questions - which packages
  provide mariadb-plugin-* and which of them are installed - through a
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

static void parse_dnf_records(char *output, int installed)
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
      return;
    safe_strcpy(e->package, sizeof(e->package), package);
    if (!e->description[0])
      safe_strcpy(e->description, sizeof(e->description), summary);
    if (installed)
      e->installed= 1;
  }
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
  parse_dnf_records(search_output.str, 0);

  /* same query against the installed packages only, for the status */
  if (run_argv_capture(inst_argv, &search_output, 0) == 0)
    parse_dnf_records(search_output.str, 1);
  return 0;
}


/**
  Parse "zypper --xmlout search --provides" solvable lines, e.g.
  <solvable status="not-installed" name="X" summary="Y" kind="package"/>.
  zypper never reports which capability matched, so the uniform names are
  filled in afterwards by search_zypper_names().

  @param[in]  output  The captured zypper output, modified in place.
*/

static void parse_zypper_solvables(char *output)
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
      return;
  }
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
  parse_zypper_solvables(search_output.str);
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

static void parse_apt_records(char *output)
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
      return;
    if (sep && !e->description[0])
      safe_strcpy(e->description, sizeof(e->description), sep + 3);
  }
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
  parse_apt_records(search_output.str);

  /*
    dpkg-query prints "no packages found" on stderr and exits nonzero
    when nothing is installed, which is an answer here, not an error.
  */
  cmd_argv[0]= (char *) "dpkg-query";
  cmd_argv[1]= (char *) "-W";
  cmd_argv[2]= (char *) "-f=${Package} ${db:Status-Status}\n";
  cmd_argv[3]= (char *) PLUGIN_PREFIX "*";
  cmd_argv[4]= 0;
  if (run_argv_capture(cmd_argv, &search_output, 1))
    return 0;
  for (line= search_output.str; line && *line; line= next)
  {
    if ((next= strchr(line, '\n')))
      *next++= '\0';
    if (!(sep= strchr(line, ' ')))
      continue;
    *sep++= '\0';
    if (strcmp(sep, "installed") == 0 &&
        strncmp(line, PLUGIN_PREFIX, PLUGIN_PREFIX_LEN) == 0 &&
        (e= add_plugin_entry(line)))
      e->installed= 1;
  }
  return 0;
}

#endif /* INSTALL_LAYOUT_DEB */

#endif /* PKG_DELEGATION */


/**
  Search for plugins.

  On rpm and deb installations the distribution's package index is the
  plugin metadata: it is queried for everything providing mariadb-plugin-*
  and the result is shown uniformly as plugin names, never as the
  distribution's own package names. Needs no root.

  @param[in]  term     Substring to match, empty to list all plugins.
  @param[in]  basedir  The base directory, empty for packaged installations.

  @retval int error or no matches = 1, matches printed = 0
*/

static int do_search(const char *term, const char *basedir)
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
  dynstr_free(&search_output);
  delete_dynamic(&plugin_list);
  return error ? 1 : 0;
#else
  printf("search: not available for %s installations yet, the plugin "
         "index does not exist\n", INSTALL_METHOD_NAME);
  return 1;
#endif
}


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
  printf("install: '%s' (%s installation%s%s) not implemented yet\n", name,
         INSTALL_METHOD_NAME, *basedir ? ", basedir=" : "", basedir);
  return 0;
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
  printf("uninstall: '%s' (%s installation%s%s) not implemented yet\n", name,
         INSTALL_METHOD_NAME, *basedir ? ", basedir=" : "", basedir);
  return 0;
#endif
}


/**
  Run the new package-manager style commands.

  Parses the options (--help, --version, etc. are handled by
  handle_options), then validates the verb and the plugin name and
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
  int error, is_search;

  if ((error= handle_options(&argc, &argv, my_long_options, get_one_option)))
    return 1;

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

  if (detect_install_method(basedir, sizeof(basedir)))
    return 1;

  if (is_search)
    return do_search(name, basedir);
  if (strcmp(verb, "install") == 0)
    return do_install(name, basedir);
  return do_uninstall(name, basedir);
}

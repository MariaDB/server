/* Copyright (c) 2000, 2017, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2011, 2018, MariaDB Corporation
   Copyright (c) 2024, Väinö Mäkelä

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#define VER "1.0"
#include <my_global.h>
#include "mariadbd_options.h"
#include <my_global.h>
#include <my_sys.h>
#include <my_dir.h>
#include <mysql/psi/mysql_file.h>
#include <m_string.h>
#include <my_getopt.h>
#include <my_default.h>
#include <mysql_version.h>
#include <stdarg.h>
#include <welcome_copyright_notice.h>

#if!defined (X_OK) && defined(_WIN32)
#define X_OK 0              /* Test existence of file with access() */
#endif


#define load_default_groups mariadbd_groups
#include <mysqld_default_groups.h>
#undef load_default_groups

const char *mysqld_groups[]=
{
  "mysqld", "server", 0, 0
};

/* The following is from my_getopt.c */
static const char *special_opt_prefix[]=
{"skip", "disable", "enable", "maximum", "loose", "autoset", 0};
static const uint special_opt_prefix_lengths[]=
{ 4,      7,         6,        7,         5,       7,        0};

#ifndef WEXITSTATUS
# ifdef _WIN32
#  define WEXITSTATUS(stat_val) (stat_val)
# else
#  define WEXITSTATUS(stat_val) ((unsigned)(stat_val) >> 8)
# endif
#endif

struct upgrade_ctx
{
  MEM_ROOT *alloc;
  TYPELIB *group;                /* Default groups and extra groups */
  DYNAMIC_ARRAY updated_files;
  my_bool failed;
};


enum edit_mode
{
  EDIT_MODE_NONE,
  EDIT_MODE_REMOVE,
  EDIT_MODE_COMMENT,
  EDIT_MODE_INLINE,
  EDIT_MODE_LAST,
};

static const char *edit_mode_values[]= {
  "", "remove", "comment", "inline", "last", NullS
};
static TYPELIB edit_mode_typelib=
{array_elements(edit_mode_values) - 1, "", edit_mode_values, NULL, NULL};

enum from_mode
{
  FROM_MYSQL,
  FROM_MARIADB,
};

static const char *from_mode_values[]= {
  "MySQL", "MariaDB", NullS
};
static TYPELIB from_mode_typelib=
{array_elements(from_mode_values) - 1, "", from_mode_values, NULL, NULL};

LEX_CSTRING mysql_name=   {STRING_WITH_LEN("mysql")};
LEX_CSTRING mariadb_name= {STRING_WITH_LEN("mariadb")};

static const char *opt_error_group="mysqld-5.7";
static const char *opt_unsupported_group="options-not-recognized-by-mariadbd";
static my_bool opt_update;
static my_bool opt_backup;
static my_bool opt_print;
static my_bool opt_no_myisam_options;
static my_bool opt_add_skip_slave_start;
static my_bool opt_silent;
static my_bool opt_update_paths;
static const char **groups_to_use;
static const char *opt_basedir= "";
static ulong opt_edit_mode, opt_convert_from;

static PSI_memory_key key_memory_upgrade_config;
static PSI_file_key key_file_cnf;
static size_t global_update_count= 0;
static my_bool copy_code_to_mariadb= 0;
static my_bool give_error_for_missing_files= 0;
static my_bool opt_mariadbd_testing;

#ifdef _WIN32
static const char *f_extensions[]= { ".ini", ".cnf", 0 };
#else
static const char *f_extensions[]= { ".cnf", 0 };
#endif

static uint count_array_elements(DYNAMIC_ARRAY *array);
static my_bool delete_line(DYNAMIC_ARRAY *array, char *line);

/**
  Remove the -upgrade-config-orig backups of the updated files if successful and
  replace the updated files with the backups if not.
*/
static int finish_updated_files(struct upgrade_ctx *ctx, my_bool success)
{
  size_t i;
  for (i= 0; i < ctx->updated_files.elements; i++)
  {
    char *name = *(char **)dynamic_array_ptr(&ctx->updated_files, i);
    const char *suffix = "-upgrade-config-orig";
    size_t orig_len = strlen(name) + strlen(suffix) + 1;
    char *orig = my_malloc(key_memory_upgrade_config, orig_len, MYF(0));
    if (!orig)
      return 1;
    strmov(strmov(orig, name), suffix);
    if (success)
    {
      my_delete(orig, MYF(0));
    }
    else
    {
      if (my_redel(name, orig, 0, MYF(0)))
      {
        fprintf(stderr, "error: Failed to rename %s to %s: %s",
                orig, name, strerror(errno));
        my_free(orig);
        return 1;
      }
    }
    my_free(orig);
    my_free(name);
  }
  delete_dynamic(&ctx->updated_files);
  return 0;
}


/**
  Run a command using the shell, storing its output in the supplied dynamic
  string.
*/
static int run_command(char* cmd,
                       DYNAMIC_STRING *ds_res)
{
  char buf[512]= {0};
  FILE *res_file;
  int error;

  if (!(res_file= my_popen(cmd, "r")))
  {
    fprintf(stderr, "popen(\"%s\", \"r\") failed\n", cmd);
    return -1;
  }

  while (fgets(buf, sizeof(buf), res_file))
  {
#ifdef _WIN32
    /* Strip '\r' off newlines. */
    size_t len = strlen(buf);
    if (len > 1 && buf[len - 2] == '\r' && buf[len - 1] == '\n')
    {
      buf[len - 2] = '\n';
      buf[len - 1] = 0;
    }
#endif
    dynstr_append(ds_res, buf);
  }

  error= my_pclose(res_file);
  return WEXITSTATUS(error);
}


/**
  Run `mariadbd --help --verbose` with the supplied arguments and write its
  stderr output to ds_res.
*/
static int run_mariadbd(const char *mariadbd_path,
                        DYNAMIC_STRING *ds_res,
                        const char **defaults_args,
                        int defaults_args_count)
{
  int ret;
  int i;
  DYNAMIC_STRING ds_cmdline;

  if (init_dynamic_string(&ds_cmdline, IF_WIN("\"", ""), FN_REFLEN, FN_REFLEN))
  {
    fputs("Out of memory\n", stderr);
    return -1;
  }

  dynstr_append_os_quoted(&ds_cmdline, mariadbd_path, NullS);
  dynstr_append(&ds_cmdline, " ");

  for (i= 0; i < defaults_args_count; i++)
  {
    dynstr_append_os_quoted(&ds_cmdline, defaults_args[i], NullS);
    dynstr_append(&ds_cmdline, " ");
  }

  dynstr_append(&ds_cmdline, "--help ");
  dynstr_append(&ds_cmdline, "--verbose ");
  dynstr_append(&ds_cmdline, "2>&1 ");
  dynstr_append(&ds_cmdline, IF_WIN("1>NUL", "1>/dev/null"));

#ifdef _WIN32
  dynstr_append(&ds_cmdline, "\"");
#endif

  ret= run_command(ds_cmdline.str, ds_res);
  dynstr_free(&ds_cmdline);
  return ret;
}


/**
  Test whether mariadbd can be launched with --no-defaults.
*/
static int test_mariadbd(const char *mariadbd_name)
{
  DYNAMIC_STRING ds_tmp;
  const char *defaults_argument = "--no-defaults";
  int err= 0;

  if (init_dynamic_string(&ds_tmp, "", 32, 32))
  {
    fputs("Out of memory\n", stderr);
    return -1;
  }

  if (run_mariadbd(mariadbd_name,
                   &ds_tmp,
                   &defaults_argument,
                   1))
  {
    fprintf(stderr, "Can't execute %s\n", mariadbd_name);
    err= 1;
  }

  dynstr_free(&ds_tmp);
  return err;
}


static int compare_options(const void *a, const void *b)
{
  const char *first= *(const char **)a;
  const char *second= *(const char **)b;
  return strcmp(first, second);
}


static my_bool mariadbd_option_exists(const char *option)
{
   return bsearch(&option, mariadbd_valid_options,
           array_elements(mariadbd_valid_options),
                  sizeof(mariadbd_valid_options[0]),
           compare_options) != NULL;
}


static my_bool mariadbd_valid_enum_value(const char *option, const char *value)
{
  const char **option_ptr= bsearch(&option, mariadbd_enum_options,
                                   array_elements(mariadbd_enum_options),
                                   sizeof(mariadbd_enum_options[0]),
                                   compare_options);
  longlong val;
  int error;
  if (!option_ptr)
    return TRUE;
  if (find_type(value,
                mariadbd_enum_typelibs[option_ptr - mariadbd_enum_options],
                FIND_TYPE_BASIC) != 0)
    return TRUE;
  val= my_strtoll10(value, (char**) 0, &error);
  return (error == 0 && val < mariadbd_enum_typelibs[option_ptr -
                                                     mariadbd_enum_options]->count);
}


/**
  Check whether the given value is a valid set value for the given
  option.
  Returns 0 on success and the first invalid set index on
  failure. If the option is not a set option, returns 0.
*/

static int mariadbd_check_set_value(const char *option, const char *value)
{
  const char **option_ptr= bsearch(&option, mariadbd_set_options,
                                   array_elements(mariadbd_set_options),
                                   sizeof(mariadbd_set_options[0]),
                                   compare_options);
  TYPELIB *typelib;
  int error_pos= 0;
  if (!option_ptr)
    return 0;
  typelib = mariadbd_set_typelibs[option_ptr - mariadbd_set_options];
  find_typeset(value, typelib, &error_pos);
  if (error_pos)
  {
    ulonglong num;
    char *endptr;

    if (!my_strcasecmp_latin1(value, "all"))
      return 0;

    num= strtol(value, &endptr, 10);
    if (!*endptr)
    {
      if ((num >> 1) >= (1ULL << (typelib->count - 1)))
        return 1;
      return 0;
    }
  }
  return error_pos;
}


enum plugin_check_result
{
  PLUGINS_OK,
  AUDIT_PLUGIN,
};
static enum plugin_check_result check_plugins(const char *option,
                                              const char *value)
{
  if (strcmp(option, "plugin_load") && strcmp(option, "plugin_load_add"))
    return PLUGINS_OK;
  /* TODO: Need more advanced checking? */
  return strstr(value, "audit_log") ? AUDIT_PLUGIN : PLUGINS_OK;
}


/*
  Skip over keyword and get argument after keyword

  SYNOPSIS
   get_argument()
   keyword		Include directive keyword
   kwlen		Length of keyword
   ptr			Pointer to the keword in the line under process
   line			line number

  RETURN
   0	error
   #	Returns pointer to the argument after the keyword.
*/

static char *get_argument(const char *keyword, size_t kwlen,
                          char *ptr, char *name, uint line)
{
  char *end;

  /* Skip over "include / includedir keyword" and following whitespace */

  for (ptr+= kwlen - 1;
       my_isspace(&my_charset_latin1, ptr[0]);
       ptr++)
  {}

  /*
    Trim trailing whitespace from directory name
    The -1 below is for the newline added by fgets()
    Note that my_isspace() is true for \r and \n
  */
  for (end= ptr + strlen(ptr) - 1;
       my_isspace(&my_charset_latin1, *(end - 1));
       end--)
  {}
  end[0]= 0;                                    /* Cut off end space */

  /* Print error msg if there is nothing after !include* directive */
  if (end <= ptr)
  {
    fprintf(stderr,
	    "error: Wrong '!%s' directive in config file: %s at line %d\n",
	    keyword, name, line);
    return 0;
  }
  return ptr;
}


static char *remove_end_comment(char *ptr)
{
  char quote= 0;	/* we are inside quote marks */
  char escape= 0;	/* symbol is protected by escape chagacter */

  for (; *ptr; ptr++)
  {
    if ((*ptr == '\'' || *ptr == '\"') && !escape)
    {
      if (!quote)
	quote= *ptr;
      else if (quote == *ptr)
	quote= 0;
    }
    /* We are not inside a string */
    if (!quote && *ptr == '#')
    {
      *ptr= 0;
      return ptr;
    }
    escape= (quote && *ptr == '\\' && !escape);
  }
  return ptr;
}

struct generator
{
  /* the memory root for allocating line strings */
  MEM_ROOT alloc;
  /* array of strings for the main content */
  DYNAMIC_ARRAY main;
  /*
    array of strings for options that should be added to the error_group
    section
  */
  DYNAMIC_ARRAY old_version;
  /*
    array of strings for options that should be added to the unsupported_group
    section
  */
  DYNAMIC_ARRAY unsupported_version;
  /* array of strings to be added to the mariadbd section */
  DYNAMIC_ARRAY mariadbd_additions;

  int start_of_group;               /* Start of current group */
  int mariadbd_group_start;         /* Start of last mariadbd group */
  /*
    one past the last index of the original mariadbd section in the main array
  */
  int mariadbd_group_end;
  /* the current group from which options are read */
  const char *input_group;
  /*
    The current group in the generated output. This may be different from
    input_group if lines are moved between groups.
  */
  char *output_group;
  /* whether last main line is empty or not */
  my_bool prev_empty;
};

static void generator_init(struct generator *generator)
{
  init_alloc_root(key_memory_upgrade_config, &generator->alloc, 512, 0, MYF(0));
  init_dynamic_array2(key_memory_upgrade_config,
                      &generator->main,
                      sizeof(char *),
                      NULL,
                      0,
                      0,
                      MYF(0));
  init_dynamic_array2(key_memory_upgrade_config,
                      &generator->old_version,
                      sizeof(char *),
                      NULL,
                      0,
                      0,
                      MYF(0));
  init_dynamic_array2(key_memory_upgrade_config,
                      &generator->unsupported_version,
                      sizeof(char *),
                      NULL,
                      0,
                      0,
                      MYF(0));
  init_dynamic_array2(key_memory_upgrade_config,
                      &generator->mariadbd_additions,
                      sizeof(char *),
                      NULL,
                      0,
                      0,
                      MYF(0));
  generator->mariadbd_group_end= -1;
  generator->input_group= NULL;
  generator->output_group= NULL;
  /* don't add extra newlines to the beginning */
  generator->prev_empty= TRUE;
}


/*
  Delete data in mariadbd_additions that was found in last [mariadbd] group

  This is needed to ensure that we don't generate new lines into the
  [mariadbd] group if we run the program twice on it
*/

static void delete_dup_data_from_mariadbd_group(struct generator *generator)
{
  if (generator->mariadbd_group_end >= 0)
  {
    for (int i= generator->mariadbd_group_start;
         i < generator->mariadbd_group_end;
         i++)
    {
      char *element= *(char **) dynamic_array_ptr(&generator->main, i);
      delete_line(&generator->mariadbd_additions, element);
    }
  }
}


static void generator_write(MYSQL_FILE *f, struct generator *generator)
{
  FILE *target_file= f ? f->m_file : stdout;
  size_t i;
  int lines_handled= 0;

  delete_dup_data_from_mariadbd_group(generator);

  if (generator->mariadbd_group_end >= 0)
  {
    /*
      There was a mariadbd group in the file. Put all generated mariadbd
      options into this group.
    */
    for (i= 0; i < (size_t)generator->mariadbd_group_end; i++)
    {
      char *element= *(char **) dynamic_array_ptr(&generator->main, i);
      fputs(element, target_file);
    }
    for (i= 0; i < generator->mariadbd_additions.elements; i++)
    {
      char *element= (*(char **)
                      dynamic_array_ptr(&generator->mariadbd_additions, i));
      if (element)                       /* Ignore deleted elements */
      {
        fputs(element, target_file);
        lines_handled++;
      }
    }
    for (i= (size_t)generator->mariadbd_group_end;
         i < generator->main.elements;
         i++)
    {
      char *element= *(char **) dynamic_array_ptr(&generator->main, i);
      fputs(element, target_file);
    }
  }
  else
  {
    for (i= 0; i < generator->main.elements; i++)
    {
      char *element= *(char **) dynamic_array_ptr(&generator->main, i);
      fputs(element, target_file);
    }
    if (count_array_elements(&generator->mariadbd_additions) > 0)
    {
      if (!generator->prev_empty)
        fputc('\n', target_file);
      fputs("[mariadbd]\n", target_file);
      for (i= 0; i < generator->mariadbd_additions.elements; i++)
      {
        char *element= (*(char **)
                        dynamic_array_ptr(&generator->mariadbd_additions, i));
        if (element)
        {
          fputs(element, target_file);
          lines_handled++;
        }
      }
      generator->prev_empty= 0;
    }
  }
  if (generator->old_version.elements > 0)
  {
    if (generator->mariadbd_group_end < 0 || !generator->prev_empty)
      fputc('\n', target_file);
    fprintf(target_file, "[%s]\n", opt_error_group);
    for (i= 0; i < generator->old_version.elements; i++)
    {
      char *element= *(char **)dynamic_array_ptr(&generator->old_version, i);
      fputs(element, target_file);
    }
    generator->prev_empty= 0;
  }

  if (generator->unsupported_version.elements > 0)
  {
    if (generator->mariadbd_group_end < 0 || !generator->prev_empty)
      fputc('\n', target_file);
    fprintf(target_file, "[%s]\n", opt_unsupported_group);
    for (i= 0; i < generator->unsupported_version.elements; i++)
    {
      char *element= (*(char **)
                      dynamic_array_ptr(&generator->unsupported_version, i));
      fputs(element, target_file);
    }
    generator->prev_empty= 0;
  }

  global_update_count+= (lines_handled + generator->old_version.elements +
                         generator->unsupported_version.elements);
}


static void generator_count_lines(struct generator *generator)
{
  int mariadbd_additions;

  delete_dup_data_from_mariadbd_group(generator);
  mariadbd_additions= count_array_elements(&generator->mariadbd_additions);
  global_update_count+= (mariadbd_additions +
                         generator->old_version.elements +
                         generator->unsupported_version.elements);

  if (!opt_print && opt_edit_mode == EDIT_MODE_NONE)
  {
    if (mariadbd_additions)
      fprintf(stdout, "%d line/lines would be copyed to [mariadbd] group\n",
              mariadbd_additions);
    if (generator->old_version.elements > 0)
      fprintf(stdout, "%d line/lines would be copyed to [%s] group\n",
              (int) generator->old_version.elements,
              opt_error_group);
    if (generator->unsupported_version.elements > 0)
      fprintf(stdout, "%d line/lines would be copyed to [%s] group\n",
              (int) generator->old_version.elements,
              opt_unsupported_group);
  }
}


static void generator_free(struct generator *generator)
{
  free_root(&generator->alloc, MYF(0));
  delete_dynamic(&generator->main);
  delete_dynamic(&generator->old_version);
  delete_dynamic(&generator->unsupported_version);
  delete_dynamic(&generator->mariadbd_additions);
  my_free(generator->output_group);
}

static char *print_alloc(MEM_ROOT *alloc, const char *format, va_list args)
{
  int length;
  char *p= NULL;
  va_list args2;
  va_copy(args2, args);

  length= vsnprintf(p, 0, format, args) + 1;
  p= alloc_root(alloc, length);
  if (!p)
    return NULL;
  vsnprintf(p, length, format, args2);
  va_end(args2);
  return p;
}

static my_bool vadd_line(MEM_ROOT *alloc, DYNAMIC_ARRAY *array,
                        const char *format, va_list args)
{
  char *p;
  p= print_alloc(alloc, format, args);
  if (!p)
    return TRUE;
  return insert_dynamic(array, &p);
}

static my_bool add_line(MEM_ROOT *alloc, DYNAMIC_ARRAY *array,
                        const char *format, ...)
{
  va_list args;
  my_bool res;
  va_start(args, format);
  res= vadd_line(alloc, array, format, args);
  va_end(args);
  return res;
}

/*
  Delete elements from array if it already exits.

  Delete is done by setting pointer to string element to 0;
*/

static my_bool delete_line(DYNAMIC_ARRAY *array, char *line)
{
  for (uint i= 0; i < array->elements ; i++)
  {
    char *element= *dynamic_element(array, i, char**);
    if (element && !strcmp(element, line))
    {
      *((char**) dynamic_array_ptr(array, i))= NullS;
      return 1;
    }
  }
  return 0;
}


/* Count not null elements in the array */

static uint count_array_elements(DYNAMIC_ARRAY *array)
{
  uint count= 0;
  for (uint i= 0; i < array->elements ; i++)
    if (*dynamic_element(array, i, char**))
      count++;
  return count;
}

enum line_type
{
  LINE_TYPE_EMPTY,
  LINE_TYPE_OPTION,
  LINE_TYPE_OTHER,
};

static my_bool add_main_line(struct generator *generator, enum line_type type,
                             const char *format, ...)
{
  va_list args;
  my_bool res;

  /* generator->input_group is empty for comments at top of include files */
  if (type != LINE_TYPE_EMPTY && generator->input_group &&
      (!generator->output_group ||
       strcmp(generator->input_group, generator->output_group)))
  {
    char *group_dupe;
    if (add_line(&generator->alloc, &generator->main,
                 generator->prev_empty ? "[%s]\n" : "\n[%s]\n",
                 generator->input_group))
      return TRUE;
    group_dupe=
        my_strdup(key_memory_upgrade_config, generator->input_group, MYF(0));
    if (!group_dupe)
      return TRUE;
    my_free(generator->output_group);
    generator->output_group= group_dupe;
    generator->start_of_group= (int) generator->main.elements;
  }
  va_start(args, format);
  res= vadd_line(&generator->alloc, &generator->main, format, args);
  va_end(args);
  if (!res)
  {
    if (type == LINE_TYPE_OPTION &&
        !strcmp(generator->input_group, "mariadbd"))
    {
      generator->mariadbd_group_start= generator->start_of_group;
      generator->mariadbd_group_end=   (int) generator->main.elements;
    }

    generator->prev_empty= type == LINE_TYPE_EMPTY;
  }
  return res;
}


/*
  Change file paths from /mysql/ to /mariadbd/

  If string contains 'from' return allocated string otherwise return
  NullS
*/

char *change_file_path(const char *line, LEX_CSTRING *from, LEX_CSTRING *to)
{
  const char *pos;

  for (pos= line ; (pos= strstr(pos, from->str)); pos++)
  {
    if (pos != line && (pos[-1] == '/' || pos[-1] == '\\') &&
        pos[from->length] == pos[-1])
    {
      /* Found path, change 'from' to 'to' */
      char *path= my_malloc(key_memory_upgrade_config,
                            strlen(line) - from->length + to->length + 1,
                            MYF(MY_WME));
      if (!path)
        break;
      memcpy(path, line, (size_t) (pos - line));
      strxmov(path + (pos - line), to->str, pos + from->length, NullS);
      return path;
    }
  }
  return NullS;
}


/**
  Write the given line to generator output as specified by opt_edit_mode.
  Returns false on success and true on failure.
*/
static my_bool generator_add_line(struct generator *generator, const char *line,
                                  enum line_type type, my_bool is_valid)
{
  if (copy_code_to_mariadb && is_valid && type != LINE_TYPE_EMPTY &&
      (opt_edit_mode == EDIT_MODE_LAST || opt_edit_mode == EDIT_MODE_INLINE))
  {
    /* Copy lines from [mysqld-XXX] group to  [mariadbd] */
    const char *pos= line, *new_line= 0;
    if (opt_update_paths &&
        (new_line= change_file_path(line, &mysql_name, &mariadb_name)))
      pos= new_line;
    add_line(&generator->alloc, &generator->mariadbd_additions, "%s", pos);
    my_free((char*) new_line);
  }
  if (copy_code_to_mariadb)
    is_valid= 1;                /* Print line to [mysqld-XXX] groups */

  switch (opt_edit_mode) {
  case EDIT_MODE_REMOVE:
    global_update_count+= MY_TEST(!is_valid);
    return is_valid ? add_main_line(generator, type, "%s", line) : FALSE;
  case EDIT_MODE_LAST:
    if (is_valid || type == LINE_TYPE_EMPTY)
      return add_main_line(generator, type, "%s", line);
    if (strcmp(generator->input_group, "mariadbd"))
      return add_line(&generator->alloc, &generator->old_version, "%s", line);
    return add_line(&generator->alloc, &generator->unsupported_version, "%s",
                    line);
  case EDIT_MODE_COMMENT:
    global_update_count+= MY_TEST(!is_valid);
    return add_main_line(generator, type, is_valid ? "%s" : "#%s", line);
  case EDIT_MODE_INLINE:
  {
    const char *old_input_group= generator->input_group;
    my_bool res= TRUE;
    global_update_count+= MY_TEST(!is_valid);
    if (!is_valid)
      generator->input_group= (strcmp(generator->input_group, "mariadbd") ?
                               opt_error_group :
                               opt_unsupported_group);
    if (!add_main_line(generator, is_valid ? type : LINE_TYPE_OTHER, "%s",
                       line))
      res= FALSE;
    generator->input_group= old_input_group;
    return res;
  }
  case EDIT_MODE_NONE:
    return opt_print ? add_main_line(generator, type, "%s", line) : 0;
  }
  return TRUE;
}


static int process_default_file_with_ext(struct upgrade_ctx *ctx,
                                        const char *dir, const char *ext,
                                        const char *config_file,
                                        int recursion_level)
{
  char name[FN_REFLEN + 10], buff[4096], curr_gr[4096], *ptr, *end, **tmp_ext;
  char tmp_name[FN_REFLEN + 30], restored_name[FN_REFLEN + 30];
  char option_buff[4096+2], conv_option[128], tmp[FN_REFLEN];
  char *value, *option, *option_value_start, *new_line;
  static const char includedir_keyword[]= "includedir";
  static const char include_keyword[]= "include";
  const int max_recursion_level= 10;
  MYSQL_FILE *fp;
  MYSQL_FILE *tmp_fp= NULL;
  my_bool file_valid= TRUE;
  uint line=0, option_start;
  enum { NONE, PARSE, SKIP } found_group= NONE;
  my_bool skip_slave_start_present= FALSE, generator_inited= 0;
  size_t i;
  MY_DIR *search_dir;
  FILEINFO *search_file;
  struct generator generator;

  if (safe_strlen(dir) + strlen(config_file) >= FN_REFLEN-3)
    return 0;					/* Ignore wrong paths */
  if (dir)
  {
    end=convert_dirname(name, dir, NullS);
    if (dir[0] == FN_HOMELIB)		/* Add . to filenames in home */
      *end++='.';
    strxmov(end,config_file,ext,NullS);
  }
  else
  {
    strmov(name,config_file);
  }
  fn_format(name,name,"","",4);
#if !defined(_WIN32)
  {
    MY_STAT stat_info;
    if (!my_stat(name,&stat_info,
                 MYF(give_error_for_missing_files ? MY_WME : 0)))
      return 1;
    /*
      Ignore world-writable regular files (exceptions apply).
      This is mainly done to protect us to not read a file that may be
      modified by anyone.

      Also check access so that read only mounted (EROFS)
      or immutable files (EPERM) that are suitable protections.

      The main case we are allowing is a container readonly volume mount
      from a filesystem that doesn't have unix permissions. This will
      have a 0777 permission and access will set errno = EROFS.

      Note if a ROFS has a file with permissions 04n6, access sets errno
      EACCESS, rather the ROFS, so in this case we'll error, even though
      the ROFS is protecting the file.

      An ideal, race free, implementation would do fstat / fstatvfs / ioctl
      for permission, read only filesystem, and immutability resprectively.
    */
    if ((stat_info.st_mode & S_IWOTH) &&
	(stat_info.st_mode & S_IFMT) == S_IFREG &&
	(access(name, W_OK) == 0 || (errno != EROFS && errno != EPERM)))
    {
      fprintf(stderr, "Warning: World-writable config file '%s' is ignored\n",
              name);
      return 0;
    }
  }
#endif
  if (!(fp= mysql_file_fopen(key_file_cnf, name, O_RDONLY, MYF(MY_WME))))
    return 1;					/* Ignore wrong files */

  if (opt_update)
  {
    strmov(strmov(tmp_name, name), "-upgrade-config");
    strmov(strmov(restored_name, name), "-upgrade-config-orig");
    if (!(tmp_fp= mysql_file_fopen(key_file_cnf, tmp_name,
                                   O_RDWR | O_CREAT | O_TRUNC, MYF(0))))
    {
      fprintf(stderr, "error: Failed to open %s for writing: %s\n", tmp_name,
              strerror(errno));
      goto err;
    }
  }

  generator_init(&generator);
  generator_inited= 1;
  if (opt_print || (opt_edit_mode != EDIT_MODE_NONE && !opt_update))
    fprintf(stdout, "### File %s:\n", name);
  while (mysql_file_fgets(buff, sizeof(buff) - 3, fp))
  {
    my_bool line_valid= TRUE;
    int invalid_set_index;
    enum plugin_check_result plugin_check_result;
    line++;
    /* Ignore comment and empty lines */
    for (ptr= buff; my_isspace(&my_charset_latin1, *ptr); ptr++)
    {}

    /* Ensure line ends with \n (delete_line code depends one this) */
    end= strend(ptr);
    if (end != ptr && end[-1] != '\n')
    {
      end[0]= '\n';
      end[1]= 0;
    }

    if (*ptr == '#' || *ptr == ';' || !*ptr)
    {
      generator_add_line(&generator, buff,
                         *ptr ? LINE_TYPE_OTHER : LINE_TYPE_EMPTY, line_valid);
      continue;
    }

    /* Configuration File Directives */
    if (*ptr == '!' || *ptr == '?')
    {
      generator_add_line(&generator, buff, LINE_TYPE_OTHER, line_valid);
      if (recursion_level >= max_recursion_level)
      {
        for (end= ptr + strlen(ptr) - 1;
             my_isspace(&my_charset_latin1, *(end - 1));
             end--)
        {}
        end[0]= 0;
        fprintf(stderr,
                "Warning: skipping '%s' directive as maximum include"
                "recursion level was reached in file %s at line %d\n",
                ptr, name, line);
        continue;
      }

      /* skip over `!' and following whitespace */
      for (++ptr; my_isspace(&my_charset_latin1, ptr[0]); ptr++)
      {}

      if ((!strncmp(ptr, includedir_keyword,
                    sizeof(includedir_keyword) - 1)) &&
          my_isspace(&my_charset_latin1, ptr[sizeof(includedir_keyword) - 1]))
      {
        if (!(ptr= get_argument(includedir_keyword,
                                sizeof(includedir_keyword),
                                ptr, name, line)))
          goto err;

        if (!(search_dir= my_dir(ptr, MYF(MY_WME | MY_WANT_SORT))))
          goto err;

        for (i= 0; i < search_dir->number_of_files; i++)
        {
          search_file= search_dir->dir_entry + i;
          ext= fn_ext2(search_file->name);

          /* check extension */
          for (tmp_ext= (char**) f_extensions; *tmp_ext; tmp_ext++)
          {
            if (!strcmp(ext, *tmp_ext))
              break;
          }

          if (*tmp_ext)
          {
            fn_format(tmp, search_file->name, ptr, "",
                      MY_UNPACK_FILENAME | MY_SAFE_PATH);

            process_default_file_with_ext(ctx, "", "", tmp, recursion_level + 1);
          }
        }

        my_dirend(search_dir);
      }
      else if ((!strncmp(ptr, include_keyword, sizeof(include_keyword) - 1)) &&
               my_isspace(&my_charset_latin1, ptr[sizeof(include_keyword)-1]))
      {
	if (!(ptr= get_argument(include_keyword,
                                sizeof(include_keyword), ptr,
                                name, line)))
	  goto err;

        process_default_file_with_ext(ctx, "", "", ptr, recursion_level + 1);
      }
      continue;
    }

    if (*ptr == '[')				/* Group name */
    {
      if (!(end=(char *) strchr(++ptr,']')))
      {
	fprintf(stderr,
		"error: Wrong group definition in config file: %s at line %d\n",
		name,line);
	goto err;
      }
      /* Remove end space */
      for ( ; my_isspace(&my_charset_latin1,end[-1]) ; end--) ;
      end[0]=0;

      strmake(curr_gr, ptr, MY_MIN((size_t) (end-ptr), sizeof(curr_gr)-1));
      copy_code_to_mariadb= 0;
      found_group= find_type(curr_gr, ctx->group, FIND_TYPE_NO_PREFIX)
                   ? PARSE : SKIP;
      /* Ensure we process all groups with the mysqld prefix */
      if (found_group == SKIP)
      {
        if (!strncmp(curr_gr, "mysqld-", 7))
        {
          found_group= PARSE;
          /* Copy all valid code from the mysqld- group to the mariadbd group */
          copy_code_to_mariadb= 1;
        }
      }

      generator.input_group= curr_gr;
      if (found_group == PARSE && !strcmp(curr_gr, "mariadbd"))
      {
        /**
          Force the mariadbd group to be emitted to allow potentially adding new
          lines to it.
         */
        generator_add_line(&generator, "", LINE_TYPE_OPTION, TRUE);
      }
      continue;
    }
    switch (found_group) {
    case NONE:
      fprintf(stderr,
	      "error: Found option without preceding group in config file: %s at "
              "line: %d\n",
	      name,line);
      goto err;
    case PARSE:
      if (opt_print && opt_edit_mode == EDIT_MODE_NONE)
      {
        generator_add_line(&generator, buff, LINE_TYPE_OPTION, TRUE);
        continue;
      }
      break;
    case SKIP:
      if (!strcmp(curr_gr, "mariadbd") &&
          (opt_edit_mode == EDIT_MODE_INLINE ||
           opt_edit_mode == EDIT_MODE_LAST))
      {
        /* Remove lines with the /mysql/ path */
        char *pos= change_file_path(buff, &mariadb_name, &mysql_name);
        if (pos)
        {
          delete_line(&generator.mariadbd_additions, pos);
          my_free(pos);
        }
        delete_line(&generator.mariadbd_additions, buff);
      }
      generator_add_line(&generator, buff, LINE_TYPE_OPTION, line_valid);
      continue;
    }

    end= remove_end_comment(ptr);
    if ((value= strchr(ptr, '=')))
      end= value;
    for ( ; my_isspace(&my_charset_latin1,end[-1]) ; end--)
      ;
    ptr= strmake(option_buff, ptr, (size_t) (end-ptr));

    /* Change '-' to '_' in the option */
    {
      char *from, *from_end, *to;
      from_end= option_buff + sizeof(conv_option)-1;
      for (from= option_buff, to= conv_option;
           *from && from < from_end;
           from++, to++)
        *to= (*from == '-') ? '_' : *from;
      *to= 0;
      option_start= 0;

      /* Ignore some prefix */
      for (uint i= 0; special_opt_prefix[i] ; i++)
      {
        if (!strncmp(conv_option, special_opt_prefix[i],
                     special_opt_prefix_lengths[i]) &&
            conv_option[special_opt_prefix_lengths[i]] == '_' &&
            conv_option[special_opt_prefix_lengths[i]+1])
        {
          option_start= special_opt_prefix_lengths[i]+1;
          break;
        }
      }
    }
    option= conv_option + option_start;

    if (value)
    {
      /* Remove pre- and end space */
      char *value_end;
      for (value++ ; my_isspace(&my_charset_latin1,*value); value++) ;
      value_end=strend(value);
      /*
	We don't have to test for value_end >= value as we know there is
	an '=' before
      */
      for ( ; my_isspace(&my_charset_latin1,value_end[-1]) ; value_end--) ;
      if (value_end < value)			/* Empty string */
	value_end=value;

      /* remove quotes around argument */
      if ((*value == '\"' || *value == '\'') && /* First char is quote */
          (value + 1 < value_end ) && /* String is longer than 1 */
          *value == value_end[-1] ) /* First char is equal to last char */
      {
	value++;
	value_end--;
      }
      *ptr++= 0;
      option_value_start= ptr;

      if (!mariadbd_option_exists(option))
      {
        line_valid= FALSE;
        file_valid= FALSE;

        if (copy_code_to_mariadb)               /* [mysqld-#] group */
        {
          generator_add_line(&generator, buff, LINE_TYPE_OPTION, FALSE);
          continue;
        }
        if (!opt_print && opt_edit_mode == EDIT_MODE_NONE)
        {
          fprintf(stdout, "In %s at line %d: Invalid option %s\n", name, line,
                  option);
          ctx->failed= 1;
          global_update_count++;
          continue;
        }
      }
      for ( ; value != value_end; value++)
      {
	if (*value == '\\' && value != value_end-1)
	{
	  switch(*++value) {
	  case 'n':
	    *ptr++='\n';
	    break;
	  case 't':
	    *ptr++= '\t';
	    break;
	  case 'r':
	    *ptr++ = '\r';
	    break;
	  case 'b':
	    *ptr++ = '\b';
	    break;
	  case 's':
	    *ptr++= ' ';			/* space */
	    break;
	  case '\"':
	    *ptr++= '\"';
	    break;
	  case '\'':
	    *ptr++= '\'';
	    break;
	  case '\\':
	    *ptr++= '\\';
	    break;
	  default:				/* Unknown; Keep '\' */
	    *ptr++= '\\';
	    *ptr++= *value;
	    break;
	  }
	}
	else
	  *ptr++= *value;
      }
      *ptr=0;
      if (!mariadbd_valid_enum_value(option, option_value_start))
      {
        line_valid= FALSE;
        file_valid= FALSE;
        if (!opt_print && opt_edit_mode == EDIT_MODE_NONE)
        {
          fprintf(stdout,
                  "In %s at line %d: Invalid enum value '%s' for option %s\n",
                  name, line, option_value_start, option);
          ctx->failed= 1;
          continue;
        }
      }
      else if ((invalid_set_index=
                mariadbd_check_set_value(option, option_value_start)))
      {
        line_valid= FALSE;
        file_valid= FALSE;
        if (!opt_print && opt_edit_mode == EDIT_MODE_NONE)
        {
          fprintf(stdout, "In %s at line %d: Invalid set value '%s' at "
                  "index %d for option %s\n",
                  name, line, option_value_start, invalid_set_index, option);
          ctx->failed= 1;
          continue;
        }
      }
      else if ((plugin_check_result= check_plugins(option, option_value_start)))
      {
        line_valid= FALSE;
        file_valid= FALSE;
        if (!opt_print && opt_edit_mode == EDIT_MODE_NONE)
        {
          switch (plugin_check_result) {
          case PLUGINS_OK: break;
          case AUDIT_PLUGIN:
            fprintf(stdout, "In %s at line %d: Please replace audit_log with "
                    "the server_audit plugin\n", name, line);
            ctx->failed= 1;
            continue;
          }
        }
      }
      else if ((!strcmp(curr_gr, "mysqld") && line_valid &&
                opt_update_paths &&
                (opt_edit_mode == EDIT_MODE_INLINE ||
                 opt_edit_mode == EDIT_MODE_LAST)) &&
               (new_line= change_file_path(buff,
                                           &mysql_name, &mariadb_name)))
      {
        if (!opt_print && opt_edit_mode == EDIT_MODE_NONE)
        {
          fprintf(stdout, "In %s at line %d: '%.*s' would be "
                    "replaced with '%.*s'\n", name, line,
                  (int) strlen(buff)-1, buff,
                  (int) strlen(new_line)-1, new_line);
          ctx->failed= 1;
        }

        generator_add_line(&generator, buff, LINE_TYPE_OPTION, FALSE);
        add_line(&generator.alloc, &generator.mariadbd_additions, "%s",
                 new_line);
        my_free(new_line);
        continue;
      }
      else if (strcmp(curr_gr, "mariadbd") &&
               !strcmp(option, "key_buffer_size") &&
               ((!opt_print && opt_edit_mode == EDIT_MODE_NONE) ||
                opt_edit_mode == EDIT_MODE_INLINE ||
                opt_edit_mode == EDIT_MODE_LAST))
      {
        file_valid= FALSE;
        if (opt_edit_mode == EDIT_MODE_NONE)
        {
          fprintf(stdout, "In %s at line %d: key_buffer_size needs "
                  "modifications\n", name, line);
          global_update_count++;
          continue;
        }
        generator_add_line(&generator, buff, LINE_TYPE_OPTION, FALSE);
        if (opt_no_myisam_options)
        {
          add_line(&generator.alloc, &generator.mariadbd_additions,
                   "key_buffer_size=64K    # Recommended value if no "
                   "MyISAM tables\n");
          snprintf(conv_option, sizeof(conv_option)-1,
                   "aria_pagecache_buffer_size=%s\n",
                   option_value_start);
          add_line(&generator.alloc, &generator.mariadbd_additions,
                   conv_option);
        }
        else
        {
          add_line(&generator.alloc, &generator.mariadbd_additions, "%s", buff);
          add_line(&generator.alloc, &generator.mariadbd_additions,
                   "#key-buffer-size=64K   # Recommended value if no MyISAM tables\n");
        }
        continue;
      }
    } /* !value */
    else if (!strcmp(option, "skip_slave_start"))
    {
      skip_slave_start_present= TRUE;
    }
    else if (!mariadbd_option_exists(option))
    {
      line_valid= FALSE;
      file_valid= FALSE;
      if (!opt_print && opt_edit_mode == EDIT_MODE_NONE &&
          !copy_code_to_mariadb)
      {
        fprintf(stdout, "In %s at line %d: Invalid option %s\n", name, line,
                option);
        ctx->failed= 1;
        global_update_count++;
        continue;
      }
    }
    generator_add_line(&generator, buff, LINE_TYPE_OPTION, line_valid);
  }
  mysql_file_fclose(fp, MYF(0));
  fp= 0;
  if (opt_add_skip_slave_start && !skip_slave_start_present)
  {
    file_valid= FALSE;
    add_line(&generator.alloc, &generator.mariadbd_additions,
             "skip_slave_start\n");
  }
  if (opt_print || opt_update)
    generator_write(tmp_fp, &generator);
  else
    generator_count_lines(&generator);

  generator_free(&generator);
  if (tmp_fp)
  {
    mysql_file_fclose(tmp_fp, MYF(0));
    tmp_fp= 0;
    if (file_valid)
    {
      my_delete(tmp_name, MYF(0));
    }
    else
    {
      myf redel_flags = opt_backup ? MYF(MY_REDEL_MAKE_BACKUP) : MYF(0);
      char *duped_name= my_strdup(key_memory_upgrade_config, name, MYF(0));
      if (!duped_name || insert_dynamic(&ctx->updated_files, &duped_name))
        goto err;
      /*
        Copy the file in case running mariadbd fails and the update must be
        reverted.
      */
      if (my_copy(name, restored_name, MYF(0)))
      {
        fprintf(stderr, "error: Failed to copy %s to %s: %s",
                name, restored_name, strerror(errno));
        return -1;
      }
      if (my_redel(name, tmp_name, time(NULL), redel_flags))
      {
        fprintf(stderr, "error: Failed to rename %s to %s: %s",
                tmp_name, name, strerror(errno));
        return -1;
      }
    }
  }
  if (opt_print || (opt_edit_mode != EDIT_MODE_NONE && !opt_update))
    fputc('\n', stdout);
  return(0);

 err:
  if (fp)
    mysql_file_fclose(fp, MYF(0));
  if (tmp_fp)
    mysql_file_fclose(tmp_fp, MYF(0));
  if (generator_inited)
    generator_free(&generator);
  return -1;					/* Fatal error */
}


static int process_default_file(struct upgrade_ctx *ctx,
                               const char *dir,
                               const char *config_file)
{
  char **ext;
  const char *empty_list[]= { "", 0 };
  my_bool have_ext= fn_ext(config_file)[0] != 0;
  const char **exts_to_use= have_ext ? empty_list : f_extensions;

  for (ext= (char**) exts_to_use; *ext; ext++)
  {
    int error;
    if ((error= process_default_file_with_ext(ctx, dir, *ext, config_file, 0)) < 0)
      return error;
  }
  return 0;
}


static int process_option_files(const char *conf_file,
                                struct upgrade_ctx *ctx,
                                const char **default_directories)
{
  const char **dirs;
  int error= 0;

  if (my_defaults_group_suffix)
  {
    /* Handle --defaults-group-suffix= */
    uint i;
    const char **extra_groups;
    const size_t instance_len= strlen(my_defaults_group_suffix);
    char *ptr;
    TYPELIB *group= ctx->group;

    if (!(extra_groups=
	  (const char**)alloc_root(ctx->alloc,
                                   (2*group->count+1)*sizeof(char*))))
      return 2;

    for (i= 0; i < group->count; i++)
    {
      size_t len;
      extra_groups[i]= group->type_names[i]; /** copy group */

      len= strlen(extra_groups[i]);
      if (!(ptr= alloc_root(ctx->alloc, (uint) (len+instance_len+1))))
       return 2;

      extra_groups[i+group->count]= ptr;

      /** Construct new group */
      memcpy(ptr, extra_groups[i], len);
      memcpy(ptr+len, my_defaults_group_suffix, instance_len+1);
    }

    group->count*= 2;
    group->type_names= extra_groups;
    group->type_names[group->count]= 0;
  }

  if (my_defaults_file)
  {
    if ((error= process_default_file_with_ext(ctx, "", "",
                                             my_defaults_file, 0)) < 0)
      goto err;
    if (error > 0)
    {
      fprintf(stderr, "Could not open required defaults file: %s\n",
              my_defaults_file);
      goto err;
    }
  }
  else if (dirname_length(conf_file) || !default_directories)
  {
    if ((error= process_default_file(ctx, NullS, conf_file)) < 0)
      goto err;
  }
  else
  {
    for (dirs= default_directories ; *dirs; dirs++)
    {
      if (**dirs)
      {
	if (process_default_file(ctx, *dirs, conf_file) < 0)
	  goto err;
      }
      else if (my_defaults_extra_file)
      {
        if ((error= process_default_file_with_ext(ctx, "", "",
                                                my_defaults_extra_file, 0)) < 0)
	  goto err;				/* Fatal error */
        if (error > 0)
        {
          fprintf(stderr, "Could not open required defaults file: %s\n",
                  my_defaults_extra_file);
          goto err;
        }
      }
    }
  }

  return 0;

err:
  fprintf(stderr,"Fatal error in defaults handling. Program aborted\n");
  return 1;
}


/*
  Process defaults

  @args default_args        --default options. Sent forward to mariadbd
  @args default_args_count  Number of default options

  @return 0 ok
  @return 1 general error
  @return 2 memory allocation error
*/

static int process_defaults(const char *conf_file,
                            const char **groups,
                            const char **dirs,
                            const char **defaults_args,
                            int defaults_args_count)
{
  int error= 0;
  MEM_ROOT alloc;
  TYPELIB group;
  struct upgrade_ctx ctx;
  char executable[FN_REFLEN];

  init_alloc_root(key_memory_upgrade_config, &alloc, 512, 0, MYF(0));

  group.count= 0;
  group.name= "defaults";
  group.type_names= groups;

  for (; *groups ; groups++)
    group.count++;

  ctx.alloc= &alloc;
  ctx.group= &group;
  init_dynamic_array2(key_memory_upgrade_config,
                      &ctx.updated_files,
                      sizeof(char *),
                      NULL,
                      0,
                      0,
                      MYF(0));
  ctx.failed= 0;

  if ((error= process_option_files(conf_file, &ctx, dirs)))
  {
    finish_updated_files(&ctx, FALSE);
    free_root(&alloc,MYF(0));
    return error;
  }

  if (ctx.updated_files.elements > 0 && opt_mariadbd_testing)
  {
    DYNAMIC_STRING mariadbd_output;
    const char *mariadbd_name= IF_WIN("mariadbd.exe", "mariadbd");
    const char *used_name= executable;

    if (opt_basedir[0])
      fn_format(executable, mariadbd_name, opt_basedir, "", MYF(0));
    else
      error= find_file_in_path(executable, mariadbd_name) != 0;
    if (!error)
      error= access(executable, X_OK);
    else
      used_name= mariadbd_name;

    if (error)
    {
      fprintf(stderr,
              "Cannot use %s for execution to test if the new option files "
              "works (error %d)\n"
              "Try specifiying the directory for %s with --basedir=...\n",
              used_name, (int) errno, mariadbd_name);
      finish_updated_files(&ctx, 1);
      return errno;
    }

    if (init_dynamic_string(&mariadbd_output, "", 32, 32))
      goto err;
    if (!test_mariadbd(mariadbd_name) &&
        (error= run_mariadbd(mariadbd_name, &mariadbd_output, defaults_args,
                             defaults_args_count)))
    {
      fputs("error: Failed to run mariadbd with the updated files, reverting\n",
            stderr);
      if (error > 0)
        fprintf(stderr, "mariadbd output:\n%s", mariadbd_output.str);
    }
    dynstr_free(&mariadbd_output);
  }

  error= finish_updated_files(&ctx, error == 0);
  free_root(&alloc, MYF(0));
  return error || ctx.failed;

err:
  finish_updated_files(&ctx, FALSE);
  free_root(&alloc, MYF(0));
  return 1;
}


static const char *config_file="my";			/* Default config file */

enum upgrade_config_options
{
  OPT_UPDATE = 256,
  OPT_BACKUP,
  OPT_ERROR_GROUP,
  OPT_UNSUPPORTED_GROUP,
  OPT_EDIT,
  OPT_PRINT,
  OPT_NO_MYISAM_OPTIONS,
  OPT_ADD_SKIP_SLAVE_START,
  OPT_FIX_ALL,
  OPT_CONVERT_FROM,
  OPT_BASENAME,
  OPT_UPDATE_PATHS,
  OPT_MARIADBD_TESTING
};

static struct my_option my_long_options[] =
{
  {"help", '?', "Display this help message and exit.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Output version information and exit.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"update", OPT_UPDATE, "Update the configuration files in place.",
   &opt_update, &opt_update, 0, GET_BOOL, NO_ARG, FALSE, 0, 0, 0, 0, 0},
  {"backup", OPT_BACKUP,
   "Backup the updated configuration files. The backup file names end in a "
   "timestamp followed by .BAK.",
   &opt_backup, &opt_backup, 0, GET_BOOL, NO_ARG, FALSE, 0, 0, 0, 0, 0},
  {"basedir", OPT_BASENAME,
   "Directory where the mariadbd server resides.",
   &opt_basedir, &opt_basedir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"update-paths", OPT_UPDATE_PATHS,
   "When used with --from=mysql, change file paths having /mysql/ in their "
   "name to /mariadbd/.",
   &opt_update_paths, &opt_update_paths,
   0, GET_BOOL, NO_ARG, FALSE, 0, 0, 0, 0, 0},
  {"error-group", OPT_ERROR_GROUP,
   "Group to use for options not supported by MariaDB when using "
   "--edit=[inline|last]. Should be set usually to something like "
   "'mysqld-#.##' when moving from MySQL and 'unknown' when upgrading "
   "MariaDB or if there are already [mariadbd] groups in the config files.",
   &opt_error_group, &opt_error_group,
   0, GET_STR, REQUIRED_ARG, 0,  0, 0, 0, 0, 0},
  {"unsupported-group", OPT_UNSUPPORTED_GROUP,
   "Group to use for options in [mariadbd] group that is not supported by "
   "MariaDB when using '--edit=[inline|last] --from=mariadb.",
   &opt_unsupported_group, &opt_unsupported_group,
   0, GET_STR, REQUIRED_ARG, 0,  0, 0, 0, 0, 0},
  {"edit", OPT_EDIT,
   "Select what to do with invalid options. '' (default) no changes. "
   "'remove' removes them. 'comment' comments them with '#'. "
   "inline' adds [error_group] before the unknown option. "
   "'last' moves the incompatible version last to the given file "
   "under the [error_group] group.",
   &opt_edit_mode, &opt_edit_mode, &edit_mode_typelib, GET_ENUM,
   REQUIRED_ARG, EDIT_MODE_NONE, 0, 0, 0, 0, 0},
  {"from", OPT_CONVERT_FROM,
   "Specify if your are upgrading from MySQL or an earlier MariaDB version. "
   "When upgrading from MySQL, only the MySQL groups [mysqld] and [server] "
   "are processed. If MariaDB is specified, all MariaDB entries read by "
   "the current MariaDB version (" MYSQL_SERVER_VERSION ") will be processed. "
   "Note that when upgrading from an earlier MariaDB version, mariadb-upgrade "
   "is the preferred tool! This tool is in this case mainly useful to check if "
   "there is any incompatibilities in the MariaDB configuration files.",
   &opt_convert_from, &opt_convert_from, &from_mode_typelib, GET_ENUM,
   REQUIRED_ARG, FROM_MYSQL, 0, 0, 0, 0, 0},
  {"mariadbd-testing", OPT_MARIADBD_TESTING,
   "Verify updates by testing the changed config files through mariadbd",
   &opt_mariadbd_testing, &opt_mariadbd_testing, 0, GET_BOOL, NO_ARG, FALSE,
   0, 0, 0, 0, 0},
  {"print", OPT_PRINT, "Print upgraded files to stdout. The 'edit' option "
   "decides the output format.",
   &opt_print, &opt_print, 0, GET_BOOL, NO_ARG, FALSE, 0, 0, 0, 0, 0},
  {"silent", 's', "Print less information",
   &opt_silent, &opt_silent, 0, GET_BOOL, NO_ARG, FALSE, 0, 0, 0, 0, 0},
  {"no-myisam", OPT_NO_MYISAM_OPTIONS,
   "Assume no usage of MyISAM tables.  Makes the MyISAM buffers very small "
   "to save memory. "
   "Useful when moving to MariaDB when there are no MyISAM tables.",
   &opt_no_myisam_options, &opt_no_myisam_options, 0, GET_BOOL, NO_ARG,
   FALSE, 0, 0, 0, 0, 0},
  {"add-skip-slave-start", OPT_ADD_SKIP_SLAVE_START,
   "Add skip_slave_start to the [mariadbd] group if not present.",
   &opt_add_skip_slave_start, &opt_add_skip_slave_start,
   0, GET_BOOL, NO_ARG, FALSE, 0, 0, 0, 0, 0},
  {"fix-all", OPT_FIX_ALL,
   "Same as --no-myisam --from=mysql --add-skip-slave-start --edit=last",
   0, 0, 0, GET_NO_ARG, NO_ARG, FALSE, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

static void cleanup_and_exit(int exit_code) __attribute__ ((noreturn));
static void cleanup_and_exit(int exit_code)
{
  my_end(0);
  exit(exit_code);
}

static void pe(const char *example, const char *description)
{
  fprintf(stdout, "%s %s\n - %s.\n\n",
          my_progname + dirname_length(my_progname),
          example, description);
}


static void usage() __attribute__ ((noreturn));
static void usage()
{
  print_version();
  puts("This software comes with ABSOLUTELY NO WARRANTY. This is free "
       "software,\nand you are welcome to modify and redistribute it under "
       "the GPL license\n");
  puts("Display or change unrecognized MariaDB options in MariaDB/MySQL\n"
       "configuration files. This is useful when planning an upgrade from\n"
       "MySQL to MariaDB or to check if there are some unsupported MariaDB\n"
       "options in the option files..\n"
       "This program can print errors, print the changed files update the\n"
       "configuration files in place.\n"
       "When running with the options:\n"
       "--upgrade --edit=last --current-version=mysqld-5.7\n"
       "The configuration files will be changed so that they should work\n"
       "with both MySQL 5.7 and MariaDB. You should provide the currently\n"
       "used MySQL version if --current-version is used.\n");

  printf("Usage: %s [--print | --upgrade] [OPTIONS] [OPTION_FILES]\n\n",
         my_progname);
  puts("If OPTION_FILES is not provided, all default MariaDB configurtion\n"
       "files will be printed/upgraded");

  my_print_default_files(config_file);
  puts("");
  my_print_help(my_long_options);

  puts("Examples:");
  pe("", "Goes through default my.cnf files and prints not supported options. "
     "The only groups checked are [server], [mysqld], and [mysqld-###]");
  pe("--from=mariadb",
     "Goes through all read my.cnf files and prints options not supported by "
     "the current MariaDB version. All MariaDB groups read by mariadbd are "
     "checked");
  pe("~/.my.cnf", "Print not supported options in ~/.my.cnf");
  pe("--print", "Print the current content of all my.cnf files (no changes).");
  pe("--print --edit=last",
     "Print modifed version of all my.cnf files where supported options from "
     "[mysqld-XXX] groups are moved to a [mariadbd] group at the end of the "
     "file. Not supported options from [mysqld] and [server] are moved to "
     "[mysqld-5.7] (default value of 'error_group')");
  pe("--print --edit=inline --current-version=mysqld-8.0",
     "Print modifed version of all my.cnf files where supported options from "
     "[mysqld-XXX] groups are moved inline to [mariadbd] groups. Not supported "
     "options in the [mysqld] and [server] groups are moved to [mysqld-8.0]");
  pe("--print --remove",
     "Print modifed version of all my.cnf files where supported options from "
     "[mysqld-XXX] groups are moved to a [mariadbd] group. Not supported "
     "options from [mysqld] and [server] are removed");
  pe("--print --comment",
     "As '--remove', but not supported options are commented");
  pe("--upgrade --edit=inline --backup",
     "Modify all my.cnf files. Supported options from [mysqld-XXX] groups "
     "are moved inline to [mariadbd] groups. Not supported options in the "
     "[mysqld] and [server] groups are moved to a [mysqld-5.7] group. "
     "The original of the changed config files are stored as backup files. "
     "If no option files are specified the mariadbd server will be started "
     "using them to verify that the new options files works with it");
  pe("--update --edit=inline --error_group=unknown --from=mariadb",
     "Move all unknown mariadbd options to the group [unknown]");
  pe("--fix-all",
     "Use all common options when converting from MySQL to MariaDB. "
     "See definition for --fix-all for more details");

  my_print_variables(my_long_options);
  cleanup_and_exit(0);
}


static my_bool
get_one_option(const struct my_option *opt __attribute__((unused)),
	       const char *argument __attribute__((unused)),
               const char *filename __attribute__((unused)))
{
  switch (opt->id) {
  case 'I':
  case '?':
    usage();
  case 'V':
    print_version();
    cleanup_and_exit(0);
  case OPT_FIX_ALL:
    opt_no_myisam_options= TRUE;
    opt_add_skip_slave_start= TRUE;
    opt_edit_mode= EDIT_MODE_LAST;
    opt_convert_from= FROM_MYSQL;
    break;
  }
  return 0;
}


static int get_options(int *argc,char ***argv)
{
  int ho_error;

  if ((ho_error=handle_options(argc, argv, my_long_options, get_one_option)))
    cleanup_and_exit(ho_error);

  if (opt_update && opt_edit_mode == EDIT_MODE_NONE)
  {
    fputs("error: --update requires the '--edit=xxx' option\n", stderr);
    cleanup_and_exit(1);
  }
  if ((!opt_error_group || !opt_unsupported_group) &&
      (opt_edit_mode == EDIT_MODE_INLINE ||
       opt_edit_mode == EDIT_MODE_LAST))
  {
    fputs("error: Selected --edit mode requires --error-group and "
          "--unsupported-group\n", stderr);
    cleanup_and_exit(1);
  }
  if (opt_print && opt_update)
  {
    fputs("error: --print and --update can't be specified simultaneously\n",
          stderr);
    cleanup_and_exit(1);
  }
  if (!opt_print && !opt_update && opt_edit_mode != EDIT_MODE_NONE &&
      !opt_silent)
  {
    puts("Note: edit_mode used without print/update. --print assumed");
    opt_print= 1;
  }
  if (opt_print && opt_edit_mode == EDIT_MODE_NONE && opt_update_paths)
  {
    puts("Note: --print --update-paths without edit mode set will not "
         "show updated paths");
  }

  if (opt_update_paths && opt_convert_from != FROM_MYSQL)
    opt_update_paths= 0;

  groups_to_use= (opt_convert_from == FROM_MYSQL ?
                  mysqld_groups : mariadbd_groups);
  return 0;
}


int main(int argc, char **argv)
{
  int count, error= 0, args_used;
  char *tmp_arguments[4];
  char **arguments, **org_argv;
  const char **default_directories;
  MY_INIT(argv[0]);

  org_argv= argv;
  args_used= get_defaults_options(argv);

  /* Copy defaults-xxx arguments & program name */
  count= args_used;
  arguments= tmp_arguments;
  memcpy((char*) arguments, (char*) org_argv, count*sizeof(*org_argv));
  arguments[count]= 0;

  /*
     We already process --defaults* options at the beginning in
     get_defaults_options(). So skip --defaults* options and
     pass remaining options to handle_options().
  */
  org_argv+=args_used-1;
  argc-=args_used-1;

  /* Check out the args */
  if (get_options(&argc, &org_argv))
    cleanup_and_exit(1);

  if (argc)
  {
    give_error_for_missing_files= 1;
    /* We cannot test a list of provided option files */
    opt_mariadbd_testing= 0;
    for (;  argc-- ; org_argv++)
    {
      count= args_used;
      arguments= tmp_arguments;
      if ((error= my_load_defaults(*org_argv, groups_to_use,
                                   &count, &arguments, 0)))
        goto handle_error;
      if ((error= process_defaults(*org_argv, groups_to_use,
                                   0,
                                   (const char **)(argv + 1),
                                   args_used - 1)))
        goto handle_error2;
      free_defaults(arguments);
    }
  }
  else
  {
    if ((error= my_load_defaults(config_file, groups_to_use,
                                 &count, &arguments, &default_directories)))
      goto handle_error;
    /*
      The defaults-xxx arguments are still intact at the beginning of argv and
      can be passed to mariadbd for testing the updated configuration files.
    */
    if ((error= process_defaults(config_file, groups_to_use,
                                 default_directories,
                                 (const char **)(argv + 1),
                                 args_used - 1)))
      goto handle_error2;
    free_defaults(arguments);
  }
  if (!opt_silent && !opt_print)
  {
    if (opt_update && global_update_count)
      fprintf(stdout, "%d issue/issues changed\n", (int) global_update_count);
    else if (global_update_count)
      fprintf(stdout, "%d issue/issues found\n", (int) global_update_count);
    else
      printf("No issues found\n");
  }

  cleanup_and_exit(error != 0);

handle_error2:
  free_defaults(arguments);
handle_error:
  DBUG_ASSERT(error);
  if (error == 4)                             /* --print-defaults */
  {
    free_defaults(arguments);
    cleanup_and_exit(0);
  }
  cleanup_and_exit(2);
}

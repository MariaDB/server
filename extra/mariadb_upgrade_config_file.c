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

#define VER "0.1"
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

#define load_default_groups mysqld_groups
#include <mysqld_default_groups.h>
#undef load_default_groups

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
  TYPELIB *group;
  DYNAMIC_ARRAY updated_files;
  my_bool failed;
};


enum edit_mode
{
  EDIT_MODE_REMOVE,
  EDIT_MODE_COMMENT,
  EDIT_MODE_INLINE_OLD_VERSION,
  EDIT_MODE_LAST_OLD_VERSION,
  EDIT_MODE_NONE,
};
static ulong opt_edit_mode;
static const char *edit_mode_values[] = {"remove",
                                         "comment",
                                         "inline-old-version",
                                         "last-old-version",
                                         NullS};
static TYPELIB edit_mode_typelib = {array_elements(edit_mode_values),
                                    "", edit_mode_values, NULL};

static const char *opt_current_version;
static my_bool opt_update;
static my_bool opt_backup;
static my_bool opt_print;
static my_bool opt_no_myisam_files;
static my_bool opt_add_skip_slave_start;


static PSI_memory_key key_memory_upgrade_config;
static PSI_file_key key_file_cnf;


#ifdef _WIN32
static const char *f_extensions[]= { ".ini", ".cnf", 0 };
#else
static const char *f_extensions[]= { ".cnf", 0 };
#endif


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
           sizeof mariadbd_valid_options[0],
           compare_options) != NULL;
}


static my_bool mariadbd_valid_enum_value(const char *option, const char *value)
{
  const char **option_ptr= bsearch(&option, mariadbd_enum_options,
                                   array_elements(mariadbd_enum_options),
                                   sizeof mariadbd_enum_options[0],
                                   compare_options);
  if (!option_ptr)
    return TRUE;
  return find_type(value,
                   mariadbd_enum_typelibs[option_ptr - mariadbd_enum_options],
                   FIND_TYPE_BASIC) != 0;
}


/**
  Check whether the given value is a valid set value for the given option.
  Returns 0 on success and the first invalid set index on failure. If the option
  is not a set option, returns 0.
*/
static int mariadbd_check_set_value(const char *option, const char *value)
{
  const char **option_ptr= bsearch(&option, mariadbd_set_options,
                                   array_elements(mariadbd_set_options),
                                   sizeof mariadbd_set_options[0],
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

    if (!my_strcasecmp(&my_charset_latin1, value, "all"))
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
static enum plugin_check_result check_plugins(const char *option, const char *value)
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
  /* array of strings for options that should be added to the current version
   * section */
  DYNAMIC_ARRAY old_version;
  /* array of strings to be added to the mariadbd section */
  DYNAMIC_ARRAY mariadbd_additions;
  /* one past the last index of the original mariadbd section in the main array */
  int mariadbd_group_end;
  /* the current group from which options are read */
  const char *input_group;
  /* The current group in the generated output. This may be different from
   * input_group if lines are moved between groups. */
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

static void generator_write(MYSQL_FILE *f, struct generator *generator)
{
  FILE *target_file= f ? f->m_file : stdout;
  size_t i;
  if (generator->mariadbd_group_end >= 0)
  {
    for (i= 0; i < (size_t)generator->mariadbd_group_end; i++)
    {
      char *element= *(char **) dynamic_array_ptr(&generator->main, i);
      fputs(element, target_file);
    }
    for (i= 0; i < generator->mariadbd_additions.elements; i++)
    {
      char *element= *(char **) dynamic_array_ptr(&generator->mariadbd_additions, i);
      fputs(element, target_file);
    }
    for (i= (size_t)generator->mariadbd_group_end; i < generator->main.elements; i++)
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
    if (generator->mariadbd_additions.elements > 0)
    {
      if (!generator->prev_empty)
        fputc('\n', target_file);
      fputs("[mariadbd]\n", target_file);
      for (i= 0; i < generator->mariadbd_additions.elements; i++)
      {
        char *element= *(char **) dynamic_array_ptr(&generator->mariadbd_additions, i);
        fputs(element, target_file);
      }
    }
  }
  if (generator->old_version.elements > 0)
  {
    if (generator->mariadbd_group_end < 0 || !generator->prev_empty)
      fputc('\n', target_file);
    fprintf(target_file, "[%s]\n", opt_current_version);
    for (i= 0; i < generator->old_version.elements; i++)
    {
      char *element= *(char **)dynamic_array_ptr(&generator->old_version, i);
      fputs(element, target_file);
    }
  }
}

static void generator_free(struct generator *generator)
{
  free_root(&generator->alloc, MYF(0));
  delete_dynamic(&generator->main);
  delete_dynamic(&generator->old_version);
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
  if (type != LINE_TYPE_EMPTY &&
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
  }
  va_start(args, format);
  res= vadd_line(&generator->alloc, &generator->main, format, args);
  va_end(args);
  if (!res && type == LINE_TYPE_OPTION &&
      !strcmp(generator->input_group, "mariadbd"))
    generator->mariadbd_group_end= generator->main.elements;
  if (!res)
    generator->prev_empty= type == LINE_TYPE_EMPTY;
  return res;
}

/**
  Write the given line to generator output as specified by opt_edit_mode.
  Returns false on success and true on failure.
*/
static my_bool generator_add_line(struct generator *generator, const char *line,
                                  enum line_type type, my_bool is_valid)
{
  switch (opt_edit_mode) {
  case EDIT_MODE_REMOVE:
    return is_valid ? add_main_line(generator, type, "%s", line) : FALSE;
  case EDIT_MODE_LAST_OLD_VERSION:
    return is_valid || type == LINE_TYPE_EMPTY
               ? add_main_line(generator, type, "%s", line)
               : add_line(&generator->alloc, &generator->old_version, "%s",
                          line);
  case EDIT_MODE_COMMENT:
    return add_main_line(generator, type, is_valid ? "%s" : "#%s", line);
  case EDIT_MODE_INLINE_OLD_VERSION: {
    const char *old_input_group= generator->input_group;
    my_bool res= TRUE;
    if (!is_valid)
      generator->input_group= opt_current_version;
    if (add_main_line(generator, is_valid ? type : LINE_TYPE_OTHER, "%s", line))
      goto out;
    res= FALSE;
out:
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
  char *value, option[4096+2], tmp[FN_REFLEN];
  static const char includedir_keyword[]= "includedir";
  static const char include_keyword[]= "include";
  const int max_recursion_level= 10;
  MYSQL_FILE *fp;
  MYSQL_FILE *tmp_fp= NULL;
  my_bool file_valid= TRUE;
  uint line=0;
  enum { NONE, PARSE, SKIP } found_group= NONE;
  my_bool skip_slave_start_present= FALSE;
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
    if (!my_stat(name,&stat_info,MYF(0)))
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
  if (!(fp= mysql_file_fopen(key_file_cnf, name, O_RDONLY, MYF(0))))
    return 1;					/* Ignore wrong files */

  if (opt_update)
  {
    strmov(strmov(tmp_name, name), "-upgrade-config");
    strmov(strmov(restored_name, name), "-upgrade-config-orig");
    if (!(tmp_fp= mysql_file_fopen(key_file_cnf, tmp_name, O_RDWR | O_CREAT | O_TRUNC, MYF(0))))
    {
      fprintf(stderr, "error: Failed to open %s for writing: %s\n", tmp_name, strerror(errno));
      return -1;
    }
  }

  generator_init(&generator);
  if (opt_print || (opt_edit_mode != EDIT_MODE_NONE && !opt_update))
    fprintf(stdout, "### File %s:\n", name);
  while (mysql_file_fgets(buff, sizeof(buff) - 1, fp))
  {
    my_bool line_valid= TRUE;
    int invalid_set_index;
    enum plugin_check_result plugin_check_result;
    line++;
    /* Ignore comment and empty lines */
    for (ptr= buff; my_isspace(&my_charset_latin1, *ptr); ptr++)
    {}

    if (*ptr == '#' || *ptr == ';' || !*ptr)
    {
      generator_add_line(&generator, buff,
                         *ptr ? LINE_TYPE_OTHER : LINE_TYPE_EMPTY, line_valid);
      continue;
    }

    /* Configuration File Directives */
    if (*ptr == '!')
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
      found_group= find_type(curr_gr, ctx->group, FIND_TYPE_NO_PREFIX)
                   ? PARSE : SKIP;
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
    switch (found_group)
    {
    case NONE:
      fprintf(stderr,
	      "error: Found option without preceding group in config file: %s at line: %d\n",
	      name,line);
      goto err;
    case PARSE:
      break;
    case SKIP:
      generator_add_line(&generator, buff, LINE_TYPE_OPTION, line_valid);
      continue;
    }

    end= remove_end_comment(ptr);
    if ((value= strchr(ptr, '=')))
      end= value;
    for ( ; my_isspace(&my_charset_latin1,end[-1]) ; end--) ;
    ptr= strmake(option, ptr, (size_t) (end-ptr));
    if (value)
    {
      /* Remove pre- and end space */
      char *option_value_start;
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

      *ptr++ = 0;
      option_value_start = ptr;
      if (!mariadbd_option_exists(option)) {
        line_valid= FALSE;
        file_valid= FALSE;
        if (!opt_print && opt_edit_mode == EDIT_MODE_NONE)
        {
            fprintf(stdout, "In %s at line %d: Invalid option %s\n", name, line, option);
            ctx->failed= 1;
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
          fprintf(stdout, "In %s at line %d: Invalid enum value %s for option %s\n",
                  name, line, option_value_start, option);
          ctx->failed= 1;
          continue;
        }
      }
      else if ((invalid_set_index= mariadbd_check_set_value(option, option_value_start)))
      {
        line_valid= FALSE;
        file_valid= FALSE;
        if (!opt_print && opt_edit_mode == EDIT_MODE_NONE)
        {
          fprintf(stdout, "In %s at line %d: Invalid value in set %s at index %d for option %s\n",
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
            fprintf(stdout, "In %s at line %d: Please replace audit_log with the server_audit plugin\n",
                    name, line);
            ctx->failed= 1;
            continue;
          }
        }
      }
      else if (strcmp(curr_gr, "mariadbd") &&
               (opt_edit_mode == EDIT_MODE_INLINE_OLD_VERSION ||
                opt_edit_mode == EDIT_MODE_LAST_OLD_VERSION)
               && !strcmp(option, "key_buffer_size"))
      {
        file_valid= FALSE;
        generator_add_line(&generator, buff, LINE_TYPE_OPTION, FALSE);
        if (opt_no_myisam_files)
        {
          add_line(&generator.alloc, &generator.mariadbd_additions,
                   "key_buffer_size=64K\n");
          add_line(&generator.alloc, &generator.mariadbd_additions,
                   "aria_pagecache_buffer_size=");
          add_line(&generator.alloc, &generator.mariadbd_additions,
                   option_value_start);
          add_line(&generator.alloc, &generator.mariadbd_additions, "\n");
        }
        else
        {
          add_line(&generator.alloc, &generator.mariadbd_additions, "%s", buff);
          add_line(&generator.alloc, &generator.mariadbd_additions,
                   "#key-buffer-size=64K\n");
        }
        continue;
      }
    }
    else if (!strcmp(option, "skip_slave_start"))
    {
      skip_slave_start_present= TRUE;
    }
    generator_add_line(&generator, buff, LINE_TYPE_OPTION, line_valid);
  }
  mysql_file_fclose(fp, MYF(0));
  if (opt_add_skip_slave_start && !skip_slave_start_present)
  {
    file_valid= FALSE;
    add_line(&generator.alloc, &generator.mariadbd_additions,
             "skip_slave_start\n");
  }
  generator_write(tmp_fp, &generator);
  generator_free(&generator);
  if (tmp_fp)
  {
    mysql_file_fclose(tmp_fp, MYF(0));
    if (file_valid)
    {
      my_delete(tmp_name, MYF(0));
    }
    else
    {
      myf redel_flags = opt_backup ? MYF(MY_REDEL_MAKE_BACKUP) : MYF(0);
      char *duped_name= my_strdup(key_memory_upgrade_config, name, MYF(0));
      if (!duped_name || insert_dynamic(&ctx->updated_files, &duped_name))
        return -1;
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
  mysql_file_fclose(fp, MYF(0));
  if (tmp_fp)
    mysql_file_fclose(tmp_fp, MYF(0));
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
  else if (dirname_length(conf_file))
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

static int process_defaults(const char *conf_file,
                            const char **groups,
                            const char **dirs,
                            const char **defaults_args,
                            int defaults_args_count)
{
  int error= 0;
  MEM_ROOT alloc;
  TYPELIB group; // XXX
  struct upgrade_ctx ctx;

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

  if (ctx.updated_files.elements > 0)
  {
    DYNAMIC_STRING mariadbd_output;
    const char *mariadbd_name= IF_WIN("mariadbd.exe", "mariadbd");

    if (init_dynamic_string(&mariadbd_output, "", 32, 32))
      goto err;
    if (!test_mariadbd(mariadbd_name) &&
        (error= run_mariadbd(mariadbd_name, &mariadbd_output, defaults_args, defaults_args_count)))
    {
      fputs("error: Failed to run mariadbd with the updated files, reverting\n", stderr);
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
  OPT_CURRENT_VERSION,
  OPT_EDIT,
  OPT_PRINT,
  OPT_NO_MYISAM_FILES,
  OPT_ADD_SKIP_SLAVE_START,
};

static struct my_option my_long_options[] =
{
  {"help", '?', "Display this help message and exit.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Output version information and exit.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"update", OPT_UPDATE, "Update the configuration files in place.",
   0, 0, 0, GET_NO_ARG, NO_ARG, FALSE, 0, 0, 0, 0, 0},
  {"backup", OPT_BACKUP,
   "Backup the updated configuration files. The backup file names end in a "
   "timestamp followed by .BAK",
   0, 0, 0, GET_NO_ARG, NO_ARG, FALSE, 0, 0, 0, 0, 0},
  {"current-version", OPT_CURRENT_VERSION,
   "Section to use for invalid options. See --edit.",
   &opt_current_version, &opt_current_version,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"edit", OPT_EDIT,
   "Select what to do with invalid options",
   &opt_edit_mode, &opt_edit_mode, &edit_mode_typelib, GET_ENUM,
   REQUIRED_ARG, EDIT_MODE_NONE, 0, 0, 0, 0, 0},
  {"print", OPT_PRINT, "Print upgraded files to stdout.",
   0, 0, 0, GET_NO_ARG, NO_ARG, FALSE, 0, 0, 0, 0, 0},
  {"no-myisam-files", OPT_NO_MYISAM_FILES,
   "Don't try to support MyISAM.",
   0, 0, 0, GET_NO_ARG, NO_ARG, FALSE, 0, 0, 0, 0, 0},
  {"add-skip-slave-start", OPT_ADD_SKIP_SLAVE_START,
   "Add skip_slave_start to the mariadbd section if not present.",
   0, 0, 0, GET_NO_ARG, NO_ARG, FALSE, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

static void cleanup_and_exit(int exit_code) __attribute__ ((noreturn));
static void cleanup_and_exit(int exit_code)
{
  my_end(0);
  exit(exit_code);
}

static void usage() __attribute__ ((noreturn));
static void usage()
{
  print_version();
  puts("This software comes with ABSOLUTELY NO WARRANTY. This is free software,\nand you are welcome to modify and redistribute it under the GPL license\n");
  puts("Displays the unrecognized options present in configuration files, which is useful when upgrading MariaDB");
  printf("Usage: %s [OPTIONS]\n", my_progname);
  my_print_help(my_long_options);
  my_print_default_files(config_file);
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
  case OPT_UPDATE:
    opt_update= TRUE;
    break;
  case OPT_BACKUP:
    opt_backup= TRUE;
    break;
  case OPT_PRINT:
    opt_print= TRUE;
    break;
  case OPT_NO_MYISAM_FILES:
    opt_no_myisam_files= TRUE;
    break;
  case OPT_ADD_SKIP_SLAVE_START:
    opt_add_skip_slave_start= TRUE;
    break;
  }
  return 0;
}


static int get_options(int *argc,char ***argv)
{
  int ho_error;

  if ((ho_error=handle_options(argc, argv, my_long_options, get_one_option)))
    exit(ho_error);

  if (opt_update && opt_edit_mode == EDIT_MODE_NONE)
  {
    fputs("error: --update provided without --edit=<mode>\n", stderr);
    exit(1);
  }
  if (opt_backup && !opt_update)
  {
    fputs("error: --backup provided without --update\n", stderr);
    exit(1);
  }
  if (!opt_current_version && (opt_edit_mode == EDIT_MODE_INLINE_OLD_VERSION ||
                               opt_edit_mode == EDIT_MODE_LAST_OLD_VERSION))
  {
    fputs("error: Selected --edit mode requires --current-version\n", stderr);
    exit(1);
  }
  if (opt_current_version && (opt_edit_mode != EDIT_MODE_INLINE_OLD_VERSION &&
                              opt_edit_mode != EDIT_MODE_LAST_OLD_VERSION))
  {
    fputs("error: --current-version provided without a corresponding --edit mode\n", stderr);
    exit(1);
  }
  if (opt_print && opt_update)
  {
    fputs("error: --print and --update can't be specified simultaneously\n", stderr);
    exit(1);
  }

  return 0;
}


int main(int argc, char **argv)
{
  int count, error, args_used;
  char **load_default_groups= 0, *tmp_arguments[6];
  char **arguments, **org_argv;
  const char **default_directories;
  int i= 0;
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
  if (get_options(&argc,&org_argv))
    cleanup_and_exit(1);

  load_default_groups=(char**) my_malloc(PSI_NOT_INSTRUMENTED,
                                         array_elements(mysqld_groups)*sizeof(char*), MYF(MY_WME));
  if (!load_default_groups)
    exit(1);
  for (; mysqld_groups[i]; i++)
    load_default_groups[i]= (char*) mysqld_groups[i];
  memcpy(load_default_groups + i, org_argv, (argc + 1) * sizeof(*org_argv));
  if ((error= my_load_defaults(config_file, (const char **) load_default_groups,
                               &count, &arguments, &default_directories)))
  {
    my_end(0);
    if (error == 4)
      return 0;
    return 2;
  }
  /*
    The defaults-xxx arguments are still intact at the beginning of argv and
    can be passed to mariadbd for testing the updated configuration files.
  */
  error= process_defaults(config_file,
                          (const char **) load_default_groups,
                          default_directories,
                          (const char **)(argv + 1),
                          args_used - 1);

  my_free(load_default_groups);
  free_defaults(arguments);
  my_end(0);
  exit(error != 0);
}

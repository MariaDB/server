/* Copyright (c) 2000, 2017, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2011, 2018, MariaDB Corporation
   Copyrigth (c) 2024, Väinö Mäkelä

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
#include <welcome_copyright_notice.h>

#define load_default_groups mysqld_groups
#include <mysqld_default_groups.h>
#undef load_default_groups


struct convert_ctx
{
  MEM_ROOT *alloc;
  TYPELIB *group;
  my_bool failed;
};


static PSI_memory_key key_memory_convert;
static PSI_file_key key_file_cnf;


#ifdef _WIN32
static const char *f_extensions[]= { ".ini", ".cnf", 0 };
#else
static const char *f_extensions[]= { ".cnf", 0 };
#endif


static int compare_options(const void *a, const void *b)
{
  const char *first= *(const char **)a;
  const char *second= *(const char **)b;
  return strcmp(first, second);
}


static my_bool mariadbd_option_exists(const char *option)
{
   return bsearch(&option, mariadbd_valid_options,
           sizeof mariadbd_valid_options / sizeof mariadbd_valid_options[0],
           sizeof mariadbd_valid_options[0],
           compare_options) != NULL;
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


static int process_default_file_with_ext(struct convert_ctx *ctx,
                                        const char *dir, const char *ext,
                                        const char *config_file,
                                        int recursion_level)
{
  char name[FN_REFLEN + 10], buff[4096], curr_gr[4096], *ptr, *end, **tmp_ext;
  char *value, option[4096+2], tmp[FN_REFLEN];
  static const char includedir_keyword[]= "includedir";
  static const char include_keyword[]= "include";
  const int max_recursion_level= 10;
  MYSQL_FILE *fp;
  uint line=0;
  enum { NONE, PARSE, SKIP } found_group= NONE;
  size_t i;
  MY_DIR *search_dir;
  FILEINFO *search_file;

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

  while (mysql_file_fgets(buff, sizeof(buff) - 1, fp))
  {
    line++;
    /* Ignore comment and empty lines */
    for (ptr= buff; my_isspace(&my_charset_latin1, *ptr); ptr++)
    {}

    if (*ptr == '#' || *ptr == ';' || !*ptr)
      continue;

    /* Configuration File Directives */
    if (*ptr == '!')
    {
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
      *ptr= 0;
      if (!mariadbd_option_exists(option))
      {
        fprintf(stdout, "In %s at line %d: Invalid option %s\n", name, line, option);
        ctx->failed= 1;
      }
      *ptr++= '=';
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
    }
  }
  mysql_file_fclose(fp, MYF(0));
  return(0);

 err:
  mysql_file_fclose(fp, MYF(0));
  return -1;					/* Fatal error */
}



static int process_default_file(struct convert_ctx *ctx,
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
                                struct convert_ctx *ctx,
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
                            const char **dirs)
{
  int error= 0;
  MEM_ROOT alloc;
  TYPELIB group; // XXX
  struct convert_ctx ctx;

  init_alloc_root(key_memory_convert, &alloc, 512, 0, MYF(0));

  group.count= 0;
  group.name= "defaults";
  group.type_names= groups;

  for (; *groups ; groups++)
    group.count++;

  ctx.alloc= &alloc;
  ctx.group= &group;
  ctx.failed= 0;

  if ((error= process_option_files(conf_file, &ctx, dirs)))
  {
    free_root(&alloc,MYF(0));
    return error;
  }

  free_root(&alloc, MYF(0));
  return ctx.failed;
}


static const char *config_file="my";			/* Default config file */

static struct my_option my_long_options[] =
{
  {"help", '?', "Display this help message and exit.",
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
  error= process_defaults(config_file, (const char **) load_default_groups, default_directories);

  my_free(load_default_groups);
  free_defaults(arguments);
  my_end(0);
  exit(error != 0);
}

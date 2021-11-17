/* Copyright (c) 2000, 2017, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2011, 2018, MariaDB Corporation

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

/****************************************************************************
 Add all options from files named "group".cnf from the default_directories
 before the command line arguments.
 On Windows defaults will also search in the Windows directory for a file
 called 'group'.ini
 As long as the program uses the last argument for conflicting
 options one only have to add a call to "load_defaults" to enable
 use of default values.
 pre- and end 'blank space' are removed from options and values. The
 following escape sequences are recognized in values:  \b \t \n \r \\

 The following arguments are handled automatically;  If used, they must be
 first argument on the command line!
 --no-defaults	; no options are read.
 --defaults-file=full-path-to-default-file	; Only this file will be read.
 --defaults-extra-file=full-path-to-default-file ; Read this file before ~/
 --defaults-group-suffix  ; Also read groups with concat(group, suffix)
 --print-defaults	  ; Print the modified command line and exit
****************************************************************************/

#include "mysys_priv.h"
#include <my_default.h>
#include <m_string.h>
#include <m_ctype.h>
#include <my_dir.h>
#ifdef _WIN32
#include <winbase.h>
#endif

/*
  Mark file names in argv[]. File marker is *always* followed by a file name
  All options after it come from that file.
  Empty file name ("") means command line.
*/
static char *file_marker= (char*)"----file-marker----";
my_bool my_defaults_mark_files= FALSE;
my_bool is_file_marker(const char* arg)
{
  return arg == file_marker;
}

my_bool my_no_defaults=FALSE, my_print_defaults= FALSE;
const char *my_defaults_file=0;
const char *my_defaults_group_suffix=0;
const char *my_defaults_extra_file=0;

/* Which directories are searched for options (and in which order) */

#define MAX_DEFAULT_DIRS 7
#define DEFAULT_DIRS_SIZE (MAX_DEFAULT_DIRS + 1)  /* Terminate with NULL */
static const char **default_directories = NULL;

#ifdef _WIN32
static const char *f_extensions[]= { ".ini", ".cnf", 0 };
#define NEWLINE "\r\n"
#else
static const char *f_extensions[]= { ".cnf", 0 };
#define NEWLINE "\n"
#endif

struct handle_option_ctx
{
   MEM_ROOT *alloc;
   DYNAMIC_ARRAY *args;
   TYPELIB *group;
};

static int search_default_file(struct handle_option_ctx *,
			       const char *, const char *);
static int search_default_file_with_ext(struct handle_option_ctx *,
					const char *, const char *,
					const char *, int);


/**
  Create the list of default directories.

  @param alloc  MEM_ROOT where the list of directories is stored

  @details
  The directories searched, in order, are:
  - Windows:     GetSystemWindowsDirectory()
  - Windows:     GetWindowsDirectory()
  - Windows:     C:/
  - Windows:     Directory above where the executable is located
  - Unix:        /etc/ or the value of DEFAULT_SYSCONFDIR, if defined
  - Unix:        /etc/mysql/ unless DEFAULT_SYSCONFDIR is defined
  - ALL:         getenv("MYSQL_HOME")
  - ALL:         --defaults-extra-file=<path> (run-time option)
  - Unix:        ~/

  On all systems, if a directory is already in the list, it will be moved
  to the end of the list.  This avoids reading defaults files multiple times,
  while ensuring the correct precedence.

  @retval NULL  Failure (out of memory, probably)
  @retval other Pointer to NULL-terminated array of default directories
*/

static const char **init_default_directories(MEM_ROOT *alloc);


static char *remove_end_comment(char *ptr);


/*
  Process config files in default directories.

  SYNOPSIS
  my_search_option_files()
  conf_file                   Basename for configuration file to search for.
                              If this is a path, then only this file is read.
  argc                        Pointer to argc of original program
  argv                        Pointer to argv of original program
  func                        Pointer to the function to process options
  func_ctx                    It's context. Usually it is the structure to
                              store additional options.
  DESCRIPTION
    Process the default options from argc & argv
    Read through each found config file looks and calls 'func' to process
    each option.

  NOTES
    --defaults-group-suffix is only processed if we are called from
    load_defaults().


  RETURN
    0  ok
    1  given cinf_file doesn't exist
    2  out of memory
    3  Can't get current working directory

    The global variable 'my_defaults_group_suffix' is updated with value for
    --defaults_group_suffix
*/

static int my_search_option_files(const char *conf_file,
                                  struct handle_option_ctx *ctx,
                                  const char **default_directories)
{
  const char **dirs;
  int error= 0;
  DBUG_ENTER("my_search_option_files");

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
      DBUG_RETURN(2);
    
    for (i= 0; i < group->count; i++)
    {
      size_t len;
      extra_groups[i]= group->type_names[i]; /** copy group */
      
      len= strlen(extra_groups[i]);
      if (!(ptr= alloc_root(ctx->alloc, (uint) (len+instance_len+1))))
       DBUG_RETURN(2);
      
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
    if ((error= search_default_file_with_ext(ctx, "", "",
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
    if ((error= search_default_file(ctx, NullS, conf_file)) < 0)
      goto err;
  }
  else
  {
    for (dirs= default_directories ; *dirs; dirs++)
    {
      if (**dirs)
      {
	if (search_default_file(ctx, *dirs, conf_file) < 0)
	  goto err;
      }
      else if (my_defaults_extra_file)
      {
        if ((error= search_default_file_with_ext(ctx, "", "",
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

  DBUG_RETURN(0);

err:
  fprintf(stderr,"Fatal error in defaults handling. Program aborted\n");
  DBUG_RETURN(1);
}


/*
  adds an option to the array of options

  SYNOPSIS
    add_option()
    in_ctx                  Handler context.
    option                  The very option to be processed. It is already
                            prepared to be used in argv (has -- prefix).

  RETURN
    0 - ok
    1 - error occurred
*/

static int add_option(struct handle_option_ctx *ctx, const char *option)
{
  char *tmp= strdup_root(ctx->alloc, option);
  return !tmp || insert_dynamic(ctx->args, (uchar*) &tmp);
}


/*
  Gets options from the command line

  SYNOPSIS
    get_defaults_options()
    argv			Pointer to argv of original program

  DESCRIPTION
    Sets my_no_defaults, my_defaults_file, my_defaults_extra_file,
    my_defaults_group_suffix, my_print_defaults

  RETURN
    # Number of arguments used from *argv
*/

int get_defaults_options(char **argv)
{
  static char file_buffer[FN_REFLEN];
  static char extra_file_buffer[FN_REFLEN];
  char **orig_argv= argv;

  argv++; /* Skip program name */

  my_defaults_file= my_defaults_group_suffix= my_defaults_extra_file= 0;
  my_no_defaults= my_print_defaults= FALSE;

  if (*argv && !strcmp(*argv, "--no-defaults"))
  {
    my_no_defaults= 1;
    argv++;
  }
  else
    for(; *argv; argv++)
    {
      if (!my_defaults_file && is_prefix(*argv, "--defaults-file="))
        my_defaults_file= *argv + sizeof("--defaults-file=")-1;
      else
      if (!my_defaults_extra_file && is_prefix(*argv, "--defaults-extra-file="))
        my_defaults_extra_file= *argv + sizeof("--defaults-extra-file=")-1;
      else
      if (!my_defaults_group_suffix && is_prefix(*argv, "--defaults-group-suffix="))
        my_defaults_group_suffix= *argv + sizeof("--defaults-group-suffix=")-1;
      else
        break;
    }

  if (*argv && !strcmp(*argv, "--print-defaults"))
  {
    my_print_defaults= 1;
    my_defaults_mark_files= FALSE;
    argv++;
  }

  if (! my_defaults_group_suffix)
    my_defaults_group_suffix= getenv("MYSQL_GROUP_SUFFIX");

  if (my_defaults_extra_file && my_defaults_extra_file != extra_file_buffer)
  {
    my_realpath(extra_file_buffer, my_defaults_extra_file, MYF(0));
    my_defaults_extra_file= extra_file_buffer;
  }

  if (my_defaults_file && my_defaults_file != file_buffer)
  {
    my_realpath(file_buffer, my_defaults_file, MYF(0));
    my_defaults_file= file_buffer;
  }

  return (int)(argv - orig_argv);
}

/*
  Wrapper around my_load_defaults() for interface compatibility.

  SYNOPSIS
    load_defaults()
    conf_file			Basename for configuration file to search for.
    				If this is a path, then only this file is read.
    groups			Which [group] entrys to read.
				Points to an null terminated array of pointers
    argc			Pointer to argc of original program
    argv			Pointer to argv of original program

  NOTES

    This function is NOT thread-safe as it uses a global pointer internally.
    See also notes for my_load_defaults().

  RETURN
    0 ok
    1 The given conf_file didn't exists
*/
int load_defaults(const char *conf_file, const char **groups,
                  int *argc, char ***argv)
{
  return my_load_defaults(conf_file, groups, argc, argv, &default_directories);
}

/*
  Read options from configurations files

  SYNOPSIS
    my_load_defaults()
    conf_file			Basename for configuration file to search for.
    				If this is a path, then only this file is read.
    groups			Which [group] entrys to read.
				Points to an null terminated array of pointers
    argc			Pointer to argc of original program
    argv			Pointer to argv of original program
    default_directories         Pointer to a location where a pointer to the list
                                of default directories will be stored

  IMPLEMENTATION

   Read options from configuration files and put them BEFORE the arguments
   that are already in argc and argv.  This way the calling program can
   easily command line options override options in configuration files

   NOTES
    In case of fatal error, the function will print a warning and returns 2
 
    To free used memory one should call free_defaults() with the argument
    that was put in *argv

   RETURN
     - If successful, 0 is returned. If 'default_directories' is not NULL,
     a pointer to the array of default directory paths is stored to a location
     it points to. That stored value must be passed to my_search_option_files()
     later.
     
     - 1 is returned if the given conf_file didn't exist. In this case, the
     value pointed to by default_directories is undefined.
*/


int my_load_defaults(const char *conf_file, const char **groups, int *argc,
                     char ***argv, const char ***default_directories)
{
  DYNAMIC_ARRAY args;
  int args_used= 0;
  int error= 0;
  MEM_ROOT alloc;
  char *ptr,**res;
  const char **dirs;
  DBUG_ENTER("my_load_defaults");

  init_alloc_root(key_memory_defaults, &alloc, 512, 0, MYF(0));
  if ((dirs= init_default_directories(&alloc)) == NULL)
    goto err;

  args_used= get_defaults_options(*argv);

  if (my_init_dynamic_array(key_memory_defaults, &args, sizeof(char*), 128, 64,
                            MYF(0)))
    goto err;

  insert_dynamic(&args, *argv);/* Name MUST be set, even by embedded library */

  *argc-= args_used;
  *argv+= args_used;

  if (!my_no_defaults)
  {
    TYPELIB group; // XXX
    struct handle_option_ctx ctx;

    group.count=0;
    group.name= "defaults";
    group.type_names= groups;

    for (; *groups ; groups++)
      group.count++;

    ctx.alloc= &alloc;
    ctx.args= &args;
    ctx.group= &group;

    if ((error= my_search_option_files(conf_file, &ctx, dirs)))
    {
      delete_dynamic(&args);
      free_root(&alloc,MYF(0));
      DBUG_RETURN(error);
    }
  }

  if (!(ptr=(char*) alloc_root(&alloc, sizeof(alloc) +
			       (args.elements + *argc + 3) * sizeof(char*))))
    goto err;
  res= (char**) (ptr+sizeof(alloc));

  /* found arguments + command line arguments to new array */
  memcpy(res, args.buffer, args.elements * sizeof(char*));

  if (my_defaults_mark_files)
  {
    res[args.elements++]= file_marker;
    res[args.elements++]= (char*)"";
  }

  if (*argc)
    memcpy(res + args.elements, *argv, *argc * sizeof(char*));

  (*argc)+= (int)args.elements;
  *argv= res;
  (*argv)[*argc]= 0;
  *(MEM_ROOT*) ptr= alloc;			/* Save alloc root for free */
  delete_dynamic(&args);
  if (my_print_defaults)
  {
    int i;
    printf("%s would have been started with the following arguments:\n",
	   **argv);
    for (i=1 ; i < *argc ; i++)
      printf("%s ", (*argv)[i]);
    puts("");
    DBUG_RETURN(4);
  }

  if (default_directories)
    *default_directories= dirs;

  DBUG_RETURN(0);

 err:
  fprintf(stderr,"Fatal error in defaults handling. Program aborted\n");
  DBUG_RETURN(2);
}


void free_defaults(char **argv)
{
  MEM_ROOT ptr;
  memcpy(&ptr, ((char *) argv) - sizeof(ptr), sizeof(ptr));
  free_root(&ptr,MYF(0));
}


static int search_default_file(struct handle_option_ctx *ctx, const char *dir,
			       const char *config_file)
{
  char **ext;
  const char *empty_list[]= { "", 0 };
  my_bool have_ext= fn_ext(config_file)[0] != 0;
  const char **exts_to_use= have_ext ? empty_list : f_extensions;

  for (ext= (char**) exts_to_use; *ext; ext++)
  {
    int error;
    if ((error= search_default_file_with_ext(ctx, dir, *ext, config_file, 0)) < 0)
      return error;
  }
  return 0;
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


/*
  Open a configuration file (if exists) and read given options from it

  SYNOPSIS
    search_default_file_with_ext()
    ctx                         Pointer to the structure to store actual 
                                parameters of the function.
    dir				directory to read
    ext				Extension for configuration file
    config_file                 Name of configuration file
    group			groups to read
    recursion_level             the level of recursion, got while processing
                                "!include" or "!includedir"

  RETURN
    0   Success
    -1	Fatal error, abort
     1	File not found (Warning)
*/

static int search_default_file_with_ext(struct handle_option_ctx *ctx,
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
      Ignore world-writable regular files.
      This is mainly done to protect us to not read a file created by
      the mysqld server, but the check is still valid in most context. 
    */
    if ((stat_info.st_mode & S_IWOTH) &&
	(stat_info.st_mode & S_IFMT) == S_IFREG)
    {
      fprintf(stderr, "Warning: World-writable config file '%s' is ignored\n",
              name);
      return 0;
    }
  }
#endif
  if (!(fp= mysql_file_fopen(key_file_cnf, name, O_RDONLY, MYF(0))))
    return 1;					/* Ignore wrong files */

  if (my_defaults_mark_files)
    if (insert_dynamic(ctx->args, (uchar*) &file_marker) ||
        add_option(ctx, name))
      goto err;

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

            search_default_file_with_ext(ctx, "", "", tmp, recursion_level + 1);
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

        search_default_file_with_ext(ctx, "", "", ptr, recursion_level + 1);
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
    ptr= strmake(strmov(option,"--"), ptr, (size_t) (end-ptr));
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

    if (add_option(ctx, option))
      goto err;
  }
  mysql_file_fclose(fp, MYF(0));
  return(0);

 err:
  mysql_file_fclose(fp, MYF(0));
  return -1;					/* Fatal error */
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


void my_print_default_files(const char *conf_file)
{
  const char *empty_list[]= { "", 0 };
  my_bool have_ext= fn_ext(conf_file)[0] != 0;
  const char **exts_to_use= have_ext ? empty_list : f_extensions;
  char name[FN_REFLEN], **ext;

  puts("\nDefault options are read from the following files in the given order:");
  if (my_defaults_file)
  {
    puts(my_defaults_file);
    return;
  }

  if (dirname_length(conf_file))
    fputs(conf_file,stdout);
  else
  {
    const char **dirs;
    MEM_ROOT alloc;
    init_alloc_root(key_memory_defaults, &alloc, 512, 0, MYF(0));

    if ((dirs= init_default_directories(&alloc)) == NULL)
    {
      fputs("Internal error initializing default directories list", stdout);
    }
    else
    {
      for ( ; *dirs; dirs++)
      {
        for (ext= (char**) exts_to_use; *ext; ext++)
        {
          const char *pos;
          char *end;
          if (**dirs)
            pos= *dirs;
          else if (my_defaults_extra_file)
          {
            pos= my_defaults_extra_file;
            fputs(pos, stdout);
            fputs(" ", stdout);
            continue;
          }
          else
            continue;
          end= convert_dirname(name, pos, NullS);
          if (name[0] == FN_HOMELIB)	/* Add . to filenames in home */
            *end++= '.';
          strxmov(end, conf_file, *ext, " ", NullS);
          fputs(name, stdout);
        }
      }
    }

    free_root(&alloc, MYF(0));
  }
  puts("");
}

void print_defaults(const char *conf_file, const char **groups)
{
  const char **groups_save= groups;
  my_print_default_files(conf_file);

  fputs("The following groups are read:",stdout);
  for ( ; *groups ; groups++)
  {
    fputc(' ',stdout);
    fputs(*groups,stdout);
  }

  if (my_defaults_group_suffix)
  {
    groups= groups_save;
    for ( ; *groups ; groups++)
    {
      fputc(' ',stdout);
      fputs(*groups,stdout);
      fputs(my_defaults_group_suffix,stdout);
    }
  }
  puts("\nThe following options may be given as the first argument:\n\
--print-defaults          Print the program argument list and exit.\n\
--no-defaults             Don't read default options from any option file.\n\
The following specify which files/extra groups are read (specified before remaining options):\n\
--defaults-file=#         Only read default options from the given file #.\n\
--defaults-extra-file=#   Read this file after the global files are read.\n\
--defaults-group-suffix=# Additionally read default groups with # appended as a suffix.");
}


static int add_directory(MEM_ROOT *alloc, const char *dir, const char **dirs)
{
  char buf[FN_REFLEN];
  size_t len;
  char *p;
  my_bool err __attribute__((unused));

  len= normalize_dirname(buf, dir);
  if (!(p= strmake_root(alloc, buf, len)))
    return 1;  /* Failure */
  /* Should never fail if DEFAULT_DIRS_SIZE is correct size */
  err= array_append_string_unique(p, dirs, DEFAULT_DIRS_SIZE);
  DBUG_ASSERT(err == FALSE);

  return 0;
}

#ifdef _WIN32
static const char *my_get_module_parent(char *buf, size_t size)
{
  char *last= NULL;
  char *end;
  if (!GetModuleFileName(NULL, buf, (DWORD) size))
    return NULL;
  end= strend(buf);

  /*
    Look for the second-to-last \ in the filename, but hang on
    to a pointer after the last \ in case we're in the root of
    a drive.
  */
  for ( ; end > buf; end--)
  {
    if (*end == FN_LIBCHAR)
    {
      if (last)
      {
        /* Keep the last '\' as this works both with D:\ and a directory */
        end[1]= 0;
        break;
      }
      last= end;
    }
  }

  return buf;
}
#endif /* _WIN32 */


static const char **init_default_directories(MEM_ROOT *alloc)
{
  const char **dirs;
  char *env;
  int errors= 0;
  DBUG_ENTER("init_default_directories");

  dirs= (const char **)alloc_root(alloc, DEFAULT_DIRS_SIZE * sizeof(char *));
  if (dirs == NULL)
    DBUG_RETURN(NULL);
  bzero((char *) dirs, DEFAULT_DIRS_SIZE * sizeof(char *));

#ifdef _WIN32

  {
    char fname_buffer[FN_REFLEN];
    if (GetSystemWindowsDirectory(fname_buffer, sizeof(fname_buffer)))
      errors += add_directory(alloc, fname_buffer, dirs);

    if (GetWindowsDirectory(fname_buffer, sizeof(fname_buffer)))
      errors += add_directory(alloc, fname_buffer, dirs);

    errors += add_directory(alloc, "C:/", dirs);

    if (my_get_module_parent(fname_buffer, sizeof(fname_buffer)) != NULL)
    {
      errors += add_directory(alloc, fname_buffer, dirs);

      strcat_s(fname_buffer, sizeof(fname_buffer), "/data");
      errors += add_directory(alloc, fname_buffer, dirs);
    }
  }

#else

#if defined(DEFAULT_SYSCONFDIR)
  if (DEFAULT_SYSCONFDIR[0])
    errors += add_directory(alloc, DEFAULT_SYSCONFDIR, dirs);
#else
  errors += add_directory(alloc, "/etc/", dirs);
  errors += add_directory(alloc, "/etc/mysql/", dirs);
#endif /* DEFAULT_SYSCONFDIR */

#endif

  /* 
    If value of $MARIADB_HOME environment variable name is NULL, check
    for $MYSQL_HOME
  */
  if ((env= getenv("MARIADB_HOME")))
    errors += add_directory(alloc, env, dirs);
  else
  {
    if ((env= getenv("MYSQL_HOME")))
      errors += add_directory(alloc, env, dirs);
  }

  /* Placeholder for --defaults-extra-file=<path> */
  errors += add_directory(alloc, "", dirs);

#if !defined(_WIN32)
  errors += add_directory(alloc, "~/", dirs);
#endif

  DBUG_RETURN(errors > 0 ? NULL : dirs);
}

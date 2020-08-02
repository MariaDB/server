/* Copyright (c) 2000, 2011, Oracle and/or its affiliates.
   Copyright (C) 2011, 2020, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file

  @brief
  Read language depeneded messagefile
*/

#include <my_global.h>
#include "sql_priv.h"
#include "unireg.h"
#include "derror.h"
#include "mysys_err.h"
#include "mysqld.h"                             // lc_messages_dir
#include "derror.h"                             // read_texts
#include "sql_class.h"                          // THD

uint errors_per_range[MAX_ERROR_RANGES+1];

static bool check_error_mesg(const char *file_name, const char **errmsg);
static void init_myfunc_errs(void);


C_MODE_START
static const char **get_server_errmsgs(int nr)
{
  int section= (nr-ER_ERROR_FIRST) / ERRORS_PER_RANGE;
  if (!current_thd)
    return DEFAULT_ERRMSGS[section];
  return CURRENT_THD_ERRMSGS[section];
}
C_MODE_END

/**
  Read messages from errorfile.

  This function can be called multiple times to reload the messages.

  If it fails to load the messages:
   - If we already have error messages loaded, keep the old ones and
     return FALSE(ok)
  - Initializing the errmesg pointer to an array of empty strings
    and return TRUE (error)

  @retval
    FALSE       OK
  @retval
    TRUE        Error
*/

static const char ***original_error_messages;

bool init_errmessage(void)
{
  const char **errmsgs;
  bool error= FALSE;
  const char *lang= my_default_lc_messages->errmsgs->language;
  my_bool use_english;

  DBUG_ENTER("init_errmessage");

  free_error_messages();
  my_free(original_error_messages);
  original_error_messages= 0;

  error_message_charset_info= system_charset_info;

  use_english= !strcmp(lang, "english");
  if (!use_english)
  {
    /* Read messages from file. */
    use_english= read_texts(ERRMSG_FILE,lang, &original_error_messages);
    error= use_english != FALSE;
    if (error)
      sql_print_error("Could not load error messages for %s",lang);
  }

  if (use_english)
  {
    static const struct
    {
      const char* name;
      uint id;
      const char* fmt;
    }
    english_msgs[]=
    {
      #include <mysqld_ername.h>
    };

    memset(errors_per_range, 0, sizeof(errors_per_range));
    /* Calculate nr of messages per range. */
    for (size_t i= 0; i < array_elements(english_msgs); i++)
    {
      uint id= english_msgs[i].id;

      // We rely on the fact the array is sorted by id.
      DBUG_ASSERT(i == 0 || english_msgs[i-1].id < id);

      errors_per_range[id/ERRORS_PER_RANGE-1]= id%ERRORS_PER_RANGE + 1;
    }

    size_t all_errors= 0;
    for (size_t i= 0; i < MAX_ERROR_RANGES; i++)
      all_errors+= errors_per_range[i];

    if (!(original_error_messages= (const char***)
          my_malloc((all_errors + MAX_ERROR_RANGES)* sizeof(void*),
                     MYF(MY_ZEROFILL))))
      DBUG_RETURN(TRUE);

    errmsgs= (const char**)(original_error_messages + MAX_ERROR_RANGES);

    original_error_messages[0]= errmsgs;
    for (uint i= 1; i < MAX_ERROR_RANGES; i++)
    {
      original_error_messages[i]=
        original_error_messages[i-1] + errors_per_range[i-1];
    }

    for (uint i= 0; i < array_elements(english_msgs); i++)
    {
      uint id= english_msgs[i].id;
      original_error_messages[id/ERRORS_PER_RANGE-1][id%ERRORS_PER_RANGE]=
         english_msgs[i].fmt;
    }
  }

  /* Register messages for use with my_error(). */
  for (uint i=0 ; i < MAX_ERROR_RANGES ; i++)
  {
    if (errors_per_range[i])
    {
      if (my_error_register(get_server_errmsgs, (i+1)*ERRORS_PER_RANGE,
                            (i+1)*ERRORS_PER_RANGE +
                            errors_per_range[i]-1))
      {
        my_free(original_error_messages);
        original_error_messages= 0;
        DBUG_RETURN(TRUE);
      }
    }
  }
  DEFAULT_ERRMSGS= original_error_messages;
  init_myfunc_errs();			/* Init myfunc messages */
  DBUG_RETURN(error);
}


void free_error_messages()
{
  /* We don't need to free errmsg as it's done in cleanup_errmsg */
  for (uint i= 0 ; i < MAX_ERROR_RANGES ; i++)
  {
    if (errors_per_range[i])
    {
      my_error_unregister((i+1)*ERRORS_PER_RANGE,
                          (i+1)*ERRORS_PER_RANGE +
                          errors_per_range[i]-1);
      errors_per_range[i]= 0;
    }
  }
}


/**
   Check the error messages array contains all relevant error messages
*/

static bool check_error_mesg(const char *file_name, const char **errmsg)
{
  /*
    The last MySQL error message can't be an empty string; If it is,
    it means that the error file doesn't contain all MySQL messages
    and is probably from an older version of MySQL / MariaDB.
    We also check that each section has enough error messages.
  */
  if (errmsg[ER_LAST_MYSQL_ERROR_MESSAGE -1 - ER_ERROR_FIRST][0] == 0 ||
      (errors_per_range[0] < ER_ERROR_LAST_SECTION_2 - ER_ERROR_FIRST + 1) ||
      errors_per_range[1] != 0 ||
      (errors_per_range[2] < ER_ERROR_LAST_SECTION_4 - 
       ER_ERROR_FIRST_SECTION_4 +1) ||
      (errors_per_range[3] < ER_ERROR_LAST - ER_ERROR_FIRST_SECTION_5 + 1))
  {
    sql_print_error("Error message file '%s' is probably from and older "
                    "version of MariaDB as it doesn't contain all "
                    "error messages", file_name);
    return 1;
  }
  return 0;
}


struct st_msg_file
{
  uint sections;
  uint max_error;
  uint errors;
  size_t text_length;
};

/**
  Open file for packed textfile in language-directory.
*/

static File open_error_msg_file(const char *file_name, const char *language,
                                uint error_messages, struct st_msg_file *ret)
{
  int error_pos= 0;
  File file;
  char name[FN_REFLEN];
  char lang_path[FN_REFLEN];
  uchar head[32];
  DBUG_ENTER("open_error_msg_file");

  convert_dirname(lang_path, language, NullS);
  (void) my_load_path(lang_path, lang_path, lc_messages_dir);
  if ((file= mysql_file_open(key_file_ERRMSG,
                             fn_format(name, file_name, lang_path, "", 4),
                             O_RDONLY | O_SHARE | O_BINARY,
                             MYF(0))) < 0)
  {
    /*
      Trying pre-5.4 semantics of the --language parameter.
      It included the language-specific part, e.g.:
      --language=/path/to/english/
    */
    if ((file= mysql_file_open(key_file_ERRMSG,
                               fn_format(name, file_name, lc_messages_dir, "",
                                         4),
                               O_RDONLY | O_SHARE | O_BINARY,
                               MYF(0))) < 0)
      goto err;
    sql_print_warning("An old style --language or -lc-message-dir value with language specific part detected: %s", lc_messages_dir);
    sql_print_warning("Use --lc-messages-dir without language specific part instead.");
  }
  error_pos=1;
  if (mysql_file_read(file, (uchar*) head, 32, MYF(MY_NABP)))
    goto err;
  error_pos=2;
  if (head[0] != (uchar) 254 || head[1] != (uchar) 254 ||
      head[2] != 2 || head[3] != 4)
    goto err; /* purecov: inspected */

  ret->text_length= uint4korr(head+6);
  ret->max_error=   uint2korr(head+10);
  ret->errors=      uint2korr(head+12);
  ret->sections=    uint2korr(head+14);

  if (ret->max_error < error_messages || ret->sections != MAX_ERROR_RANGES)
  {
    sql_print_error("\
Error message file '%s' had only %d error messages, but it should contain at least %d error messages.\nCheck that the above file is the right version for this program!",
		    name,ret->errors,error_messages);
    (void) mysql_file_close(file, MYF(MY_WME));
    DBUG_RETURN(FERR);
  }
  DBUG_RETURN(file);

err:
  sql_print_error((error_pos == 2) ?
                  "Incompatible header in messagefile '%s'. Probably from "
                  "another version of MariaDB" :
                  ((error_pos == 1) ? "Can't read from messagefile '%s'" :
                   "Can't find messagefile '%s'"), name);
  if (file != FERR)
    (void) mysql_file_close(file, MYF(MY_WME));
  DBUG_RETURN(FERR);
}


/*
  Define the number of normal and extra error messages in the errmsg.sys
  file
*/

static const uint error_messages= ER_ERROR_LAST - ER_ERROR_FIRST+1;

/**
  Read text from packed textfile in language-directory.
*/

bool read_texts(const char *file_name, const char *language,
                const char ****data)
{
  uint i, range_size;
  const char **point;
  size_t offset;
  File file;
  uchar *buff, *pos;
  struct st_msg_file msg_file;
  DBUG_ENTER("read_texts");

  if ((file= open_error_msg_file(file_name, language, error_messages,
                                 &msg_file)) == FERR)
    DBUG_RETURN(1);

  if (!(*data= (const char***)
	my_malloc((size_t) ((MAX_ERROR_RANGES+1) * sizeof(char**) +
                            MY_MAX(msg_file.text_length, msg_file.errors * 2)+
                            msg_file.errors * sizeof(char*)),
                  MYF(MY_WME))))
    goto err;					/* purecov: inspected */

  point= (const char**) ((*data) + MAX_ERROR_RANGES);
  buff=  (uchar*) (point + msg_file.errors);

  if (mysql_file_read(file, buff,
                      (size_t) (msg_file.errors + msg_file.sections) * 2,
                      MYF(MY_NABP | MY_WME)))
    goto err;

  pos= buff;
  /* read in sections */
  for (i= 0, offset= 0; i < msg_file.sections ; i++)
  {
    (*data)[i]= point + offset;
    errors_per_range[i]= range_size= uint2korr(pos);
    offset+= range_size;
    pos+= 2;
  }

  /* Calculate pointers to text data */
  for (i=0, offset=0 ; i < msg_file.errors ; i++)
  {
    point[i]= (char*) buff+offset;
    offset+=uint2korr(pos);
    pos+=2;
  }

  /* Read error message texts */
  if (mysql_file_read(file, buff, msg_file.text_length, MYF(MY_NABP | MY_WME)))
    goto err;

  (void) mysql_file_close(file, MYF(MY_WME));

  DBUG_RETURN(check_error_mesg(file_name, point));

err:
  (void) mysql_file_close(file, MYF(0));
  DBUG_RETURN(1);
} /* read_texts */


/**
  Initiates error-messages used by my_func-library.
*/

static void init_myfunc_errs()
{
  init_glob_errs();			/* Initiate english errors */
  if (!(specialflag & SPECIAL_ENGLISH))
  {
    EE(EE_FILENOTFOUND)   = ER_DEFAULT(ER_FILE_NOT_FOUND);
    EE(EE_CANTCREATEFILE) = ER_DEFAULT(ER_CANT_CREATE_FILE);
    EE(EE_READ)           = ER_DEFAULT(ER_ERROR_ON_READ);
    EE(EE_WRITE)          = ER_DEFAULT(ER_ERROR_ON_WRITE);
    EE(EE_BADCLOSE)       = ER_DEFAULT(ER_ERROR_ON_CLOSE);
    EE(EE_OUTOFMEMORY)    = ER_DEFAULT(ER_OUTOFMEMORY);
    EE(EE_DELETE)         = ER_DEFAULT(ER_CANT_DELETE_FILE);
    EE(EE_LINK)           = ER_DEFAULT(ER_ERROR_ON_RENAME);
    EE(EE_EOFERR)         = ER_DEFAULT(ER_UNEXPECTED_EOF);
    EE(EE_CANTLOCK)       = ER_DEFAULT(ER_CANT_LOCK);
    EE(EE_DIR)            = ER_DEFAULT(ER_CANT_READ_DIR);
    EE(EE_STAT)           = ER_DEFAULT(ER_CANT_GET_STAT);
    EE(EE_GETWD)          = ER_DEFAULT(ER_CANT_GET_WD);
    EE(EE_SETWD)          = ER_DEFAULT(ER_CANT_SET_WD);
    EE(EE_DISK_FULL)      = ER_DEFAULT(ER_DISK_FULL);
  }
}

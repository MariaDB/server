/*
   Copyright (c) 2005, 2013, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#include <my_global.h>
#include "sql_priv.h"
#include "sql_binlog.h"
#include "sql_parse.h"
#include "sql_acl.h"
#include "rpl_rli.h"
#include "slave.h"
#include "log_event.h"


/**
  Check if the event type is allowed in a BINLOG statement.

  @retval 0 if the event type is ok.
  @retval 1 if the event type is not ok.
*/
static int check_event_type(int type, Relay_log_info *rli)
{
  Format_description_log_event *fd_event=
    rli->relay_log.description_event_for_exec;

  /*
    Convert event type id of certain old versions (see comment in
    Format_description_log_event::Format_description_log_event(char*,...)).
  */
  if (fd_event && fd_event->event_type_permutation)
  {
    IF_DBUG({
        int new_type= fd_event->event_type_permutation[type];
        DBUG_PRINT("info",
                   ("converting event type %d to %d (%s)",
                    type, new_type,
                    Log_event::get_type_str((Log_event_type)new_type)));
      },
      (void)0);
    type= fd_event->event_type_permutation[type];
  }

  switch (type)
  {
  case START_EVENT_V3:
  case FORMAT_DESCRIPTION_EVENT:
    /*
      We need a preliminary FD event in order to parse the FD event,
      if we don't already have one.
    */
    if (!fd_event)
      if (!(rli->relay_log.description_event_for_exec=
            new Format_description_log_event(4)))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), 1);
        return 1;
      }

    /* It is always allowed to execute FD events. */
    return 0;
    
  case TABLE_MAP_EVENT:
  case WRITE_ROWS_EVENT_V1:
  case UPDATE_ROWS_EVENT_V1:
  case DELETE_ROWS_EVENT_V1:
  case WRITE_ROWS_EVENT:
  case UPDATE_ROWS_EVENT:
  case DELETE_ROWS_EVENT:
  case PRE_GA_WRITE_ROWS_EVENT:
  case PRE_GA_UPDATE_ROWS_EVENT:
  case PRE_GA_DELETE_ROWS_EVENT:
    /*
      Row events are only allowed if a Format_description_event has
      already been seen.
    */
    if (fd_event)
      return 0;
    else
    {
      my_error(ER_NO_FORMAT_DESCRIPTION_EVENT_BEFORE_BINLOG_STATEMENT,
               MYF(0), Log_event::get_type_str((Log_event_type)type));
      return 1;
    }
    break;

  default:
    /*
      It is not meaningful to execute other events than row-events and
      FD events. It would even be dangerous to execute Stop_log_event
      and Rotate_log_event since they call Relay_log_info::flush(), which
      is not allowed to call by other threads than the slave SQL
      thread when the slave SQL thread is running.
    */
    my_error(ER_ONLY_FD_AND_RBR_EVENTS_ALLOWED_IN_BINLOG_STATEMENT,
             MYF(0), Log_event::get_type_str((Log_event_type)type));
    return 1;
  }
}

/**
  Copy fragments into the standard placeholder thd->lex->comment.str.

  Compute the size of the (still) encoded total,
  allocate and then copy fragments one after another.
  The size can exceed max(max_allowed_packet) which is not a
  problem as no String instance is created off this char array.

  @param thd  THD handle
  @return
     0        at success,
    -1        otherwise.
*/
int binlog_defragment(THD *thd)
{
  user_var_entry *entry[2];
  LEX_STRING name[2]= { thd->lex->comment, thd->lex->ident };

  /* compute the total size */
  thd->lex->comment.str= NULL;
  thd->lex->comment.length= 0;
  for (uint k= 0; k < 2; k++)
  {
    entry[k]=
      (user_var_entry*) my_hash_search(&thd->user_vars, (uchar*) name[k].str,
                                       name[k].length);
    if (!entry[k] || entry[k]->type != STRING_RESULT)
    {
      my_error(ER_WRONG_TYPE_FOR_VAR, MYF(0), name[k].str);
      return -1;
    }
    thd->lex->comment.length += entry[k]->length;
  }

  thd->lex->comment.str=                            // to be freed by the caller
    (char *) my_malloc(thd->lex->comment.length, MYF(MY_WME));
  if (!thd->lex->comment.str)
  {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR), 1);
    return -1;
  }

  /* fragments are merged into allocated buf while the user var:s get reset */
  size_t gathered_length= 0;
  for (uint k=0; k < 2; k++)
  {
    memcpy(thd->lex->comment.str + gathered_length, entry[k]->value,
           entry[k]->length);
    gathered_length += entry[k]->length;
  }
  for (uint k=0; k < 2; k++)
    update_hash(entry[k], true, NULL, 0, STRING_RESULT, &my_charset_bin, 0);

  DBUG_ASSERT(gathered_length == thd->lex->comment.length);

  return 0;
}


/**
  Execute a BINLOG statement.

  To execute the BINLOG command properly the server needs to know
  which format the BINLOG command's event is in.  Therefore, the first
  BINLOG statement seen must be a base64 encoding of the
  Format_description_log_event, as outputted by mysqlbinlog.  This
  Format_description_log_event is cached in
  rli->description_event_for_exec.

  @param thd Pointer to THD object for the client thread executing the
  statement.
*/

void mysql_client_binlog_statement(THD* thd)
{
  DBUG_ENTER("mysql_client_binlog_statement");
  DBUG_PRINT("info",("binlog base64: '%*s'",
                     (int) (thd->lex->comment.length < 2048 ?
                            thd->lex->comment.length : 2048),
                     thd->lex->comment.str));

  if (check_global_access(thd, SUPER_ACL))
    DBUG_VOID_RETURN;

  /*
    option_bits will be changed when applying the event. But we don't expect
    it be changed permanently after BINLOG statement, so backup it first.
    It will be restored at the end of this function.
  */
  ulonglong thd_options= thd->variables.option_bits;

  /*
    Allocation
  */

  int err;
  Relay_log_info *rli;
  rpl_group_info *rgi;
  char *buf= NULL;
  size_t coded_len= 0, decoded_len= 0;

  rli= thd->rli_fake;
  if (!rli && (rli= thd->rli_fake= new Relay_log_info(FALSE)))
    rli->sql_driver_thd= thd;
  if (!(rgi= thd->rgi_fake))
    rgi= thd->rgi_fake= new rpl_group_info(rli);
  rgi->thd= thd;

  const char *error= 0;
  Log_event *ev = 0;
  my_bool is_fragmented= FALSE;

  /*
    Out of memory check
  */
  if (!(rli))
  {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR), 1);  /* needed 1 bytes */
    goto end;
  }

  DBUG_ASSERT(rli->belongs_to_client());

  if (unlikely(is_fragmented= thd->lex->comment.str && thd->lex->ident.str))
    if (binlog_defragment(thd))
      goto end;

  if (!(coded_len= thd->lex->comment.length))
  {
    my_error(ER_SYNTAX_ERROR, MYF(0));
    goto end;
  }

  decoded_len= my_base64_needed_decoded_length(coded_len);
  if (!(buf= (char *) my_malloc(decoded_len, MYF(MY_WME))))
  {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR), 1);
    goto end;
  }

  for (char const *strptr= thd->lex->comment.str ;
       strptr < thd->lex->comment.str + thd->lex->comment.length ; )
  {
    char const *endptr= 0;
    int bytes_decoded= my_base64_decode(strptr, coded_len, buf, &endptr,
                                     MY_BASE64_DECODE_ALLOW_MULTIPLE_CHUNKS);

#ifndef HAVE_valgrind
      /*
        This debug printout should not be used for valgrind builds
        since it will read from unassigned memory.
      */
    DBUG_PRINT("info",
               ("bytes_decoded: %d  strptr: %p  endptr: %p ('%c':%d)",
                bytes_decoded, strptr, endptr, *endptr,
                *endptr));
#endif

    if (bytes_decoded < 0)
    {
      my_error(ER_BASE64_DECODE_ERROR, MYF(0));
      goto end;
    }
    else if (bytes_decoded == 0)
      break; // If no bytes where read, the string contained only whitespace

    DBUG_ASSERT(bytes_decoded > 0);
    DBUG_ASSERT(endptr > strptr);
    coded_len-= endptr - strptr;
    strptr= endptr;

    /*
      Now we have one or more events stored in the buffer. The size of
      the buffer is computed based on how much base64-encoded data
      there were, so there should be ample space for the data (maybe
      even too much, since a statement can consist of a considerable
      number of events).

      TODO: Switch to use a stream-based base64 encoder/decoder in
      order to be able to read exactly what is necessary.
    */

    DBUG_PRINT("info",("binlog base64 decoded_len: %lu  bytes_decoded: %d",
                       (ulong) decoded_len, bytes_decoded));

    /*
      Now we start to read events of the buffer, until there are no
      more.
    */
    for (char *bufptr= buf ; bytes_decoded > 0 ; )
    {
      /*
        Checking that the first event in the buffer is not truncated.
      */
      ulong event_len;
      if (bytes_decoded < EVENT_LEN_OFFSET + 4 || 
          (event_len= uint4korr(bufptr + EVENT_LEN_OFFSET)) > 
           (uint) bytes_decoded)
      {
        my_error(ER_SYNTAX_ERROR, MYF(0));
        goto end;
      }
      DBUG_PRINT("info", ("event_len=%lu, bytes_decoded=%d",
                          event_len, bytes_decoded));

      if (check_event_type(bufptr[EVENT_TYPE_OFFSET], rli))
        goto end;

      ev= Log_event::read_log_event(bufptr, event_len, &error,
                                    rli->relay_log.description_event_for_exec,
                                    0);

      DBUG_PRINT("info",("binlog base64 err=%s", error));
      if (!ev)
      {
        /*
          This could actually be an out-of-memory, but it is more likely
          caused by a bad statement
        */
        my_error(ER_SYNTAX_ERROR, MYF(0));
        goto end;
      }

      bytes_decoded -= event_len;
      bufptr += event_len;

      DBUG_PRINT("info",("ev->get_type_code()=%d", ev->get_type_code()));
      ev->thd= thd;
      /*
        We go directly to the application phase, since we don't need
        to check if the event shall be skipped or not.

        Neither do we have to update the log positions, since that is
        not used at all: the rli_fake instance is used only for error
        reporting.
      */
#if !defined(MYSQL_CLIENT) && defined(HAVE_REPLICATION)
      ulonglong save_skip_replication=
                        thd->variables.option_bits & OPTION_SKIP_REPLICATION;
      thd->variables.option_bits=
        (thd->variables.option_bits & ~OPTION_SKIP_REPLICATION) |
        (ev->flags & LOG_EVENT_SKIP_REPLICATION_F ?
         OPTION_SKIP_REPLICATION : 0);

      err= ev->apply_event(rgi);

      thd->variables.option_bits=
        (thd->variables.option_bits & ~OPTION_SKIP_REPLICATION) |
        save_skip_replication;
#else
      err= 0;
#endif
      /*
        Format_description_log_event should not be deleted because it
        will be used to read info about the relay log's format; it
        will be deleted when the SQL thread does not need it,
        i.e. when this thread terminates.
      */
      if (ev->get_type_code() != FORMAT_DESCRIPTION_EVENT)
        delete ev; 
      ev= 0;
      if (err)
      {
        /*
          TODO: Maybe a better error message since the BINLOG statement
          now contains several events.
        */
        my_error(ER_UNKNOWN_ERROR, MYF(0));
        goto end;
      }
    }
  }


  DBUG_PRINT("info",("binlog base64 execution finished successfully"));
  my_ok(thd);

end:
  if (unlikely(is_fragmented))
    my_free(thd->lex->comment.str);
  thd->variables.option_bits= thd_options;
  rgi->slave_close_thread_tables(thd);
  my_free(buf);
  DBUG_VOID_RETURN;
}

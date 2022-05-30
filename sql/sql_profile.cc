/* Copyright (c) 2007, 2012, Oracle and/or its affiliates.
   Copyright (c) 2008, 2012, Monty Program Ab

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

  Implement query profiling as as list of metaphorical fences, with one fence
  per query, and each fencepost a change of thd->proc_info state (with a
  snapshot of system statistics).  When asked, we can then iterate over the 
  fenceposts and calculate the distance between them, to inform the user what
  happened during a particular query or thd->proc_info state.

  User variables that inform profiling behavior:
  - "profiling", boolean, session only, "Are queries profiled?"
  - "profiling_history_size", integer, session + global, "Num queries stored?"
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_profile.h"
#include "sql_i_s.h"                      // schema_table_store_record
#include "sql_class.h"                    // THD

#ifdef _WIN32
#pragma comment(lib,"psapi.lib")
#endif

#define TIME_FLOAT_DIGITS 9
/** two vals encoded: (len*100)+dec */
#define TIME_I_S_DECIMAL_SIZE (TIME_FLOAT_DIGITS*100)+(TIME_FLOAT_DIGITS-3)

#define MAX_QUERY_LENGTH 300
#define MAX_QUERY_HISTORY 101

/**
  Connects Information_Schema and Profiling.
*/
int fill_query_profile_statistics_info(THD *thd, TABLE_LIST *tables,
                                       Item *cond)
{
#if defined(ENABLED_PROFILING)
  return(thd->profiling.fill_statistics_info(thd, tables, cond));
#else
  my_error(ER_FEATURE_DISABLED, MYF(0), "SHOW PROFILE", "enable-profiling");
  return(1);
#endif
}

namespace Show {

ST_FIELD_INFO query_profile_statistics_info[]=
{
  Column("QUERY_ID",            SLong(20),   NOT_NULL, "Query_id"),
  Column("SEQ",                 SLong(20),   NOT_NULL, "Seq"),
  Column("STATE",               Varchar(30), NOT_NULL, "Status"),
  Column("DURATION", Decimal(TIME_I_S_DECIMAL_SIZE), NOT_NULL, "Duration"),
  Column("CPU_USER", Decimal(TIME_I_S_DECIMAL_SIZE), NULLABLE, "CPU_user"),
  Column("CPU_SYSTEM", Decimal(TIME_I_S_DECIMAL_SIZE), NULLABLE, "CPU_system"),
  Column("CONTEXT_VOLUNTARY",   SLong(20),   NULLABLE, "Context_voluntary"),
  Column("CONTEXT_INVOLUNTARY", SLong(20),   NULLABLE, "Context_involuntary"),
  Column("BLOCK_OPS_IN",        SLong(20),   NULLABLE, "Block_ops_in"),
  Column("BLOCK_OPS_OUT",       SLong(20),   NULLABLE, "Block_ops_out"),
  Column("MESSAGES_SENT",       SLong(20),   NULLABLE, "Messages_sent"),
  Column("MESSAGES_RECEIVED",   SLong(20),   NULLABLE, "Messages_received"),
  Column("PAGE_FAULTS_MAJOR",   SLong(20),   NULLABLE, "Page_faults_major"),
  Column("PAGE_FAULTS_MINOR",   SLong(20),   NULLABLE, "Page_faults_minor"),
  Column("SWAPS",               SLong(20),   NULLABLE, "Swaps"),
  Column("SOURCE_FUNCTION",     Varchar(30), NULLABLE, "Source_function"),
  Column("SOURCE_FILE",         Varchar(20), NULLABLE, "Source_file"),
  Column("SOURCE_LINE",         SLong(20),   NULLABLE, "Source_line"),
  CEnd()
};

} // namespace Show

int make_profile_table_for_show(THD *thd, ST_SCHEMA_TABLE *schema_table)
{
  uint profile_options = thd->lex->profile_options;
  uint fields_include_condition_truth_values[]= {
    FALSE, /* Query_id */
    FALSE, /* Seq */
    TRUE, /* Status */
    TRUE, /* Duration */
    profile_options & PROFILE_CPU, /* CPU_user */
    profile_options & PROFILE_CPU, /* CPU_system */
    profile_options & PROFILE_CONTEXT, /* Context_voluntary */
    profile_options & PROFILE_CONTEXT, /* Context_involuntary */
    profile_options & PROFILE_BLOCK_IO, /* Block_ops_in */
    profile_options & PROFILE_BLOCK_IO, /* Block_ops_out */
    profile_options & PROFILE_IPC, /* Messages_sent */
    profile_options & PROFILE_IPC, /* Messages_received */
    profile_options & PROFILE_PAGE_FAULTS, /* Page_faults_major */
    profile_options & PROFILE_PAGE_FAULTS, /* Page_faults_minor */
    profile_options & PROFILE_SWAPS, /* Swaps */
    profile_options & PROFILE_SOURCE, /* Source_function */
    profile_options & PROFILE_SOURCE, /* Source_file */
    profile_options & PROFILE_SOURCE, /* Source_line */
  };

  ST_FIELD_INFO *field_info;
  Name_resolution_context *context= &thd->lex->first_select_lex()->context;
  int i;

  for (i= 0; !schema_table->fields_info[i].end_marker(); i++)
  {
    if (! fields_include_condition_truth_values[i])
      continue;

    field_info= &schema_table->fields_info[i];
    Item_field *field= new (thd->mem_root) Item_field(thd, context,
                                                      field_info->name());
    if (field)
    {
      field->set_name(thd, field_info->old_name());
      if (add_item_to_list(thd, field))
        return 1;
    }
  }
  return 0;
}


#if defined(ENABLED_PROFILING)

#define RUSAGE_USEC(tv)  ((tv).tv_sec*1000*1000 + (tv).tv_usec)
#define RUSAGE_DIFF_USEC(tv1, tv2) (RUSAGE_USEC((tv1))-RUSAGE_USEC((tv2)))

#ifdef _WIN32
static ULONGLONG FileTimeToQuadWord(FILETIME *ft)
{
  // Overlay FILETIME onto a ULONGLONG.
  union {
    ULONGLONG qwTime;
    FILETIME ft;
  } u;

  u.ft = *ft;
  return u.qwTime;
}


// Get time difference between to FILETIME objects in seconds.
static double GetTimeDiffInSeconds(FILETIME *a, FILETIME *b)
{
  return ((FileTimeToQuadWord(a) - FileTimeToQuadWord(b)) / 1e7);
}
#endif

PROF_MEASUREMENT::PROF_MEASUREMENT(QUERY_PROFILE *profile_arg, const char
                                   *status_arg)
  :profile(profile_arg)
{
  collect();
  set_label(status_arg, NULL, NULL, 0);
}

PROF_MEASUREMENT::PROF_MEASUREMENT(QUERY_PROFILE *profile_arg, 
                                   const char *status_arg, 
                                   const char *function_arg, 
                                   const char *file_arg,
                                   unsigned int line_arg)
  :profile(profile_arg)
{
  collect();
  set_label(status_arg, function_arg, file_arg, line_arg);
}

PROF_MEASUREMENT::~PROF_MEASUREMENT()
{
  my_free(allocated_status_memory);
  status= function= file= NULL;
}

void PROF_MEASUREMENT::set_label(const char *status_arg, 
                                 const char *function_arg,
                                 const char *file_arg, unsigned int line_arg)
{
  size_t sizes[3];                              /* 3 == status+function+file */
  char *cursor;

  /*
    Compute all the space we'll need to allocate one block for everything
    we'll need, instead of N mallocs.
  */
  sizes[0]= (status_arg == NULL) ? 0 : strlen(status_arg) + 1;
  sizes[1]= (function_arg == NULL) ? 0 : strlen(function_arg) + 1;
  sizes[2]= (file_arg == NULL) ? 0 : strlen(file_arg) + 1;

  allocated_status_memory= (char *) my_malloc(key_memory_PROFILE, sizes[0] +
                                              sizes[1] + sizes[2], MYF(0));
  DBUG_ASSERT(allocated_status_memory != NULL);

  cursor= allocated_status_memory;

  if (status_arg != NULL)
  {
    strcpy(cursor, status_arg);
    status= cursor;
    cursor+= sizes[0];
  }
  else
    status= NULL;

  if (function_arg != NULL)
  {
    strcpy(cursor, function_arg);
    function= cursor;
    cursor+= sizes[1];
  }
  else
    function= NULL;

  if (file_arg != NULL)
  {
    strcpy(cursor, file_arg);
    file= cursor;
    cursor+= sizes[2];
  }
  else
    file= NULL;

  line= line_arg;
}

/**
  This updates the statistics for this moment of time.  It captures the state
  of the running system, so later we can compare points in time and infer what
  happened in the mean time.  It should only be called immediately upon
  instantiation of this PROF_MEASUREMENT.

  @todo  Implement resource capture for OSes not like BSD.
*/
void PROF_MEASUREMENT::collect()
{
  time_usecs= my_interval_timer() / 1e3;  /* ns to us */
#ifdef HAVE_GETRUSAGE
  getrusage(RUSAGE_SELF, &rusage);
#elif defined(_WIN32)
  FILETIME ftDummy;
  // NOTE: Get{Process|Thread}Times has a granularity of the clock interval,
  // which is typically ~15ms. So intervals shorter than that will not be
  // measurable by this function.
  GetProcessTimes(GetCurrentProcess(), &ftDummy, &ftDummy, &ftKernel, &ftUser);
  GetProcessIoCounters(GetCurrentProcess(), &io_count);
  GetProcessMemoryInfo(GetCurrentProcess(), &mem_count, sizeof(mem_count));
#endif
}


QUERY_PROFILE::QUERY_PROFILE(PROFILING *profiling_arg, const char *status_arg)
  :profiling(profiling_arg), profiling_query_id(0), query_source(NULL)
{
  m_seq_counter= 1;
  PROF_MEASUREMENT *prof= new PROF_MEASUREMENT(this, status_arg);
  prof->m_seq= m_seq_counter++;
  m_start_time_usecs= prof->time_usecs;
  m_end_time_usecs= m_start_time_usecs;
  entries.push_back(prof);
}

QUERY_PROFILE::~QUERY_PROFILE()
{
  while (! entries.is_empty())
    delete entries.pop();

  my_free(query_source);
}

/**
  @todo  Provide a way to include the full text, as in  SHOW PROCESSLIST.
*/
void QUERY_PROFILE::set_query_source(char *query_source_arg, size_t query_length_arg)
{
  /* Truncate to avoid DoS attacks. */
  size_t length= MY_MIN(MAX_QUERY_LENGTH, query_length_arg);

  DBUG_ASSERT(query_source == NULL); /* we don't leak memory */
  if (query_source_arg != NULL)
    query_source= my_strndup(key_memory_PROFILE, query_source_arg, length, MYF(0));
}

void QUERY_PROFILE::new_status(const char *status_arg,
                               const char *function_arg, const char *file_arg,
                               unsigned int line_arg)
{
  PROF_MEASUREMENT *prof;
  DBUG_ENTER("QUERY_PROFILE::status");

  if (!status_arg)
    DBUG_VOID_RETURN;

  if ((function_arg != NULL) && (file_arg != NULL))
    prof= new PROF_MEASUREMENT(this, status_arg, function_arg, base_name(file_arg), line_arg);
  else
    prof= new PROF_MEASUREMENT(this, status_arg);

  prof->m_seq= m_seq_counter++;
  m_end_time_usecs= prof->time_usecs;
  entries.push_back(prof);

  /* Maintain the query history size. */
  while (entries.elements > MAX_QUERY_HISTORY)
    delete entries.pop();

  DBUG_VOID_RETURN;
}



PROFILING::PROFILING()
  :profile_id_counter(1), current(NULL), last(NULL)
{
}

PROFILING::~PROFILING()
{
  restart();
}

/*
  Restart profiling from scratch
*/

void PROFILING::restart()
{
  while (! history.is_empty())
    delete history.pop();

  if (current != NULL)
    delete current;
  /* Ensure that profiling object can be reused */
  profile_id_counter= 1;
  current= NULL;
  last= NULL;
}


/**
  Throw away the current profile, because it's useless or unwanted
  or corrupted.
*/
void PROFILING::discard_current_query()
{
  DBUG_ENTER("PROFILING::discard_current_profile");

  delete current;
  current= NULL;

  DBUG_VOID_RETURN;
}

/**
  Try to save the current profile entry, clean up the data if it shouldn't be
  saved, and maintain the profile history size.  Naturally, this may not
  succeed if the profile was previously discarded, and that's expected.
*/
void PROFILING::finish_current_query_impl()
{
  DBUG_ENTER("PROFILING::finish_current_profile");
  DBUG_ASSERT(current);

  /* The last fence-post, so we can support the span before this. */
  status_change("ending", NULL, NULL, 0);

  if (enabled &&   /* ON at end? */
      (current->query_source != NULL) &&
      (! current->entries.is_empty()))
  {
    current->profiling_query_id= next_profile_id();   /* assign an id */

    history.push_back(current);
    last= current; /* never contains something that is not in the history. */

    /* Maintain the history size. */
    while (history.elements > thd->variables.profiling_history_size)
      delete history.pop();
  }
  else
    delete current;

  current= NULL;
  DBUG_VOID_RETURN;
}

bool PROFILING::show_profiles()
{
  QUERY_PROFILE *prof;
  List<Item> field_list;
  MEM_ROOT *mem_root= thd->mem_root;
  SELECT_LEX *sel= thd->lex->first_select_lex();
  SELECT_LEX_UNIT *unit= &thd->lex->unit;
  ha_rows idx;
  Protocol *protocol= thd->protocol;
  void *iterator;
  DBUG_ENTER("PROFILING::show_profiles");

  field_list.push_back(new (mem_root)
                       Item_return_int(thd, "Query_ID", 10,
                                       MYSQL_TYPE_LONG),
                       mem_root);
  field_list.push_back(new (mem_root)
                       Item_return_int(thd, "Duration",
                                       TIME_FLOAT_DIGITS - 1,
                                       MYSQL_TYPE_DOUBLE),
                       mem_root);
  field_list.push_back(new (mem_root) Item_empty_string(thd, "Query", 40),
                       mem_root);

  if (protocol->send_result_set_metadata(&field_list,
                                         Protocol::SEND_NUM_ROWS |
                                         Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  unit->set_limit(sel);

  for (iterator= history.new_iterator(), idx= 1;
       iterator != NULL;
       iterator= history.iterator_next(iterator), idx++)
  {
    prof= history.iterator_value(iterator);

    double query_time_usecs= prof->m_end_time_usecs - prof->m_start_time_usecs;

    if (unit->lim.check_offset(idx))
      continue;
    if (idx > unit->lim.get_select_limit())
      break;

    protocol->prepare_for_resend();
    protocol->store((uint32)(prof->profiling_query_id));
    protocol->store_double(query_time_usecs/(1000.0*1000),
                           (uint32) TIME_FLOAT_DIGITS-1);
    if (prof->query_source != NULL)
      protocol->store(prof->query_source, strlen(prof->query_source),
                      system_charset_info);
    else
      protocol->store_null();

    if (protocol->write())
      DBUG_RETURN(TRUE);
  }
  my_eof(thd);
  DBUG_RETURN(FALSE);
}

/**
  Fill the information schema table, "query_profile", as defined in show.cc .
  There are two ways to get to this function:  Selecting from the information
  schema, and a SHOW command.
*/
int PROFILING::fill_statistics_info(THD *thd_arg, TABLE_LIST *tables, Item *cond)
{
  DBUG_ENTER("PROFILING::fill_statistics_info");
  TABLE *table= tables->table;
  ulonglong row_number= 0;

  QUERY_PROFILE *query;
  /* Go through each query in this thread's stored history... */
  void *history_iterator;
  for (history_iterator= history.new_iterator();
       history_iterator != NULL;
       history_iterator= history.iterator_next(history_iterator))
  {
    query= history.iterator_value(history_iterator);

    /*
      Because we put all profiling info into a table that may be reordered, let
      us also include a numbering of each state per query.  The query_id and
      the "seq" together are unique.
    */
    ulong seq;

    void *entry_iterator;
    PROF_MEASUREMENT *entry, *previous= NULL;
    /* ...and for each query, go through all its state-change steps. */
    for (entry_iterator= query->entries.new_iterator();
         entry_iterator != NULL;
         entry_iterator= query->entries.iterator_next(entry_iterator),
         previous=entry, row_number++)
    {
      entry= query->entries.iterator_value(entry_iterator);
      seq= entry->m_seq;

      /* Skip the first.  We count spans of fence, not fence-posts. */
      if (previous == NULL) continue;

      if (thd_arg->lex->sql_command == SQLCOM_SHOW_PROFILE)
      {
        /*
          We got here via a SHOW command.  That means that we stored
          information about the query we wish to show and that isn't
          in a WHERE clause at a higher level to filter out rows we
          wish to exclude.

          Because that functionality isn't available in the server yet,
          we must filter here, at the wrong level.  Once one can con-
          struct where and having conditions at the SQL layer, then this
          condition should be ripped out.
        */
        if (thd_arg->lex->profile_query_id == 0) /* 0 == show final query */
        {
          if (query != last)
            continue;
        }
        else
        {
          if (thd_arg->lex->profile_query_id != query->profiling_query_id)
            continue;
        }
      }

      /* Set default values for this row. */
      restore_record(table, s->default_values);

      /*
        The order of these fields is set by the  query_profile_statistics_info
        array.
      */
      table->field[0]->store((ulonglong) query->profiling_query_id, TRUE);
      table->field[1]->store((ulonglong) seq, TRUE); /* the step in the sequence */
      /*
        This entry, n, has a point in time, T(n), and a status phrase, S(n).
        The status phrase S(n) describes the period of time that begins at
        T(n).  The previous status phrase S(n-1) describes the period of time
        that starts at T(n-1) and ends at T(n).  Since we want to describe the
        time that a status phrase took T(n)-T(n-1), this line must describe the
        previous status.
      */
      table->field[2]->store(previous->status, strlen(previous->status),
                             system_charset_info);

      my_decimal duration_decimal;
      double2my_decimal(E_DEC_FATAL_ERROR,
                        (entry->time_usecs-previous->time_usecs)/(1000.0*1000),
                        &duration_decimal);

      table->field[3]->store_decimal(&duration_decimal);


#ifdef HAVE_GETRUSAGE

      my_decimal cpu_utime_decimal, cpu_stime_decimal;

      double2my_decimal(E_DEC_FATAL_ERROR,
                        RUSAGE_DIFF_USEC(entry->rusage.ru_utime,
                                         previous->rusage.ru_utime) /
                                                        (1000.0*1000),
                        &cpu_utime_decimal);

      double2my_decimal(E_DEC_FATAL_ERROR,
                        RUSAGE_DIFF_USEC(entry->rusage.ru_stime,
                                         previous->rusage.ru_stime) /
                                                        (1000.0*1000),
                        &cpu_stime_decimal);

      table->field[4]->store_decimal(&cpu_utime_decimal);
      table->field[5]->store_decimal(&cpu_stime_decimal);
      table->field[4]->set_notnull();
      table->field[5]->set_notnull();
#elif defined(_WIN32)
      my_decimal cpu_utime_decimal, cpu_stime_decimal;

      double2my_decimal(E_DEC_FATAL_ERROR,
                        GetTimeDiffInSeconds(&entry->ftUser,
                                             &previous->ftUser),
                        &cpu_utime_decimal);
      double2my_decimal(E_DEC_FATAL_ERROR,
                        GetTimeDiffInSeconds(&entry->ftKernel,
                                             &previous->ftKernel),
                        &cpu_stime_decimal);

      // Store the result.
      table->field[4]->store_decimal(&cpu_utime_decimal);
      table->field[5]->store_decimal(&cpu_stime_decimal);
      table->field[4]->set_notnull();
      table->field[5]->set_notnull();
#else
      /* TODO: Add CPU-usage info for non-BSD systems */
#endif

#ifdef HAVE_GETRUSAGE
      table->field[6]->store((uint32)(entry->rusage.ru_nvcsw -
                             previous->rusage.ru_nvcsw));
      table->field[6]->set_notnull();
      table->field[7]->store((uint32)(entry->rusage.ru_nivcsw -
                             previous->rusage.ru_nivcsw));
      table->field[7]->set_notnull();
#else
      /* TODO: Add context switch info for non-BSD systems */
#endif

#ifdef HAVE_GETRUSAGE
      table->field[8]->store((uint32)(entry->rusage.ru_inblock -
                             previous->rusage.ru_inblock));
      table->field[8]->set_notnull();
      table->field[9]->store((uint32)(entry->rusage.ru_oublock -
                             previous->rusage.ru_oublock));
      table->field[9]->set_notnull();
#elif defined(_WIN32)
      ULONGLONG reads_delta = entry->io_count.ReadOperationCount - 
                              previous->io_count.ReadOperationCount;
      ULONGLONG writes_delta = entry->io_count.WriteOperationCount - 
                              previous->io_count.WriteOperationCount;

      table->field[8]->store((uint32)reads_delta);
      table->field[8]->set_notnull();

      table->field[9]->store((uint32)writes_delta);
      table->field[9]->set_notnull();
#else
      /* TODO: Add block IO info for non-BSD systems */
#endif

#ifdef HAVE_GETRUSAGE
      table->field[10]->store((uint32)(entry->rusage.ru_msgsnd -
                             previous->rusage.ru_msgsnd), true);
      table->field[10]->set_notnull();
      table->field[11]->store((uint32)(entry->rusage.ru_msgrcv -
                             previous->rusage.ru_msgrcv), true);
      table->field[11]->set_notnull();
#else
      /* TODO: Add message info for non-BSD systems */
#endif

#ifdef HAVE_GETRUSAGE
      table->field[12]->store((uint32)(entry->rusage.ru_majflt -
                             previous->rusage.ru_majflt), true);
      table->field[12]->set_notnull();
      table->field[13]->store((uint32)(entry->rusage.ru_minflt -
                             previous->rusage.ru_minflt), true);
      table->field[13]->set_notnull();
#elif defined(_WIN32)
      /* Windows APIs don't easily distinguish between hard and soft page
         faults, so we just fill the 'major' column and leave the second NULL.
      */
      table->field[12]->store((uint32)(entry->mem_count.PageFaultCount -
                             previous->mem_count.PageFaultCount), true);
      table->field[12]->set_notnull();
#else
      /* TODO: Add page fault info for non-BSD systems */
#endif

#ifdef HAVE_GETRUSAGE
      table->field[14]->store((uint32)(entry->rusage.ru_nswap -
                             previous->rusage.ru_nswap), true);
      table->field[14]->set_notnull();
#else
      /* TODO: Add swap info for non-BSD systems */
#endif

      /* Emit the location that started this step, not that ended it. */
      if ((previous->function != NULL) && (previous->file != NULL))
      {
        table->field[15]->store(previous->function, strlen(previous->function),
                        system_charset_info);
        table->field[15]->set_notnull();
        table->field[16]->store(previous->file, strlen(previous->file), system_charset_info);
        table->field[16]->set_notnull();
        table->field[17]->store(previous->line, true);
        table->field[17]->set_notnull();
      }

      if (schema_table_store_record(thd_arg, table))
        DBUG_RETURN(1);

    }
  }

  DBUG_RETURN(0);
}


void PROFILING::reset()
{
  enabled= (thd->variables.option_bits & OPTION_PROFILING) != 0;
}
#endif /* ENABLED_PROFILING */

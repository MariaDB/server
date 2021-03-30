/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.

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


/* Write some debug info */

#include <my_global.h>
#include "sql_priv.h"
#include "unireg.h"
#include "sql_test.h"
#include "sql_base.h"
#include "sql_show.h" // calc_sum_of_all_status
#include "sql_select.h"
#include "keycaches.h"
#include <hash.h>
#include <thr_alarm.h>
#if defined(HAVE_MALLINFO) && defined(HAVE_MALLOC_H)
#include <malloc.h>
#elif defined(HAVE_MALLINFO) && defined(HAVE_SYS_MALLOC_H)
#include <sys/malloc.h>
#endif

#ifdef HAVE_EVENT_SCHEDULER
#include "events.h"
#endif

static const char *lock_descriptions[] =
{
  /* TL_UNLOCK                  */  "No lock",
  /* TL_READ_DEFAULT            */  NULL,
  /* TL_READ                    */  "Low priority read lock",
  /* TL_READ_WITH_SHARED_LOCKS  */  "Shared read lock",
  /* TL_READ_HIGH_PRIORITY      */  "High priority read lock",
  /* TL_READ_NO_INSERT          */  "Read lock without concurrent inserts",
  /* TL_WRITE_ALLOW_WRITE       */  "Write lock that allows other writers",
  /* TL_WRITE_CONCURRENT_INSERT */  "Concurrent insert lock",
  /* TL_WRITE_DELAYED           */  "Lock used by delayed insert",
  /* TL_WRITE_DEFAULT           */  NULL,
  /* TL_WRITE_LOW_PRIORITY      */  "Low priority write lock",
  /* TL_WRITE                   */  "High priority write lock",
  /* TL_WRITE_ONLY              */  "Highest priority write lock"
};


#ifndef DBUG_OFF

void
print_where(COND *cond,const char *info, enum_query_type query_type)
{
  char buff[1024];
  String str(buff,(uint32) sizeof(buff), system_charset_info);
  str.length(0);
  str.extra_allocation(1024);
  if (cond)
    cond->print(&str, query_type);

  DBUG_LOCK_FILE;
  (void) fprintf(DBUG_FILE,"\nWHERE:(%s) %p ", info, cond);
  (void) fputs(str.c_ptr_safe(),DBUG_FILE);
  (void) fputc('\n',DBUG_FILE);
  DBUG_UNLOCK_FILE;
}

#ifdef EXTRA_DEBUG
	/* This is for debugging purposes */
static my_bool print_cached_tables_callback(TDC_element *element,
                                            void *arg __attribute__((unused)))
{
  TABLE *entry;

  mysql_mutex_lock(&element->LOCK_table_share);
  All_share_tables_list::Iterator it(element->all_tables);
  while ((entry= it++))
  {
    THD *in_use= entry->in_use;
    printf("%-14.14s %-32s%6lu%8ld%6d  %s\n",
           entry->s->db.str, entry->s->table_name.str,
           (ulong) element->version,
           in_use ? (long) in_use->thread_id : (long) 0,
           entry->db_stat ? 1 : 0,
           in_use ? lock_descriptions[(int)entry->reginfo.lock_type] :
                    "Not in use");
  }
  mysql_mutex_unlock(&element->LOCK_table_share);
  return FALSE;
}


static void print_cached_tables(void)
{
  compile_time_assert(TL_WRITE_ONLY+1 == array_elements(lock_descriptions));

  /* purecov: begin tested */
  puts("DB             Table                            Version  Thread  Open  Lock");

  tdc_iterate(0, (my_hash_walk_action) print_cached_tables_callback, NULL, true);

  printf("\nCurrent refresh version: %ld\n",
         (long) tdc_refresh_version());
  fflush(stdout);
  /* purecov: end */
  return;
}
#endif


void TEST_filesort(SORT_FIELD *sortorder,uint s_length)
{
  char buff[256],buff2[256];
  String str(buff,sizeof(buff),system_charset_info);
  String out(buff2,sizeof(buff2),system_charset_info);
  const char *sep;
  DBUG_ENTER("TEST_filesort");

  out.length(0);
  for (sep=""; s_length-- ; sortorder++, sep=" ")
  {
    out.append(sep);
    if (sortorder->reverse)
      out.append('-');
    if (sortorder->field)
    {
      if (sortorder->field->table_name)
      {
	out.append(*sortorder->field->table_name);
	out.append('.');
      }
      out.append(sortorder->field->field_name ? sortorder->field->field_name:
		 "tmp_table_column");
    }
    else
    {
      str.length(0);
      sortorder->item->print(&str, QT_ORDINARY);
      out.append(str);
    }
  }
  DBUG_LOCK_FILE;
  (void) fputs("\nInfo about FILESORT\n",DBUG_FILE);
  fprintf(DBUG_FILE,"Sortorder: %s\n",out.c_ptr_safe());
  DBUG_UNLOCK_FILE;
  DBUG_VOID_RETURN;
}


void
TEST_join(JOIN *join)
{
  uint ref;
  int i;
  List_iterator<JOIN_TAB_RANGE> it(join->join_tab_ranges);
  JOIN_TAB_RANGE *jt_range;
  DBUG_ENTER("TEST_join");

  DBUG_LOCK_FILE;
  (void) fputs("\nInfo about JOIN\n",DBUG_FILE);
  while ((jt_range= it++))
  {
    /*
      Assemble results of all the calls to full_name() first,
      in order not to garble the tabular output below.
    */
    String ref_key_parts[MAX_TABLES];
    int tables_in_range= (int)(jt_range->end - jt_range->start);
    for (i= 0; i < tables_in_range; i++)
    {
      JOIN_TAB *tab= jt_range->start + i;
      for (ref= 0; ref < tab->ref.key_parts; ref++)
      {
        ref_key_parts[i].append(tab->ref.items[ref]->full_name());
        ref_key_parts[i].append("  ");
      }
    }

    for (i= 0; i < tables_in_range; i++)
    {
      JOIN_TAB *tab= jt_range->start + i;
      TABLE *form=tab->table;
      char key_map_buff[128];
      fprintf(DBUG_FILE,"%-16.16s  type: %-7s  q_keys: %s  refs: %d  key: %d  len: %d\n",
	    form->alias.c_ptr(),
              join_type_str[tab->type],
              tab->keys.print(key_map_buff),
              tab->ref.key_parts,
              tab->ref.key,
              tab->ref.key_length);
      if (tab->select)
      {
        char buf[MAX_KEY/8+1];
        if (tab->use_quick == 2)
          fprintf(DBUG_FILE,
                  "                  quick select checked for each record (keys: %s)\n",
                  tab->select->quick_keys.print(buf));
        else if (tab->select->quick)
        {
          fprintf(DBUG_FILE, "                  quick select used:\n");
          tab->select->quick->dbug_dump(18, FALSE);
        }
        else
          (void)fputs("                  select used\n",DBUG_FILE);
      }
      if (tab->ref.key_parts)
      {
        fprintf(DBUG_FILE,
              "                  refs:  %s\n", ref_key_parts[i].c_ptr_safe());
      }
    }
    (void)fputs("\n",DBUG_FILE);
  }
  DBUG_UNLOCK_FILE;
  DBUG_VOID_RETURN;
}


#define FT_KEYPART   (MAX_FIELDS+10)

static void print_keyuse(KEYUSE *keyuse)
{
  char buff[256];
  char buf2[64]; 
  const char *fieldname;
  JOIN_TAB *join_tab= keyuse->table->reginfo.join_tab;
  KEY *key_info= join_tab->get_keyinfo_by_key_no(keyuse->key);
  String str(buff,(uint32) sizeof(buff), system_charset_info);
  str.length(0);
  keyuse->val->print(&str, QT_ORDINARY);
  str.append('\0');
  if (keyuse->is_for_hash_join())
    fieldname= keyuse->table->field[keyuse->keypart]->field_name;
  else if (keyuse->keypart == FT_KEYPART)
    fieldname= "FT_KEYPART";
  else
    fieldname= key_info->key_part[keyuse->keypart].field->field_name;
  ll2str(keyuse->used_tables, buf2, 16, 0); 
  fprintf(DBUG_FILE, "KEYUSE: %s.%s=%s  optimize: %u  used_tables: %s "
          "ref_table_rows: %lu  keypart_map: %0lx\n",
          keyuse->table->alias.c_ptr(), fieldname, str.ptr(),
          (uint) keyuse->optimize, buf2, (ulong) keyuse->ref_table_rows, 
          (ulong) keyuse->keypart_map);
}


/* purecov: begin inspected */
void print_keyuse_array(DYNAMIC_ARRAY *keyuse_array)
{
  DBUG_LOCK_FILE;
  fprintf(DBUG_FILE, "KEYUSE array (%d elements)\n", keyuse_array->elements);
  for(uint i=0; i < keyuse_array->elements; i++)
    print_keyuse((KEYUSE*)dynamic_array_ptr(keyuse_array, i));
  DBUG_UNLOCK_FILE;
}


/* 
  Print the current state during query optimization.

  SYNOPSIS
    print_plan()
    join         pointer to the structure providing all context info for
                 the query
    read_time    the cost of the best partial plan
    record_count estimate for the number of records returned by the best
                 partial plan
    idx          length of the partial QEP in 'join->positions';
                 also an index in the array 'join->best_ref';
    info         comment string to appear above the printout

  DESCRIPTION
    This function prints to the log file DBUG_FILE the members of 'join' that
    are used during query optimization (join->positions, join->best_positions,
    and join->best_ref) and few other related variables (read_time,
    record_count).
    Useful to trace query optimizer functions.

  RETURN
    None
*/

void
print_plan(JOIN* join, uint idx, double record_count, double read_time,
           double current_read_time, const char *info)
{
  uint i;
  POSITION pos;
  JOIN_TAB *join_table;
  JOIN_TAB **plan_nodes;
  TABLE*   table;

  if (info == 0)
    info= "";

  DBUG_LOCK_FILE;
  if (join->best_read == DBL_MAX)
  {
    fprintf(DBUG_FILE,
            "%s; idx: %u  best: DBL_MAX  atime: %g  itime: %g  count: %g\n",
            info, idx, current_read_time, read_time, record_count);
  }
  else
  {
    fprintf(DBUG_FILE,
            "%s; idx :%u  best: %g  accumulated: %g  increment: %g  count: %g\n",
            info, idx, join->best_read, current_read_time, read_time,
            record_count);
  }

  /* Print the tables in JOIN->positions */
  fputs("     POSITIONS: ", DBUG_FILE);
  for (i= 0; i < idx ; i++)
  {
    pos = join->positions[i];
    table= pos.table->table;
    if (table)
      fputs(table->s->table_name.str, DBUG_FILE);
    fputc(' ', DBUG_FILE);
  }
  fputc('\n', DBUG_FILE);

  /*
    Print the tables in JOIN->best_positions only if at least one complete plan
    has been found. An indicator for this is the value of 'join->best_read'.
  */
  if (join->best_read < DBL_MAX)
  {
    fputs("BEST_POSITIONS: ", DBUG_FILE);
    for (i= 0; i < idx ; i++)
    {
      pos= join->best_positions[i];
      table= pos.table->table;
      if (table)
        fputs(table->s->table_name.str, DBUG_FILE);
      fputc(' ', DBUG_FILE);
    }
  }
  fputc('\n', DBUG_FILE);

  /* Print the tables in JOIN->best_ref */
  fputs("      BEST_REF: ", DBUG_FILE);
  for (plan_nodes= join->best_ref ; *plan_nodes ; plan_nodes++)
  {
    join_table= (*plan_nodes);
    fputs(join_table->table->s->table_name.str, DBUG_FILE);
    fprintf(DBUG_FILE, "(%lu,%lu,%lu)",
            (ulong) join_table->found_records,
            (ulong) join_table->records,
            (ulong) join_table->read_time);
    fputc(' ', DBUG_FILE);
  }
  fputc('\n', DBUG_FILE);

  DBUG_UNLOCK_FILE;
}


void print_sjm(SJ_MATERIALIZATION_INFO *sjm)
{
  DBUG_LOCK_FILE;
  fprintf(DBUG_FILE, "\nsemi-join nest{\n");
  fprintf(DBUG_FILE, "  tables { \n");
  for (uint i= 0;i < sjm->tables; i++)
  {
    fprintf(DBUG_FILE, "    %s%s\n", 
            sjm->positions[i].table->table->alias.c_ptr(),
            (i == sjm->tables -1)? "": ",");
  }
  fprintf(DBUG_FILE, "  }\n");
  fprintf(DBUG_FILE, "  materialize_cost= %g\n",
          sjm->materialization_cost.total_cost());
  fprintf(DBUG_FILE, "  rows= %g\n", sjm->rows);
  fprintf(DBUG_FILE, "}\n");
  DBUG_UNLOCK_FILE;
}
/* purecov: end */

/*
  Debugging help: force List<...>::elem function not be removed as unused.
*/
Item* (List<Item>::*dbug_list_item_elem_ptr)(int)= &List<Item>::elem;
Item_equal* (List<Item_equal>::*dbug_list_item_equal_elem_ptr)(int)=
  &List<Item_equal>::elem;
TABLE_LIST* (List<TABLE_LIST>::*dbug_list_table_list_elem_ptr)(int) =
  &List<TABLE_LIST>::elem;

#endif

typedef struct st_debug_lock
{
  ulong thread_id;
  char table_name[FN_REFLEN];
  bool waiting;
  const char *lock_text;
  enum thr_lock_type type;
} TABLE_LOCK_INFO;

C_MODE_START
static int dl_compare(const void *p1, const void *p2)
{
  TABLE_LOCK_INFO *a, *b;

  a= (TABLE_LOCK_INFO *) p1;
  b= (TABLE_LOCK_INFO *) p2;

  if (a->thread_id > b->thread_id)
    return 1;
  if (a->thread_id < b->thread_id)
    return -1;
  if (a->waiting == b->waiting)
    return 0;
  else if (a->waiting)
    return -1;
  return 1;
}
C_MODE_END


static void push_locks_into_array(DYNAMIC_ARRAY *ar, THR_LOCK_DATA *data,
				  bool wait, const char *text)
{
  if (data)
  {
    TABLE *table=(TABLE *)data->debug_print_param;
    if (table && table->s->tmp_table == NO_TMP_TABLE)
    {
      TABLE_LOCK_INFO table_lock_info;
      table_lock_info.thread_id= (ulong)table->in_use->thread_id;
      memcpy(table_lock_info.table_name, table->s->table_cache_key.str,
	     table->s->table_cache_key.length);
      table_lock_info.table_name[strlen(table_lock_info.table_name)]='.';
      table_lock_info.waiting=wait;
      table_lock_info.lock_text=text;
      // lock_type is also obtainable from THR_LOCK_DATA
      table_lock_info.type=table->reginfo.lock_type;
      (void) push_dynamic(ar,(uchar*) &table_lock_info);
    }
  }
}


/*
  Regarding MERGE tables:

  For now, the best option is to use the common TABLE *pointer for all
  cases;  The drawback is that for MERGE tables we will see many locks
  for the merge tables even if some of them are for individual tables.

  The way to solve this is to add to 'THR_LOCK' structure a pointer to
  the filename and use this when printing the data.
  (We can for now ignore this and just print the same name for all merge
  table parts;  Please add the above as a comment to the display_lock
  function so that we can easily add this if we ever need this.
*/

static void display_table_locks(void)
{
  LIST *list;
  void *saved_base;
  DYNAMIC_ARRAY saved_table_locks;

  (void) my_init_dynamic_array(&saved_table_locks,sizeof(TABLE_LOCK_INFO),
                               tc_records() + 20, 50, MYF(0));
  mysql_mutex_lock(&THR_LOCK_lock);
  for (list= thr_lock_thread_list; list; list= list_rest(list))
  {
    THR_LOCK *lock=(THR_LOCK*) list->data;

    mysql_mutex_lock(&lock->mutex);
    push_locks_into_array(&saved_table_locks, lock->write.data, FALSE,
			  "Locked - write");
    push_locks_into_array(&saved_table_locks, lock->write_wait.data, TRUE,
			  "Waiting - write");
    push_locks_into_array(&saved_table_locks, lock->read.data, FALSE,
			  "Locked - read");
    push_locks_into_array(&saved_table_locks, lock->read_wait.data, TRUE,
			  "Waiting - read");
    mysql_mutex_unlock(&lock->mutex);
  }
  mysql_mutex_unlock(&THR_LOCK_lock);

  if (!saved_table_locks.elements)
    goto end;

  saved_base= dynamic_element(&saved_table_locks, 0, TABLE_LOCK_INFO *);
  my_qsort(saved_base, saved_table_locks.elements, sizeof(TABLE_LOCK_INFO),
           dl_compare);
  freeze_size(&saved_table_locks);

  puts("\nThread database.table_name          Locked/Waiting        Lock_type\n");

  unsigned int i;
  for (i=0 ; i < saved_table_locks.elements ; i++)
  {
    TABLE_LOCK_INFO *dl_ptr=dynamic_element(&saved_table_locks,i,TABLE_LOCK_INFO*);
    printf("%-8ld%-28.28s%-22s%s\n",
	   dl_ptr->thread_id,dl_ptr->table_name,dl_ptr->lock_text,lock_descriptions[(int)dl_ptr->type]);
  }
  puts("\n\n");
end:
  delete_dynamic(&saved_table_locks);
}

C_MODE_START
static int print_key_cache_status(const char *name, KEY_CACHE *key_cache,
                                  void *unused __attribute__((unused)))
{
  char llbuff1[22];
  char llbuff2[22];
  char llbuff3[22];
  char llbuff4[22];

  if (!key_cache->key_cache_inited)
  {
    printf("%s: Not in use\n", name);
  }
  else
  {
    KEY_CACHE_STATISTICS stats;
    get_key_cache_statistics(key_cache, 0, &stats);

    printf("%s\n\
Buffer_size:    %10lu\n\
Block_size:     %10lu\n\
Division_limit: %10lu\n\
Age_threshold:  %10lu\n\
Partitions:     %10lu\n\
blocks used:    %10lu\n\
not flushed:    %10lu\n\
w_requests:     %10s\n\
writes:         %10s\n\
r_requests:     %10s\n\
reads:          %10s\n\n",
	   name,
	   (ulong)key_cache->param_buff_size,
           (ulong)key_cache->param_block_size,
	   (ulong)key_cache->param_division_limit,
           (ulong)key_cache->param_age_threshold,
           (ulong)key_cache->param_partitions,
	   (ulong)stats.blocks_used,
           (ulong)stats.blocks_changed,
	   llstr(stats.write_requests,llbuff1),
           llstr(stats.writes,llbuff2),
	   llstr(stats.read_requests,llbuff3),
           llstr(stats.reads,llbuff4));
  }
  return 0;
}
C_MODE_END


void mysql_print_status()
{
  char current_dir[FN_REFLEN];
  STATUS_VAR tmp;
  uint count;

  tmp= global_status_var;
  count= calc_sum_of_all_status(&tmp);
  printf("\nStatus information:\n\n");
  (void) my_getwd(current_dir, sizeof(current_dir),MYF(0));
  printf("Current dir: %s\n", current_dir);
  printf("Running threads: %d  Cached threads: %lu  Stack size: %ld\n",
         count, cached_thread_count,
	 (long) my_thread_stack_size);
#ifdef EXTRA_DEBUG
  thr_print_locks();				// Write some debug info
  print_cached_tables();
#endif
  /* Print key cache status */
  puts("\nKey caches:");
  process_key_caches(print_key_cache_status, 0);
  printf("\nhandler status:\n\
read_key:   %10lu\n\
read_next:  %10lu\n\
read_rnd    %10lu\n\
read_first: %10lu\n\
write:      %10lu\n\
delete      %10lu\n\
update:     %10lu\n",
	 tmp.ha_read_key_count,
	 tmp.ha_read_next_count,
	 tmp.ha_read_rnd_count,
	 tmp.ha_read_first_count,
	 tmp.ha_write_count,
	 tmp.ha_delete_count,
	 tmp.ha_update_count);
  printf("\nTable status:\n\
Opened tables: %10lu\n\
Open tables:   %10lu\n\
Open files:    %10lu\n\
Open streams:  %10lu\n",
	 tmp.opened_tables,
	 (ulong) tc_records(),
	 (ulong) my_file_opened,
	 (ulong) my_stream_opened);

#ifndef DONT_USE_THR_ALARM
  ALARM_INFO alarm_info;
  thr_alarm_info(&alarm_info);
  printf("\nAlarm status:\n\
Active alarms:   %u\n\
Max used alarms: %u\n\
Next alarm time: %lu\n",
	 alarm_info.active_alarms,
	 alarm_info.max_used_alarms,
	(ulong)alarm_info.next_alarm_time);
#endif
  display_table_locks();
#if defined(HAVE_MALLINFO2)
  struct mallinfo2 info = mallinfo2();
#elif defined(HAVE_MALLINFO)
  struct mallinfo info= mallinfo();
#endif
#if defined(HAVE_MALLINFO) || defined(HAVE_MALLINFO2)
  char llbuff[10][22];
  printf("\nMemory status:\n\
Non-mmapped space allocated from system: %s\n\
Number of free chunks:                   %lu\n\
Number of fastbin blocks:                %lu\n\
Number of mmapped regions:               %lu\n\
Space in mmapped regions:                %s\n\
Maximum total allocated space:           %s\n\
Space available in freed fastbin blocks: %s\n\
Total allocated space:                   %s\n\
Total free space:                        %s\n\
Top-most, releasable space:              %s\n\
Estimated memory (with thread stack):    %s\n\
Global memory allocated by server:       %s\n\
Memory allocated by threads:             %s\n",
	 llstr(info.arena,   llbuff[0]),
	 (ulong) info.ordblks,
	 (ulong) info.smblks,
	 (ulong) info.hblks,
	 llstr(info.hblkhd,   llbuff[1]),
	 llstr(info.usmblks,  llbuff[2]),
	 llstr(info.fsmblks,  llbuff[3]),
	 llstr(info.uordblks, llbuff[4]),
	 llstr(info.fordblks, llbuff[5]),
	 llstr(info.keepcost, llbuff[6]),
	 llstr((count + cached_thread_count)* my_thread_stack_size + info.hblkhd + info.arena, llbuff[7]),
         llstr(tmp.global_memory_used, llbuff[8]),
         llstr(tmp.local_memory_used, llbuff[9]));

#endif

#ifdef HAVE_EVENT_SCHEDULER
  Events::dump_internal_status();
#endif
  puts("");
  fflush(stdout);
}

/* Copyright (c) 2023 MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/*
  This program simulates the MariaDB query process execution using
  the SCAN, EQ_REF,  REF and join_cache (CACHE) row lookup methods.

  The purpose is to verify that 'prev_record_reads()' function correctly
  estimates the number of lookups we have to do for EQ_REF access
  assuming we have 'one-row-cache' before the lookup.

  The logic for the prev_record_reads() function in this file should
  match the logic in sql_select.cc::prev_record_reads() in MariaDB 11.0
  and above.

  The program generates first a randomized plan with the above
  methods, then executes a full 'query' processing and then lastly
  checks that the number of EQ_REF engine lookups matches the
  estimated number of lookups.

  If the number of estimated lookups are not exact, the plan and
  lookup numbers are printed. That a plan is printed is not to be
  regarded as a failure. It's a failure only of the number of engine
  calls are far greater than the number of estimated lookups.

  Note that the estimated number of lookups are exact only if CACHE
  refills == 1 and if the EQ_REF table only depends on one earlier
  table.
*/

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>

#define TABLES 21
#define DEFAULT_TABLES 10
#define CACHED_ROWS 10000
#define unlikely(A) A

enum JOIN_TYPE { SCAN, EQ_REF,  REF, CACHE };
const char *type[]= { "SCAN", "EQ_REF", "REF", "CACHE"};

typedef unsigned long long DEPEND;
typedef unsigned int uint;
typedef unsigned long long ulonglong;

struct TABLE
{
  ulonglong data;
  JOIN_TYPE type;
  DEPEND map;
  DEPEND ref_depend_map;
  uint records_in_table;
  uint matching_records;
  uint last_key;
  ulonglong lookups;
  ulonglong *cache;                             // join cache
  ulong cached_records;
  ulong flushed_caches;
};

struct POSITION
{
  TABLE *table;
  JOIN_TYPE type;
  double records;
  double record_count;
  double records_out;
  double prev_record_read;
  double same_keys;
  ulong refills;
};

uint opt_tables= DEFAULT_TABLES;
bool verbose=0;
uint rand_init;
struct TABLE table[TABLES];
struct POSITION positions[TABLES];

void do_select(uint table_index);


static void
prev_record_reads(POSITION *position, uint idx, DEPEND found_ref,
                  double record_count)
{
  double found= 1.0;
  POSITION *pos_end= position - 1;
  POSITION *cur_pos= position + idx;

  /* Safety against const tables */
  if (!found_ref)
    goto end;

  for (POSITION *pos= cur_pos-1; pos != pos_end; pos--)
  {
    if (found_ref & pos->table->map)
    {
      found_ref&= ~pos->table->map;

      /* Found dependent table */
      if (pos->type == EQ_REF)
      {
        if (!found_ref)
          found*= pos->same_keys;
      }
      else if (pos->type == CACHE)
      {
        if (!found_ref)
          found*= pos->record_count / pos->refills;
      }
      break;
    }
    if (pos->type != CACHE)
    {
      /*
        We are not depending on the current table
        There are 'records_out' rows with identical rows
        value for our depending tables.
        We are ignoring join_cache as in this case the
        preceding tables row combination can change for
        each call.
      */
      found*= pos->records_out;
    }
    else
      found/= pos->refills;
  }

end:
  cur_pos->record_count= record_count;
  cur_pos->same_keys= found;
  assert(record_count >= found);

  if (unlikely(found <= 1.0))
    cur_pos->prev_record_read= record_count;
  else if (unlikely(found > record_count))
    cur_pos->prev_record_read=1;
  else
    cur_pos->prev_record_read= record_count / found;
  return;
}


void cleanup()
{
  for (uint i= 0; i < opt_tables ; i++)
  {
    free(table[i].cache);
    table[i].cache= 0;
  }
}


void intialize_tables()
{
  int eq_ref_tables;

restart:
  eq_ref_tables= 0;
  for (uint i= 0; i < opt_tables ; i++)
  {
    if (i == 0)
      table[i].type= SCAN;
    else
      table[i].type= (JOIN_TYPE) (rand() % 4);
    table[i].records_in_table= rand() % 5+3;
    table[i].matching_records= 2 + rand() % 3;
    table[i].map= (DEPEND) 1 << i;
    table[i].ref_depend_map= 0;

/* The following is for testing */
#ifdef FORCE_COMB
    if (i == 5 || i == 6)
    {
      table[i].type= REF;
      table[i].matching_records= 5;
    }
#endif
    if (table[i].type != SCAN)
    {
      /* This just to make do_select a bit easier */
      table[i].ref_depend_map=  ((DEPEND) 1) << (rand() % i);
      if (rand() & 1)
      {
        uint second_depend= rand() % i;
        if (!(table[i].ref_depend_map & second_depend))
          table[i].ref_depend_map|= ((DEPEND) 1) << second_depend;
      }
    }

    if (table[i].type == EQ_REF)
    {
      table[i].matching_records= 1;
      eq_ref_tables++;
    }
    else if (table[i].type != REF)
      table[i].matching_records= table[i].records_in_table;

    table[i].last_key= 0;
    table[i].lookups= 0;
    table[i].cached_records= 0;
    table[i].flushed_caches= 0;
    table[i].cache= 0;
    if (table[i].type == CACHE)
      table[i].cache= (ulonglong*) malloc(CACHED_ROWS *
                                          sizeof(table[i].data) * i);
  }

  /* We must have at least one EQ_REF table */
  if (!eq_ref_tables)
  {
    cleanup();
    goto restart;
  }
}


void optimize_tables()
{
  double record_count= 1.0, records;

  for (uint i= 0; i < opt_tables ; i++)
  {
    TABLE *tab= table+i;
    positions[i].refills= 0;

    switch (tab->type) {
    case SCAN:
      records= tab->records_in_table;
      break;
    case EQ_REF:
      records= 1.0;
      prev_record_reads(positions, i, tab->ref_depend_map, record_count);
      break;
    case REF:
      records= tab->matching_records;
      break;
    case CACHE:
      records= tab->records_in_table;
      positions[i].refills= (record_count + CACHED_ROWS-1)/ CACHED_ROWS;
      break;
    default:
      assert(0);
    }
    positions[i].table= table + i;
    positions[i].type= table[i].type;
    positions[i].records= records;
    positions[i].record_count= record_count;
    positions[i].records_out= records;

    record_count*= records;
  }
}



void process_join_cache(TABLE *tab, uint table_index)
{
  if (!tab->cached_records)
    return;

#ifdef PRINT_CACHE
  putc('>', stdout);
  for (uint k= 0 ; k < table_index ; k++)
  {
    printf("%8lld ", tab->cache[k]);
  }
  putc('\n',stdout);
  putc('<', stdout);
  for (uint k= 0 ; k < table_index ; k++)
  {
    printf("%8lld ", tab->cache[k+(tab->cached_records-1)*table_index]);
  }
  putc('\n',stdout);
#endif

  for (uint k= 0 ; k < tab->records_in_table; k++)
  {
    table[table_index].data= k+1;
    ulonglong *cache= tab->cache;
    for (uint i= 0 ; i < tab->cached_records ; i++)
    {
      for (uint j= 0 ; j < table_index ; j++)
        table[j].data= *cache++;
      do_select(table_index+1);
    }
  }
  tab->flushed_caches++;
  tab->cached_records= 0;
}

/*
  Calculate a key depending on multiple tables
*/

ulonglong calc_ref_key(DEPEND depend_map)
{
  ulonglong value= 1;
  TABLE *t= table;

  do
  {
    if (t->map & depend_map)
    {
      depend_map&= ~t->map;
      value*= t->data;
    }
    t++;
  } while (depend_map);
  return value;
}


void do_select(uint table_index)
{
  if (table_index == opt_tables)
    return;

  TABLE *tab= table + table_index;
  switch (tab->type) {
  case SCAN:
    for (uint i= 1 ; i <= tab->records_in_table ; i++)
    {
      tab->data= i;
      do_select(table_index+1);
    }
    break;
  case REF:
  {
    ulonglong ref_key= calc_ref_key(tab->ref_depend_map);
    for (uint i=1 ; i <= tab->matching_records ; i++)
    {
      tab->data= ref_key * tab->matching_records + i;
      do_select(table_index+1);
    }
    break;
  }
  case EQ_REF:
  {
    ulonglong ref_key= calc_ref_key(tab->ref_depend_map);
    if (ref_key != tab->last_key)
    {
      tab->lookups++;
#ifdef PRINT_EQ_KEY
      if (table_index == 9)
        printf("ref_key: %lld\n", ref_key);
#endif
      tab->last_key= ref_key;
      tab->data= ref_key * tab->matching_records;
    }
    else
    {
      assert(tab->lookups != 0);
    }
    do_select(table_index+1);
    break;
  }
  case CACHE:
  {
    ulonglong *cache= tab->cache + tab->cached_records * table_index;
    for (uint i= 0 ; i <= table_index ; i++)
      *cache++ = table[i].data;
    if (++tab->cached_records == CACHED_ROWS)
      process_join_cache(tab, table_index);
    break;
  }
  default:
    break;
  }
  return;
}


void do_select_end(uint table_index)
{
  if (table_index == opt_tables)
    return;

  TABLE *tab= table + table_index;
  switch (tab->type) {
  case CACHE:
    process_join_cache(tab, table_index);
    break;
  default:
    break;
  }
  do_select_end(table_index+1);
}


void execute()
{
  do_select(0);
  do_select_end(0);
}

int check_prev_records()
{
  int errors= 0;
  for (uint i= 0; i < opt_tables ; i++)
  {
    TABLE *tab= table + i;
    if (tab->type == EQ_REF)
    {
      if (positions[i].prev_record_read != (double) tab->lookups)
      {
        fprintf(stdout, "table: %d  lookups: %lld  prev_record_read: %g\n",
                i, tab->lookups, positions[i].prev_record_read);
        errors++;
      }
    }
  }
  if (errors || verbose)
  {
    fprintf(stdout, "tables:     %u\n", opt_tables);
    fprintf(stdout, "rand_init:  %u\n", rand_init);
    fprintf(stdout, "cache_size: %u\n", (uint) CACHED_ROWS);
    for (uint i= 0; i < opt_tables ; i++)
    {
      TABLE *tab= table + i;
      fprintf(stdout, "table: %2d (%3lx)  type: %-6s  comb: %3lg  out: %2lg  lookups: %lld  prev: %lg  depend: %llx\n",
              i, (uint) 1 << i, type[tab->type], positions[i].record_count,
              positions[i].records_out, tab->lookups,
              positions[i].prev_record_read, tab->ref_depend_map);
    }
  }
  return errors;
}


int main(int argc, char **argv)
{
  if (argc > 1)
  {
    opt_tables=atoi(argv[1]);
    if (opt_tables <= 3)
      opt_tables= 3;
    if (opt_tables > TABLES)
      opt_tables= TABLES;
  }
  if (argc > 2)
    rand_init= atoi(argv[2]);
  else
    rand_init= (uint) time(0);
  srand(rand_init);

  intialize_tables();
  optimize_tables();
  execute();
  cleanup();
  exit(check_prev_records() > 0);
}

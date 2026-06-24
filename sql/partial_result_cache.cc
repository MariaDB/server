/* Copyright (c) 2026, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1335  USA */

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_select.h"
#include "partial_result_cache.h"
#include "my_json_writer.h"
#include "optimizer_defaults.h"
#include "opt_hints.h"

#include <new>

ulong partial_result_cache_hit;
ulong partial_result_cache_miss;
ulong partial_result_cache_rows_cached;
ulong partial_result_cache_rows_replayed;
ulong partial_result_cache_bypass;

static int join_read_partial_result_cache_key(JOIN_TAB *tab);
static int join_read_partial_result_cache_next_same(READ_RECORD *info);

// Estimates how often repeated outer rows can reuse an inner ref result.
//
// MariaDB does not retain a precise distinct-value estimate for the outer ref
// expressions at executor setup. Use a conservative lower bound where one
// probe per key is a miss and subsequent probes of the same key can hit.
static double
partial_result_cache_estimate_hit_ratio(JOIN_TAB *tab, double *saved_cost)
{
  double fanout= MY_MAX(tab->records_read, 1.0);
  double probes= tab->partial_join_cardinality / fanout;

  if (probes < 2.0)
  {
    *saved_cost= 0.0;
    return 0.0;
  }

  double hit_ratio= (probes - 1.0) / probes;
  *saved_cost= tab->join_read_time * hit_ratio;
  return hit_ratio;
}


// Emits the PTRC choice into optimizer trace when tracing is active.
static void
trace_partial_result_cache_choice(JOIN_TAB *tab, bool chosen,
                                  const char *cause)
{
  THD *thd= tab->join->thd;
  if (likely(!thd->trace_started()))
    return;

  Json_writer_object trace_wrapper(thd);
  Json_writer_object trace(thd, "partial_result_cache");
  trace.add_table_name(tab);
  trace.add("chosen", chosen);
  trace.add("cause", cause);
  trace.add("estimated_hit_ratio",
            tab->partial_result_cache_estimated_hit_ratio);
  trace.add("estimated_saved_cost",
            tab->partial_result_cache_estimated_saved_cost);
}


// Checks whether the current JOIN_TAB can be wrapped by PTRC.
//
// Phase 1 intentionally limits the supported surface to plain nested-loop ref
// access. Join-buffer algorithms, outer/semi join inner tables, BLOB rows and
// locking reads keep the original executor path.
static bool
partial_result_cache_is_eligible(JOIN_TAB *tab, JOIN_TAB *first_tab,
                                 uint join_cache_level, const char **cause)
{
  THD *thd= tab->join->thd;
  hint_state prc_join_hint= hint_state::NOT_PRESENT;

  if (tab->tab_list)
    prc_join_hint= hint_table_state(thd, tab->tab_list, PRC_JOIN_HINT_ENUM);

  if (prc_join_hint == hint_state::DISABLED)
  {
    *cause= "disabled by NO_PRC_JOIN hint";
    return false;
  }

  if (prc_join_hint == hint_state::NOT_PRESENT &&
      !(thd->variables.optimizer_switch &
        OPTIMIZER_SWITCH_PARTIAL_RESULT_CACHE))
  {
    *cause= "disabled by optimizer_switch";
    return false;
  }

  if (!thd->variables.partial_result_cache_max_mem_size)
  {
    *cause= "memory limit is zero";
    return false;
  }

  if (tab == first_tab)
  {
    *cause= "first non-const table";
    return false;
  }

  if (tab->type != JT_REF)
  {
    *cause= "not ref access";
    return false;
  }

  if (join_cache_level)
  {
    *cause= "join buffer is selected";
    return false;
  }

  if (tab->bush_children || tab->is_inner_table_of_outer_join() ||
      tab->is_inner_table_of_semijoin())
  {
    *cause= tab->bush_children ? "bush join tab is not supported" :
            tab->is_inner_table_of_outer_join() ?
            "outer join inner table is not supported" :
            "semi join inner table is not supported";
    return false;
  }

  if (tab->ref.disable_cache || tab->ref.is_access_triggered() ||
      tab->ref.key_length == 0)
  {
    *cause= tab->ref.disable_cache ? "ref cache disabled" :
            tab->ref.is_access_triggered() ? "triggered ref access" :
            "empty ref key";
    return false;
  }

  if (tab->table->s->blob_fields)
  {
    *cause= "BLOB row is not supported";
    return false;
  }

  if ((int) tab->table->reginfo.lock_type > (int) TL_READ_HIGH_PRIORITY)
  {
    *cause= "locking read is not supported";
    return false;
  }

  tab->partial_result_cache_estimated_hit_ratio=
    partial_result_cache_estimate_hit_ratio(tab,
      &tab->partial_result_cache_estimated_saved_cost);

  if (prc_join_hint != hint_state::ENABLED &&
      tab->partial_result_cache_estimated_hit_ratio <
      thd->variables.partial_result_cache_cost_threshold)
  {
    *cause= "cost below threshold";
    return false;
  }

  *cause= prc_join_hint == hint_state::ENABLED ?
          "chosen by PRC_JOIN hint" : "chosen by cost";
  return true;
}


class Partial_result_cache
{
  static const size_t DEFAULT_BUCKET_COUNT= 1024;

  struct Cached_row
  {
    Cached_row *next;
    uchar data[1];
  };

  struct Entry
  {
    Entry *next;
    bool found;
    uint key_length;
    uchar *key;
    Cached_row *rows;
    Cached_row *last_row;
  };

  Entry **m_buckets;
  size_t m_bucket_count;
  size_t m_bucket_bytes;
  Entry *m_current_entry;
  Cached_row *m_current_row;
  size_t m_mem_used;
  ulong m_rec_length;
  ulong m_hit;
  ulong m_miss;
  ulong m_rows_cached;
  ulong m_rows_replayed;
  ulong m_bypass;
  Partial_result_cache_stats *m_stats;
  bool m_replaying;
  bool m_disabled;

  void update_mem_stats()
  {
    if (m_stats)
      m_stats->mem_used= m_mem_used;
  }

  void count_hit()
  {
    m_hit++;
    if (m_stats)
      m_stats->hit++;
  }

  void count_miss()
  {
    m_miss++;
    if (m_stats)
      m_stats->miss++;
  }

  void count_row_cached()
  {
    m_rows_cached++;
    if (m_stats)
      m_stats->rows_cached++;
  }

  void count_row_replayed()
  {
    m_rows_replayed++;
    if (m_stats)
      m_stats->rows_replayed++;
  }

  void count_bypass()
  {
    m_bypass++;
    if (m_stats)
      m_stats->bypass++;
  }

  // Returns a stable hash for the materialized ref lookup tuple.
  size_t hash_key(const uchar *key, uint key_length) const
  {
    size_t hash= 1469598103934665603ULL;
    for (uint i= 0; i < key_length; i++)
    {
      hash^= key[i];
      hash*= 1099511628211ULL;
    }
    return hash;
  }

  // Frees all cached entries while keeping the bucket array reusable.
  void clear_entries()
  {
    if (!m_buckets)
      return;

    for (size_t i= 0; i < m_bucket_count; i++)
    {
      Entry *entry= m_buckets[i];
      while (entry)
      {
        Entry *next_entry= entry->next;
        Cached_row *row= entry->rows;
        while (row)
        {
          Cached_row *next_row= row->next;
          my_free(row);
          row= next_row;
        }
        my_free(entry->key);
        my_free(entry);
        entry= next_entry;
      }
      m_buckets[i]= NULL;
    }
    m_mem_used= m_bucket_bytes;
    update_mem_stats();
  }

  // Disables this PTRC instance and releases cached entries.
  void disable()
  {
    clear_entries();
    m_current_entry= NULL;
    m_current_row= NULL;
    m_replaying= false;
    if (!m_disabled)
      count_bypass();
    m_disabled= true;
  }

  // Allocates bytes against the per-session memory limit.
  //
  // If the limit is exceeded, all cached rows are discarded and this PTRC
  // instance is disabled for the rest of the execution. This keeps executor
  // semantics unchanged while avoiding repeated allocation failures. Runtime
  // allocation failures follow the same disable-and-continue path.
  void *alloc_bytes(size_t bytes, THD *thd)
  {
    size_t max_mem=
      static_cast<size_t>(thd->variables.partial_result_cache_max_mem_size);
    if (!max_mem || bytes > max_mem || m_mem_used > max_mem - bytes)
    {
      disable();
      return NULL;
    }

    void *ptr= my_malloc(PSI_INSTRUMENT_ME, bytes, MYF(MY_ZEROFILL));
    if (!ptr)
    {
      disable();
      return NULL;
    }

    m_mem_used+= bytes;
    update_mem_stats();
    return ptr;
  }

  // Finds an entry for a materialized ref key.
  Entry *find_entry(const uchar *key, uint key_length) const
  {
    if (!m_buckets)
      return NULL;

    Entry *entry= m_buckets[hash_key(key, key_length) % m_bucket_count];
    while (entry)
    {
      if (entry->key_length == key_length &&
          !memcmp(entry->key, key, key_length))
        return entry;
      entry= entry->next;
    }
    return NULL;
  }

  // Returns true when a low runtime hit ratio should stop cache maintenance.
  bool should_bypass_for_hit_ratio(THD *thd) const
  {
    uint frequency= thd->variables.partial_result_cache_hit_ratio_frequency;
    ulong probes= m_hit + m_miss;

    if (!frequency || !m_miss || (m_miss % frequency))
      return false;

    double hit_ratio= probes ? static_cast<double>(m_hit) / probes : 0.0;
    return hit_ratio < thd->variables.partial_result_cache_min_hit_ratio;
  }

  // Adds a row buffer to an existing positive cache entry.
  bool append_row(Entry *entry, TABLE *table, THD *thd)
  {
    Cached_row *row=
      static_cast<Cached_row*>(alloc_bytes(sizeof(Cached_row) + m_rec_length,
                                           thd));
    if (!row)
      return false;

    memcpy(row->data, table->record[0], m_rec_length);
    row->next= NULL;
    if (entry->last_row)
      entry->last_row->next= row;
    else
      entry->rows= row;
    entry->last_row= row;
    count_row_cached();
    return true;
  }

  // Restores a cached row into TABLE::record[0].
  //
  // Eligibility rejects BLOB rows and locking reads. For supported rows, the
  // fixed TABLE record buffer fully represents the values needed by the
  // remaining executor pipeline.
  void load_row(TABLE *table, const Cached_row *row)
  {
    memcpy(table->record[0], row->data, m_rec_length);
    table->status= 0;
    table->null_row= 0;
    count_row_replayed();
  }

  // Replays the next cached row for the current ref key.
  int replay_next(TABLE *table)
  {
    if (!m_current_entry || !m_current_entry->found ||
        !m_current_row)
    {
      table->status= STATUS_GARBAGE;
      return -1;
    }

    Cached_row *row= m_current_row;
    m_current_row= row->next;
    load_row(table, row);
    return 0;
  }

  // Caches a key that found no inner rows.
  void remember_negative(const uchar *key, uint key_length, THD *thd)
  {
    Entry *entry= static_cast<Entry*>(alloc_bytes(sizeof(Entry), thd));
    if (!entry)
      return;

    if (!(entry->key= static_cast<uchar*>(alloc_bytes(key_length, thd))))
    {
      my_free(entry);
      return;
    }

    memcpy(entry->key, key, key_length);
    entry->key_length= key_length;
    entry->found= false;
    size_t bucket= hash_key(key, key_length) % m_bucket_count;
    entry->next= m_buckets[bucket];
    m_buckets[bucket]= entry;
  }

  // Caches the first row returned by the storage engine for a ref key.
  void remember_first_row(const uchar *key, uint key_length, JOIN_TAB *tab)
  {
    THD *thd= tab->join->thd;
    Entry *entry= static_cast<Entry*>(alloc_bytes(sizeof(Entry), thd));
    if (!entry)
      return;

    if (!(entry->key= static_cast<uchar*>(alloc_bytes(key_length, thd))))
    {
      my_free(entry);
      return;
    }

    memcpy(entry->key, key, key_length);
    entry->key_length= key_length;
    entry->found= true;
    if (!append_row(entry, tab->table, thd))
    {
      my_free(entry->key);
      my_free(entry);
      return;
    }

    size_t bucket= hash_key(key, key_length) % m_bucket_count;
    entry->next= m_buckets[bucket];
    m_buckets[bucket]= entry;
    m_current_entry= entry;
    m_current_row= entry->rows ? entry->rows->next : NULL;
  }

  // Caches an additional row from join_read_next_same().
  void remember_next_row(JOIN_TAB *tab)
  {
    if (!m_current_entry || m_disabled)
      return;

    append_row(m_current_entry, tab->table, tab->join->thd);
  }

public:
  explicit Partial_result_cache(TABLE *table)
   : m_buckets(NULL),
     m_bucket_count(DEFAULT_BUCKET_COUNT),
     m_bucket_bytes(0),
     m_current_entry(NULL),
     m_current_row(NULL),
     m_mem_used(0),
     m_rec_length(table->s->reclength),
     m_hit(0),
     m_miss(0),
     m_rows_cached(0),
     m_rows_replayed(0),
     m_bypass(0),
     m_stats(NULL),
     m_replaying(false),
     m_disabled(false)
  {}

  void set_stats(Partial_result_cache_stats *stats)
  {
    m_stats= stats;
    if (m_stats)
    {
      m_stats->hit= m_hit;
      m_stats->miss= m_miss;
      m_stats->rows_cached= m_rows_cached;
      m_stats->rows_replayed= m_rows_replayed;
      m_stats->bypass= m_bypass;
      m_stats->mem_used= m_mem_used;
    }
  }

  ~Partial_result_cache()
  {
    clear_entries();
    my_free(m_buckets);
    statistic_add(partial_result_cache_hit, m_hit, &LOCK_status);
    statistic_add(partial_result_cache_miss, m_miss, &LOCK_status);
    statistic_add(partial_result_cache_rows_cached, m_rows_cached,
                  &LOCK_status);
    statistic_add(partial_result_cache_rows_replayed, m_rows_replayed,
                  &LOCK_status);
    statistic_add(partial_result_cache_bypass, m_bypass, &LOCK_status);
  }

  // Allocates the bucket array after construction.
  bool init(THD *thd)
  {
    if (m_buckets)
      return false;
    m_bucket_bytes= sizeof(Entry*) * m_bucket_count;
    if (!(m_buckets= static_cast<Entry**>(alloc_bytes(m_bucket_bytes, thd))))
      return true;
    return false;
  }

  // Reads the first matching row for the current ref key.
  //
  // A cache hit replays a cached positive or negative result. A miss performs
  // the normal index lookup and records the returned result for later probes.
  int read_first(JOIN_TAB *tab)
  {
    int error;
    TABLE *table= tab->table;

    m_current_entry= NULL;
    m_current_row= NULL;
    m_replaying= false;

    if (m_disabled)
      return join_read_always_key(tab);

    if (!table->file->inited)
    {
      if (unlikely((error= table->file->ha_index_init(tab->ref.key,
                                                      tab->sorted))))
      {
        (void) report_error(table, error);
        return 1;
      }
    }

    if (unlikely(cp_buffer_from_ref(tab->join->thd, table, &tab->ref)))
      return -1;

    if (unlikely((error=
                  table->file->prepare_index_key_scan_map(
                    tab->ref.key_buff,
                    make_prev_keypart_map(tab->ref.key_parts)))))
    {
      report_error(table, error);
      return -1;
    }

    const uchar *key= tab->ref.key_buff;
    uint key_length= tab->ref.key_length;
    Entry *entry= find_entry(key, key_length);
    if (entry)
    {
      count_hit();
      m_current_entry= entry;
      m_current_row= entry->rows;
      m_replaying= true;
      return replay_next(table);
    }

    count_miss();
    if (should_bypass_for_hit_ratio(tab->join->thd))
    {
      disable();
      return join_read_always_key(tab);
    }

    if ((error= table->file->ha_index_read_map(
           table->record[0], tab->ref.key_buff,
           make_prev_keypart_map(tab->ref.key_parts), HA_READ_KEY_EXACT)))
    {
      if (error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE)
        return report_error(table, error);
      remember_negative(key, key_length, tab->join->thd);
      return -1;
    }

    remember_first_row(key, key_length, tab);
    return 0;
  }

  // Reads the next row for the current ref key.
  int read_next(READ_RECORD *info)
  {
    TABLE *table= info->table;
    JOIN_TAB *tab= table->reginfo.join_tab;

    if (m_replaying)
      return replay_next(table);

    int error= join_read_next_same(info);
    if (!error)
      remember_next_row(tab);
    return error;
  }
};


bool
setup_partial_result_cache(JOIN_TAB *tab, JOIN_TAB *first_tab,
                           uint join_cache_level)
{
  tab->partial_result_cache_estimated_hit_ratio= 0.0;
  tab->partial_result_cache_estimated_saved_cost= 0.0;
  tab->partial_result_cache_cause= NULL;
  tab->partial_result_cache_eligible=
    partial_result_cache_is_eligible(tab, first_tab, join_cache_level,
                                     &tab->partial_result_cache_cause);
  trace_partial_result_cache_choice(tab, tab->partial_result_cache_eligible,
                                    tab->partial_result_cache_cause);
  if (tab->partial_result_cache_eligible)
  {
    if (!tab->partial_result_cache &&
        !(tab->partial_result_cache=
            new (std::nothrow) Partial_result_cache(tab->table)))
      return true;
    if (tab->partial_result_cache->init(tab->join->thd))
      return true;

    tab->read_first_record= join_read_partial_result_cache_key;
    tab->read_record.read_record_func=
      join_read_partial_result_cache_next_same;
    return false;
  }

  free_partial_result_cache(tab->partial_result_cache);
  tab->partial_result_cache= NULL;
  return false;
}


void
free_partial_result_cache(Partial_result_cache *cache)
{
  delete cache;
}


void
set_partial_result_cache_stats(Partial_result_cache *cache,
                               Partial_result_cache_stats *stats)
{
  if (cache)
    cache->set_stats(stats);
}


static int
join_read_partial_result_cache_key(JOIN_TAB *tab)
{
  DBUG_ASSERT(tab->partial_result_cache != NULL);
  return tab->partial_result_cache->read_first(tab);
}


static int
join_read_partial_result_cache_next_same(READ_RECORD *info)
{
  JOIN_TAB *tab= info->table->reginfo.join_tab;
  DBUG_ASSERT(tab->partial_result_cache != NULL);
  return tab->partial_result_cache->read_next(info);
}

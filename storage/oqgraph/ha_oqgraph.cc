/* Copyright (C) 2007-2009 Arjen G Lentz & Antony T Curtis for Open Query
   Portions of this file copyright (C) 2000-2006 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/* ======================================================================
   Open Query Graph Computation Engine, based on a concept by Arjen Lentz
   Mk.II implementation by Antony Curtis & Arjen Lentz
   For more information, documentation, support, enhancement engineering,
   and non-GPL licensing, see http://openquery.com/graph
   or contact graph@openquery.com
   For packaged binaries, see http://ourdelta.org
   ======================================================================
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include <stdarg.h>
#include <stdio.h>

#define MYSQL_SERVER	// to have THD
#include "mysql_priv.h"
#include <mysql/plugin.h>

#include "ha_oqgraph.h"
#include "graphcore.h"

#define OQGRAPH_STATS_UPDATE_THRESHOLD 10

using namespace open_query;


struct oqgraph_table_option_struct
{
  char *table_name;

  char *origid; // name of the origin id column
  char *destid; // name of the target id column
  char *weight; // name of the weight column (optional)
};

#define ha_table_option_struct oqgraph_table_option_struct
ha_create_table_option oqgraph_table_option_list[]=
{
  HA_TOPTION_STRING("data_table", table_name),
  HA_TOPTION_STRING("origid", origid),
  HA_TOPTION_STRING("destid", destid),
  HA_TOPTION_STRING("weight", weight),
  HA_TOPTION_END
};

static const char oqgraph_description[]=
  "Open Query Graph Computation Engine "
  "(http://openquery.com/graph)";

#define STATISTIC_INCREMENT(X) ha_statistic_increment(&SSV::X)
#define MOVE(X) move_field_offset(X)
#define RECORDS stats.records

static bool oqgraph_init_done= 0;


static handler* oqgraph_create_handler(handlerton *hton, TABLE_SHARE *table,
                                       MEM_ROOT *mem_root)
{
  return new (mem_root) ha_oqgraph(hton, table);
}

static int oqgraph_init(handlerton *hton)
{
  hton->state= SHOW_OPTION_YES;
  hton->db_type= DB_TYPE_AUTOASSIGN;
  hton->create= oqgraph_create_handler;
  hton->flags= HTON_NO_FLAGS;
  hton->table_options= oqgraph_table_option_list;
  oqgraph_init_done= TRUE;
  return 0;
}

static int oqgraph_fini(void *)
{
  oqgraph_init_done= FALSE;
  return 0;
}

static int error_code(int res)
{
  switch (res)
  {
  case oqgraph::OK:
    return 0;
  case oqgraph::NO_MORE_DATA:
    return HA_ERR_END_OF_FILE;
  case oqgraph::EDGE_NOT_FOUND:
    return HA_ERR_KEY_NOT_FOUND;
  case oqgraph::INVALID_WEIGHT:
    return HA_ERR_AUTOINC_ERANGE;
  case oqgraph::DUPLICATE_EDGE:
    return HA_ERR_FOUND_DUPP_KEY;
  case oqgraph::CANNOT_ADD_VERTEX:
  case oqgraph::CANNOT_ADD_EDGE:
    return HA_ERR_RECORD_FILE_FULL;
  case oqgraph::MISC_FAIL:
  default:
    return HA_ERR_CRASHED_ON_USAGE;
  }
}

/**
 * Check if table complies with our designated structure
 *
 *    ColName    Type      Attributes
 *    =======    ========  =============
 *    latch     SMALLINT  UNSIGNED NULL
 *    origid    BIGINT    UNSIGNED NULL
 *    destid    BIGINT    UNSIGNED NULL
 *    weight    DOUBLE    NULL
 *    seq       BIGINT    UNSIGNED NULL
 *    linkid    BIGINT    UNSIGNED NULL
 *    =================================
 *
  CREATE TABLE foo (
    latch   SMALLINT  UNSIGNED NULL,
    origid  BIGINT    UNSIGNED NULL,
    destid  BIGINT    UNSIGNED NULL,
    weight  DOUBLE    NULL,
    seq     BIGINT    UNSIGNED NULL,
    linkid  BIGINT    UNSIGNED NULL,
    KEY (latch, origid, destid) USING HASH,
    KEY (latch, destid, origid) USING HASH
  ) ENGINE=OQGRAPH
    READ_TABLE=bar
    ORIGID=src_id
    DESTID=tgt_id

 */
static int oqgraph_check_table_structure (TABLE *table_arg)
{
  int i;
  struct { const char *colname; int coltype; } skel[] = {
    { "latch" , MYSQL_TYPE_SHORT },
    { "origid", MYSQL_TYPE_LONGLONG },
    { "destid", MYSQL_TYPE_LONGLONG },
    { "weight", MYSQL_TYPE_DOUBLE },
    { "seq"   , MYSQL_TYPE_LONGLONG },
    { "linkid", MYSQL_TYPE_LONGLONG },
  { NULL    , 0}
  };

  DBUG_ENTER("ha_oqgraph::table_structure_ok");

  Field **field= table_arg->field;
  for (i= 0; *field && skel[i].colname; i++, field++) {
    /* Check Column Type */
    if ((*field)->type() != skel[i].coltype)
      DBUG_RETURN(-1);
    if (skel[i].coltype != MYSQL_TYPE_DOUBLE) {
      /* Check Is UNSIGNED */
      if (!((*field)->flags & UNSIGNED_FLAG ))
        DBUG_RETURN(-1);
    }
    /* Check THAT  NOT NULL isn't set */
    if ((*field)->flags & NOT_NULL_FLAG)
      DBUG_RETURN(-1);
    /* Check the column name */
    if (strcmp(skel[i].colname,(*field)->field_name))
      DBUG_RETURN(-1);
  }

  if (skel[i].colname || *field || !table_arg->key_info || !table_arg->s->keys)
    DBUG_RETURN(-1);

  KEY *key= table_arg->key_info;
  for (uint i= 0; i < table_arg->s->keys; ++i, ++key)
  {
    Field **field= table_arg->field;
    /* check that the first key part is the latch and it is a hash key */
    if (!(field[0] == key->key_part[0].field &&
          HA_KEY_ALG_HASH == key->algorithm))
      DBUG_RETURN(-1);
    if (key->key_parts == 3)
    {
      /* KEY (latch, origid, destid) USING HASH */
      /* KEY (latch, destid, origid) USING HASH */
      if (!(field[1] == key->key_part[1].field &&
            field[2] == key->key_part[2].field) &&
          !(field[1] == key->key_part[2].field &&
            field[2] == key->key_part[1].field))
        DBUG_RETURN(-1);
    }
    else
      DBUG_RETURN(-1);
  }

  DBUG_RETURN(0);
}

/*****************************************************************************
** OQGRAPH tables
*****************************************************************************/

ha_oqgraph::ha_oqgraph(handlerton *hton, TABLE_SHARE *table_arg)
  : handler(hton, table_arg)
  , graph_share(0)
  , graph(0)
  , error_message("", 0, &my_charset_latin1)
{ }

ha_oqgraph::~ha_oqgraph()
{ }

static const char *ha_oqgraph_exts[] =
{
  NullS
};

const char **ha_oqgraph::bas_ext() const
{
  return ha_oqgraph_exts;
}

ulonglong ha_oqgraph::table_flags() const
{
  return (HA_NO_BLOBS | HA_NULL_IN_KEY |
          HA_REC_NOT_IN_SEQ | HA_CAN_INSERT_DELAYED |
          HA_BINLOG_STMT_CAPABLE | HA_BINLOG_ROW_CAPABLE);
}

ulong ha_oqgraph::index_flags(uint inx, uint part, bool all_parts) const
{
  return HA_ONLY_WHOLE_INDEX | HA_KEY_SCAN_NOT_ROR;
}

bool ha_oqgraph::get_error_message(int error, String* buf)
{
  if (error < 0)
  {
    buf->append(error_message);
    buf->c_ptr_safe();
    error_message.length(0);
  }
  return false;
}

void ha_oqgraph::print_error(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  error_message.reserve(256);
  size_t len = error_message.length();
  len += vsnprintf(&error_message[len], 255, fmt, ap);
  error_message.length(len);
  va_end(ap);
}


int ha_oqgraph::open(const char *name, int mode, uint test_if_locked)
{
  THD* thd = current_thd;
  oqgraph_table_option_struct *options=
    reinterpret_cast<oqgraph_table_option_struct*>(table->s->option_struct);

  error_message.length(0);

  const char* p= strend(name)-1;
  while (p > name && *p != '\\' && *p != '/')
    --p;

  init_tmp_table_share(
      thd, share, table->s->db.str, table->s->db.length,
      options->table_name, "");

  size_t tlen= strlen(options->table_name);
  size_t plen= (int)(p - name) + tlen;

  share->path.str= (char*)
      alloc_root(&share->mem_root, plen + 1);

  strmov(strnmov(share->path.str, name, (int)(p - name) + 1), options->table_name);

  share->normalized_path.str= share->path.str;
  share->path.length= share->normalized_path.length= plen;

  origid= destid= weight= 0;

  while (open_table_def(thd, share, 0))
  {
    if (thd->is_error() && thd->main_da.sql_errno() != ER_NO_SUCH_TABLE)
    {
      free_table_share(share);
      return thd->main_da.sql_errno();
    }

    if (ha_create_table_from_engine(thd, table->s->db.str, options->table_name))
    {
      free_table_share(share);
      return thd->main_da.sql_errno();
    }
    mysql_reset_errors(thd, 1);
    thd->clear_error();
    continue;
  }

  if (int err= share->error)
  {
    open_table_error(share, share->error, share->open_errno, share->errarg);
    free_table_share(share);
    return err;
  }

  if (share->is_view)
  {
    open_table_error(share, 1, EMFILE, 0);
    free_table_share(share);
    print_error("VIEWs are not supported for a backing store");
    return -1;
  }

  if (int err= open_table_from_share(thd, share, "",
                            (uint) (HA_OPEN_KEYFILE | HA_OPEN_RNDFILE |
                                    HA_GET_INDEX | HA_TRY_READ_ONLY),
                            READ_KEYINFO | COMPUTE_TYPES | EXTRA_RECORD,
                            thd->open_options, edges, FALSE))
  {
    open_table_error(share, err, EMFILE, 0);
    free_table_share(share);
    return -1;
  }

  edges->reginfo.lock_type= TL_READ;

  edges->tablenr= thd->current_tablenr++;
  edges->status= STATUS_NO_RECORD;
  edges->file->ha_start_of_new_statement();
  edges->file->ft_handler= 0;
  edges->pos_in_table_list= 0;
  edges->clear_column_bitmaps();
  bfill(table->record[0], table->s->null_bytes, 255);
  bfill(table->record[1], table->s->null_bytes, 255);

  // We expect fields origid, destid and optionally weight
  origid= destid= weight= 0;

  if (!edges->file)
  {
    print_error("Some error occurred opening table '%s'", options->table_name);
    free_table_share(share);
    return -1;
  }

  for (Field **field= edges->field; *field; ++field)
  {
    if (strcmp(options->origid, (*field)->field_name))
      continue;
    if ((*field)->cmp_type() != INT_RESULT ||
        !((*field)->flags & NOT_NULL_FLAG))
    {
      print_error("Column '%s.%s' is not a not-null integer type",
          options->table_name, options->origid);
      closefrm(edges, 0);
      free_table_share(share);
      return -1;
    }
    origid = *field;
    break;
  }

  for (Field **field= edges->field; *field; ++field)
  {
    if (strcmp(options->destid, (*field)->field_name))
      continue;
    if ((*field)->type() != origid->type() ||
        !((*field)->flags & NOT_NULL_FLAG))
    {
      print_error("Column '%s.%s' is not a not-null integer type",
          options->table_name, options->destid);
      closefrm(edges, 0);
      free_table_share(share);
      return -1;
    }
    destid = *field;
    break;
  }

  for (Field **field= edges->field; options->weight && *field; ++field)
  {
    if (strcmp(options->weight, (*field)->field_name))
      continue;
    if ((*field)->result_type() != REAL_RESULT ||
        !((*field)->flags & NOT_NULL_FLAG))
    {
      print_error("Column '%s.%s' is not a not-null real type",
          options->table_name, options->weight);
      closefrm(edges, 0);
      free_table_share(share);
      return -1;
    }
    weight = *field;
    break;
  }

  if (!origid || !destid || (!weight && options->weight))
  {
    print_error("Data columns missing on table '%s'", options->table_name);
    closefrm(edges, 0);
    free_table_share(share);
    return -1;
  }

  if (!(graph_share = oqgraph::create(edges, origid, destid, weight)))
  {
    print_error("Unable to create graph instance.");
    closefrm(edges, 0);
    free_table_share(share);
    return -1;
  }
  ref_length= oqgraph::sizeof_ref;

  graph = oqgraph::create(graph_share);

  return 0;
}

int ha_oqgraph::close(void)
{
  oqgraph::free(graph); graph= 0;
  oqgraph::free(graph_share); graph_share= 0;

  if (share)
  {
    if (edges->file)
      closefrm(edges, 0);
    free_table_share(share);
  }
  return 0;
}

void ha_oqgraph::update_key_stats()
{
  for (uint i= 0; i < table->s->keys; i++)
  {
    KEY *key=table->key_info+i;
    if (!key->rec_per_key)
      continue;
    if (key->algorithm != HA_KEY_ALG_BTREE)
    {
      if (key->flags & HA_NOSAME)
        key->rec_per_key[key->key_parts-1]= 1;
      else
      {
        //unsigned vertices= graph->vertices_count();
        //unsigned edges= graph->edges_count();
        //uint no_records= vertices ? 2 * (edges + vertices) / vertices : 2;
        //if (no_records < 2)
        uint
          no_records= 2;
        key->rec_per_key[key->key_parts-1]= no_records;
      }
    }
  }
  /* At the end of update_key_stats() we can proudly claim they are OK. */
  //skey_stat_version= share->key_stat_version;
}


int ha_oqgraph::write_row(byte * buf)
{
  return ER_OPEN_AS_READONLY;
}

int ha_oqgraph::update_row(const byte * old, byte * buf)
{
  return ER_OPEN_AS_READONLY;
}

int ha_oqgraph::delete_row(const byte * buf)
{
  return ER_OPEN_AS_READONLY;
}

int ha_oqgraph::index_read(byte * buf, const byte * key, uint key_len,
			enum ha_rkey_function find_flag)
{
  DBUG_ASSERT(inited==INDEX);
  return index_read_idx(buf, active_index, key, key_len, find_flag);
}

int ha_oqgraph::index_next_same(byte *buf, const byte *key, uint key_len)
{
  int res;
  open_query::row row;
  DBUG_ASSERT(inited==INDEX);
  STATISTIC_INCREMENT(ha_read_key_count);
  if (!(res= graph->fetch_row(row)))
    res= fill_record(buf, row);
  table->status= res ? STATUS_NOT_FOUND : 0;
  return error_code(res);
}

int ha_oqgraph::index_read_idx(byte * buf, uint index, const byte * key,
			    uint key_len, enum ha_rkey_function find_flag)
{
  Field **field= table->field;
  KEY *key_info= table->key_info + index;
  int res;
  VertexID orig_id, dest_id;
  int latch;
  VertexID *orig_idp=0, *dest_idp=0;
  int *latchp=0;
  open_query::row row;
  STATISTIC_INCREMENT(ha_read_key_count);

  bmove_align(buf, table->s->default_values, table->s->reclength);
  key_restore(buf, (byte*) key, key_info, key_len);

  my_bitmap_map *old_map= dbug_tmp_use_all_columns(table, table->read_set);
  my_ptrdiff_t ptrdiff= buf - table->record[0];

  if (ptrdiff)
  {
    field[0]->MOVE(ptrdiff);
    field[1]->MOVE(ptrdiff);
    field[2]->MOVE(ptrdiff);
  }

  if (!field[0]->is_null())
  {
    latch= (int) field[0]->val_int();
    latchp= &latch;
  }

  if (!field[1]->is_null())
  {
    orig_id= (VertexID) field[1]->val_int();
    orig_idp= &orig_id;
  }

  if (!field[2]->is_null())
  {
    dest_id= (VertexID) field[2]->val_int();
    dest_idp= &dest_id;
  }

  if (ptrdiff)
  {
    field[0]->MOVE(-ptrdiff);
    field[1]->MOVE(-ptrdiff);
    field[2]->MOVE(-ptrdiff);
  }
  dbug_tmp_restore_column_map(table->read_set, old_map);

  res= graph->search(latchp, orig_idp, dest_idp);

  if (!res && !(res= graph->fetch_row(row)))
    res= fill_record(buf, row);
  table->status = res ? STATUS_NOT_FOUND : 0;
  return error_code(res);
}

int ha_oqgraph::fill_record(byte *record, const open_query::row &row)
{
  Field **field= table->field;

  bmove_align(record, table->s->default_values, table->s->reclength);

  my_bitmap_map *old_map= dbug_tmp_use_all_columns(table, table->write_set);
  my_ptrdiff_t ptrdiff= record - table->record[0];

  if (ptrdiff)
  {
    field[0]->MOVE(ptrdiff);
    field[1]->MOVE(ptrdiff);
    field[2]->MOVE(ptrdiff);
    field[3]->MOVE(ptrdiff);
    field[4]->MOVE(ptrdiff);
    field[5]->MOVE(ptrdiff);
  }

  // just each field specifically, no sense iterating
  if (row.latch_indicator)
  {
    field[0]->set_notnull();
    field[0]->store((longlong) row.latch, 0);
  }

  if (row.orig_indicator)
  {
    field[1]->set_notnull();
    field[1]->store((longlong) row.orig, 0);
  }

  if (row.dest_indicator)
  {
    field[2]->set_notnull();
    field[2]->store((longlong) row.dest, 0);
  }

  if (row.weight_indicator)
  {
    field[3]->set_notnull();
    field[3]->store((double) row.weight);
  }

  if (row.seq_indicator)
  {
    field[4]->set_notnull();
    field[4]->store((longlong) row.seq, 0);
  }

  if (row.link_indicator)
  {
    field[5]->set_notnull();
    field[5]->store((longlong) row.link, 0);
  }

  if (ptrdiff)
  {
    field[0]->MOVE(-ptrdiff);
    field[1]->MOVE(-ptrdiff);
    field[2]->MOVE(-ptrdiff);
    field[3]->MOVE(-ptrdiff);
    field[4]->MOVE(-ptrdiff);
    field[5]->MOVE(-ptrdiff);
  }
  dbug_tmp_restore_column_map(table->write_set, old_map);

  return 0;
}

int ha_oqgraph::rnd_init(bool scan)
{
  edges->prepare_for_position();
  return error_code(graph->random(scan));
}

int ha_oqgraph::rnd_next(byte *buf)
{
  int res;
  open_query::row row;
  STATISTIC_INCREMENT(ha_read_rnd_next_count);
  if (!(res= graph->fetch_row(row)))
    res= fill_record(buf, row);
  table->status= res ? STATUS_NOT_FOUND: 0;
  return error_code(res);
}

int ha_oqgraph::rnd_pos(byte * buf, byte *pos)
{
  int res;
  open_query::row row;
  STATISTIC_INCREMENT(ha_read_rnd_count);
  if (!(res= graph->fetch_row(row, pos)))
    res= fill_record(buf, row);
  table->status=res ? STATUS_NOT_FOUND: 0;
  return error_code(res);
}

void ha_oqgraph::position(const byte *record)
{
  graph->row_ref((void*) ref);	// Ref is aligned
}

int ha_oqgraph::cmp_ref(const byte *ref1, const byte *ref2)
{
  return memcmp(ref1, ref2, oqgraph::sizeof_ref);
}

int ha_oqgraph::info(uint flag)
{
  RECORDS= graph->edges_count();

  /*
    If info() is called for the first time after open(), we will still
    have to update the key statistics. Hoping that a table lock is now
    in place.
  */
//  if (key_stat_version != share->key_stat_version)
  //  update_key_stats();
  return 0;
}

int ha_oqgraph::extra(enum ha_extra_function operation)
{
  return edges->file->extra(operation);
}

int ha_oqgraph::delete_all_rows()
{
  return ER_OPEN_AS_READONLY;
}

int ha_oqgraph::external_lock(THD *thd, int lock_type)
{
  return edges->file->ha_external_lock(thd, lock_type);
}


THR_LOCK_DATA **ha_oqgraph::store_lock(THD *thd,
				       THR_LOCK_DATA **to,
				       enum thr_lock_type lock_type)
{
  return edges->file->store_lock(thd, to, lock_type);
}

/*
  We have to ignore ENOENT entries as the HEAP table is created on open and
  not when doing a CREATE on the table.
*/

int ha_oqgraph::delete_table(const char *)
{
  return 0;
}

int ha_oqgraph::rename_table(const char *, const char *)
{
  return 0;
}


ha_rows ha_oqgraph::records_in_range(uint inx, key_range *min_key,
                                  key_range *max_key)
{
  KEY *key=table->key_info+inx;
  //if (key->algorithm == HA_KEY_ALG_BTREE)
  //  return btree_records_in_range(file, inx, min_key, max_key);

  if (!min_key || !max_key ||
      min_key->length != max_key->length ||
      min_key->length < key->key_length - key->key_part[2].store_length ||
      min_key->flag != HA_READ_KEY_EXACT ||
      max_key->flag != HA_READ_AFTER_KEY)
  {
    if (min_key->length == key->key_part[0].store_length)
    {
      // If latch is not null and equals 0, return # nodes
      DBUG_ASSERT(key->key_part[0].store_length == 3);
      if (key->key_part[0].null_bit && !min_key->key[0] &&
          !min_key->key[1] && !min_key->key[2])
        return graph->vertices_count();
    }
    return HA_POS_ERROR;			// Can only use exact keys
  }

  if (RECORDS <= 1)
    return RECORDS;

  /* Assert that info() did run. We need current statistics here. */
  //DBUG_ASSERT(key_stat_version == share->key_stat_version);
  //ha_rows result= key->rec_per_key[key->key_parts-1];
  ha_rows result= 10;

  return result;
}


int ha_oqgraph::create(const char *name, TABLE *table_arg,
		    HA_CREATE_INFO *create_info)
{
  oqgraph_table_option_struct *options=
    reinterpret_cast<oqgraph_table_option_struct*>(table->s->option_struct);

  if (int res = oqgraph_check_table_structure(table_arg))
    return error_code(res);

  (void)(options);
  return 0;
}


void ha_oqgraph::update_create_info(HA_CREATE_INFO *create_info)
{
  table->file->info(HA_STATUS_AUTO);
  //if (!(create_info->used_fields & HA_CREATE_USED_AUTO))
  //  create_info->auto_increment_value= auto_increment_value;
}

struct st_mysql_storage_engine oqgraph_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

mysql_declare_plugin(oqgraph)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &oqgraph_storage_engine,
  "OQGRAPH",
  "Arjen Lentz & Antony T Curtis, Open Query",
  oqgraph_description,
  PLUGIN_LICENSE_GPL,
  (int (*)(void*)) oqgraph_init, /* Plugin Init                  */
  oqgraph_fini,               /* Plugin Deinit                   */
  0x0300,                     /* Version: 3s.0                    */
  NULL,                       /* status variables                */
  NULL,                       /* system variables                */
  NULL                        /* config options                  */
}
mysql_declare_plugin_end;

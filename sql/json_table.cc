/*
  Copyright (c) 2020, MariaDB Corporation

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h" /* TMP_TABLE_PARAM */
#include "table.h"
#include "item_jsonfunc.h"
#include "json_table.h"
#include "sql_show.h"


class table_function_handlerton
{
public:
  handlerton m_hton;
  table_function_handlerton()
  {
    bzero(&m_hton, sizeof(m_hton));
    m_hton.tablefile_extensions= hton_no_exts;
    m_hton.slot= HA_SLOT_UNDEF;
  }
};


static table_function_handlerton table_function_hton;


/*
   A table that produces output rows for JSON_TABLE().
*/

class ha_json_table: public handler
{
  Table_function_json_table *m_jt;

  String *m_js; // The JSON document we're reading
  String m_tmps; // Buffer for the above

  int fill_column_values(uchar * buf, uchar *pos);

public:
  ha_json_table(TABLE_SHARE *share_arg, Table_function_json_table *jt):
    handler(&table_function_hton.m_hton, share_arg), m_jt(jt)
  {
    /*
      set the mark_trx_read_write_done to avoid the
      handler::mark_trx_read_write_internal() call.
      It relies on &ha_thd()->ha_data[ht->slot].ha_info[0] to be set.
      But we don't set the ha_data for the ha_json_table, and
      that call makes no sence for ha_json_table.
   */
    mark_trx_read_write_done= 1;

    /* See ha_json_table::position for format definition */
    ref_length= m_jt->m_columns.elements * 4;
  }
  ~ha_json_table() {}
  handler *clone(const char *name, MEM_ROOT *mem_root) override { return NULL; }
  /* Rows also use a fixed-size format */
  enum row_type get_row_type() const override { return ROW_TYPE_FIXED; }
  ulonglong table_flags() const override
  {
    return (HA_FAST_KEY_READ | /*HA_NO_BLOBS |*/ HA_NULL_IN_KEY |
            HA_CAN_SQL_HANDLER |
            HA_REC_NOT_IN_SEQ | HA_NO_TRANSACTIONS |
            HA_HAS_RECORDS);
  }
  ulong index_flags(uint inx, uint part, bool all_parts) const override
  {
    return HA_ONLY_WHOLE_INDEX | HA_KEY_SCAN_NOT_ROR;
  }
  ha_rows records() override { return HA_POS_ERROR; }

  int open(const char *name, int mode, uint test_if_locked) override
  { return 0; }
  int close(void) override { return 0; }
  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar * buf, uchar *pos) override;
  void position(const uchar *record) override;
  int info(uint) override;
  int extra(enum ha_extra_function operation) override { return 0; }
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
			     enum thr_lock_type lock_type) override
    { return NULL; }
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info)
    override { return 1; }
};


/*
  Helper class that creates the temporary table that
  represents the table function in the query.
*/
  
class Create_json_table: public Data_type_statistics
{
  // The following members are initialized only in start()
  Field **m_default_field;
  uchar	*m_bitmaps;
  // The following members are initialized in ctor
  uint m_temp_pool_slot;
  uint m_null_count;
public:
  Create_json_table(const TMP_TABLE_PARAM *param,
                    bool save_sum_fields)
   :m_temp_pool_slot(MY_BIT_NONE),
    m_null_count(0)
  { }

  void add_field(TABLE *table, Field *field, uint fieldnr);

  TABLE *start(THD *thd,
               TMP_TABLE_PARAM *param,
               Table_function_json_table *jt,
               const LEX_CSTRING *table_alias);

  bool add_json_table_fields(THD *thd, TABLE *table,
                             Table_function_json_table *jt);
  bool finalize(THD *thd, TABLE *table, TMP_TABLE_PARAM *param,
                Table_function_json_table *jt);
};


/*
  @brief
    Start scanning the JSON document in [str ... end]

  @detail
    Note: non-root nested paths are set to scan one JSON node (that is, a
    "subdocument").
*/

void Json_table_nested_path::scan_start(CHARSET_INFO *i_cs,
                                        const uchar *str, const uchar *end)
{
  json_get_path_start(&m_engine, i_cs, str, end, &m_cur_path);
  m_cur_nested= NULL;
  m_null= false;
  m_ordinality_counter= 0;
}


/*
  @brief
    Find the next JSON element that matches the search path.
*/

int Json_table_nested_path::scan_next()
{
  bool no_records_found= false;
  if (m_cur_nested)
  {
    for (;;)
    {
      if (m_cur_nested->scan_next() == 0)
        return 0;
      if (!(m_cur_nested= m_cur_nested->m_next_nested))
        break;
handle_new_nested:
      m_cur_nested->scan_start(m_engine.s.cs, m_engine.value_begin,
                               m_engine.s.str_end);
    }
    if (no_records_found)
      return 0;
  }

  DBUG_ASSERT(!m_cur_nested);

  while (!json_get_path_next(&m_engine, &m_cur_path))
  {
    if (json_path_compare(&m_path, &m_cur_path, m_engine.value_type))
      continue;
    /* path found. */
    ++m_ordinality_counter;

    if (!m_nested)
      return 0;

    m_cur_nested= m_nested;
    no_records_found= true;
    goto handle_new_nested;
  }

  m_null= true;
  return 1;
}


int ha_json_table::rnd_init(bool scan)
{
  Json_table_nested_path &p= m_jt->m_nested_path;
  DBUG_ENTER("ha_json_table::rnd_init");

  if ((m_js= m_jt->m_json->val_str(&m_tmps)))
  {
    p.scan_start(m_js->charset(),
                 (const uchar *) m_js->ptr(), (const uchar *) m_js->end());
  }

  DBUG_RETURN(0);
}


/*
  @brief
     Store JSON value in an SQL field, doing necessary special conversions
     for JSON's null, true, and false.
*/

static int store_json_in_field(Field *f, const json_engine_t *je)
{
  switch (je->value_type)
  {
  case JSON_VALUE_NULL:
    f->set_null();
    return 0;

  case JSON_VALUE_TRUE:
  case JSON_VALUE_FALSE:
  {
    Item_result rt= f->result_type();
    if (rt == INT_RESULT || rt == DECIMAL_RESULT || rt == REAL_RESULT)
      return f->store(je->value_type == JSON_VALUE_TRUE, false);
    break;
  }
  default:
    break;
  };

  return f->store((const char *) je->value, (uint32) je->value_len, je->s.cs);
}


bool Json_table_nested_path::check_error(const char *str)
{
  if (m_engine.s.error)
  {
    report_json_error_ex(str, &m_engine, "JSON_TABLE", 0,
                         Sql_condition::WARN_LEVEL_ERROR);
    return true; // Error
  }
  return false; // Ok
}


int ha_json_table::rnd_next(uchar *buf)
{
  int res;
  enum_check_fields cf_orig;

  if (!m_js)
    return HA_ERR_END_OF_FILE;

  /*
    Step 1: Move the root nested path to the next record (this implies moving
    its child nested paths accordingly)
  */
  if (m_jt->m_nested_path.scan_next())
  {
    if (m_jt->m_nested_path.check_error(m_js->ptr()))
    {
      /*
        We already reported an error, so returning an
        error code that just doesn't produce extra
        messages.
      */
      return HA_ERR_TABLE_IN_FK_CHECK;
    }
    return HA_ERR_END_OF_FILE;
  }
  
  /*
    Step 2: Read values for all columns (the columns refer to nested paths
    they are in).
  */
  cf_orig= table->in_use->count_cuted_fields;
  table->in_use->count_cuted_fields= CHECK_FIELD_EXPRESSION;
  res= fill_column_values(buf, NULL);
  table->in_use->count_cuted_fields= cf_orig;
  return res ? HA_ERR_TABLE_IN_FK_CHECK : 0;
}


/*
  @brief
    Fill values of table columns, taking data either from Json_nested_path
    objects, or from the rowid value

  @param pos   NULL means the data should be read from Json_nested_path
                 objects.
               Non-null value is a pointer to previously saved rowid (see
                 ha_json_table::position() for description)
*/

int ha_json_table::fill_column_values(uchar * buf, uchar *pos)
{
  Field **f= table->field;
  Json_table_column *jc;
  List_iterator_fast<Json_table_column> jc_i(m_jt->m_columns);
  my_ptrdiff_t ptrdiff= buf - table->record[0];

  while ((jc= jc_i++))
  {
    bool is_null_value;
    uint int_pos= 0; /* just to make compilers happy. */

    if (!bitmap_is_set(table->read_set, (*f)->field_index))
      goto cont_loop;

    if (ptrdiff)
      (*f)->move_field_offset(ptrdiff);

    /*
      Read the NULL flag:
       - if we are reading from a rowid value, 0 means SQL NULL.
       - if scanning json document, read it from the nested path
    */
    if (pos)
      is_null_value= !(int_pos= uint4korr(pos));
    else
     is_null_value= jc->m_nest->m_null;

    if (is_null_value)
    {
      (*f)->set_null();
    }
    else
    {
      (*f)->set_notnull();
      switch (jc->m_column_type)
      {
      case Json_table_column::FOR_ORDINALITY:
      {
        /*
          Read the cardinality counter:
           - read it from nested path when scanning the json document
           - or, read it from rowid when in rnd_pos() call
        */
        longlong counter= pos? int_pos: jc->m_nest->m_ordinality_counter;
        (*f)->store(counter, TRUE);
        break;
      }
      case Json_table_column::PATH:
      case Json_table_column::EXISTS_PATH:
      {
        json_engine_t je;
        json_path_step_t *cur_step;
        uint array_counters[JSON_DEPTH_LIMIT];
        int not_found;
        const uchar* node_start;
        const uchar* node_end;

        /*
          Get the JSON context node that we will need to evaluate PATH or
          EXISTS against:
           - when scanning the json document, read it from nested path
           - when in rnd_pos call, the rowid has the start offset.
        */
        if (pos)
        {
          node_start= (const uchar *) (m_js->ptr() + (int_pos-1));
          node_end= (const uchar *) m_js->end();
        }
        else
        {
          node_start= jc->m_nest->get_value();
          node_end=   jc->m_nest->get_value_end();
        }

        json_scan_start(&je, m_js->charset(), node_start, node_end);

        cur_step= jc->m_path.steps;
        not_found= json_find_path(&je, &jc->m_path, &cur_step, array_counters) ||
                   json_read_value(&je);

        if (jc->m_column_type == Json_table_column::EXISTS_PATH)
        {
          (*f)->store(!not_found);
        }
        else /*PATH*/
        {
          if (not_found)
          {
            if (jc->m_on_empty.respond(jc, *f))
              goto error_return;
          }
          else
          {
            if (!json_value_scalar(&je) ||
                store_json_in_field(*f, &je))
            {
              if (jc->m_on_error.respond(jc, *f))
                goto error_return;
            }
            else
            {
              /*
                If the path contains wildcards, check if there are
                more matches for it in json and report an error if so.
              */
              if (jc->m_path.types_used &
                    (JSON_PATH_WILD | JSON_PATH_DOUBLE_WILD) &&
                  (json_scan_next(&je) ||
                   !json_find_path(&je, &jc->m_path, &cur_step,
                                   array_counters)))
              {
                if (jc->m_on_error.respond(jc, *f))
                  goto error_return;
              }
            }
          }
        }
        break;
      }
      };
    }
    if (ptrdiff)
      (*f)->move_field_offset(-ptrdiff);
cont_loop:
    f++;
    if (pos)
      pos+= 4;
  }
  return 0;
error_return:
  return 1;
}


int ha_json_table::rnd_pos(uchar * buf, uchar *pos)
{
  return fill_column_values(buf, pos);
}


/*
  The reference has 4 bytes for every column of the JSON_TABLE.
  There it keeps 0 for the NULL values, ordinality index for
  the ORDINALITY columns and the offset of the field's data in
  the JSON for other column types.
*/
void ha_json_table::position(const uchar *record)
{
  uchar *c_ref= ref;
  Json_table_column *jc;
  List_iterator_fast<Json_table_column> jc_i(m_jt->m_columns);

  while ((jc= jc_i++))
  {
    if (jc->m_nest->m_null)
    {
      int4store(c_ref, 0);
    }
    else
    {
      switch (jc->m_column_type)
      {
      case Json_table_column::FOR_ORDINALITY:
        int4store(c_ref, jc->m_nest->m_ordinality_counter);
        break;
      case Json_table_column::PATH:
      case Json_table_column::EXISTS_PATH:
      {
        size_t pos= jc->m_nest->get_value() -
                    (const uchar *) m_js->ptr() + 1;
        int4store(c_ref, pos);
        break;
      }
      };
    }
    c_ref+= 4;
  }
}


int ha_json_table::info(uint)
{
  /*
    We don't want 0 or 1 in stats.records.
    Though this value shouldn't matter as the optimizer
    supposed to use Table_function_json_table::get_estimates
    to obtain this data.
  */
  stats.records= 4;
  return 0;
}


void Create_json_table::add_field(TABLE *table, Field *field, uint fieldnr)
{
  DBUG_ASSERT(!field->field_name.str ||
              strlen(field->field_name.str) == field->field_name.length);

  if (!(field->flags & NOT_NULL_FLAG))
    m_null_count++;

  table->s->reclength+= field->pack_length();

  // Assign it here, before update_data_type_statistics() changes m_blob_count
  if (field->flags & BLOB_FLAG)
    table->s->blob_field[m_blob_count]= fieldnr;

  table->field[fieldnr]= field;
  field->field_index= fieldnr;

  field->update_data_type_statistics(this);
}


/**
  Create a json table according to a field list.

  @param thd                  thread handle
  @param param                a description used as input to create the table
  @param jt                   json_table specificaion
  @param table_alias          alias
*/

TABLE *Create_json_table::start(THD *thd,
                               TMP_TABLE_PARAM *param,
                               Table_function_json_table *jt,
                               const LEX_CSTRING *table_alias)
{
  MEM_ROOT *mem_root_save, own_root;
  TABLE *table;
  TABLE_SHARE *share;
  uint  copy_func_count= param->func_count;
  char  *tmpname,path[FN_REFLEN];
  Field **reg_field;
  uint *blob_field;
  DBUG_ENTER("Create_json_table::start");
  DBUG_PRINT("enter",
             ("table_alias: '%s'  ", table_alias->str));

  if (use_temp_pool && !(test_flags & TEST_KEEP_TMP_TABLES))
    m_temp_pool_slot = bitmap_lock_set_next(&temp_pool);

  if (m_temp_pool_slot != MY_BIT_NONE) // we got a slot
    sprintf(path, "%s-%lx-%i", tmp_file_prefix,
            current_pid, m_temp_pool_slot);
  else
  {
    /* if we run out of slots or we are not using tempool */
    sprintf(path, "%s-%lx-%lx-%x", tmp_file_prefix,current_pid,
            (ulong) thd->thread_id, thd->tmp_table++);
  }

  /*
    No need to change table name to lower case.
  */
  fn_format(path, path, mysql_tmpdir, "",
            MY_REPLACE_EXT|MY_UNPACK_FILENAME);

  const uint field_count= param->field_count;
  DBUG_ASSERT(field_count);

  init_sql_alloc(key_memory_TABLE, &own_root,
                 TABLE_ALLOC_BLOCK_SIZE, 0, MYF(MY_THREAD_SPECIFIC));

  if (!multi_alloc_root(&own_root,
                        &table, sizeof(*table),
                        &share, sizeof(*share),
                        &reg_field, sizeof(Field*) * (field_count+1),
                        &m_default_field, sizeof(Field*) * (field_count),
                        &blob_field, sizeof(uint)*(field_count+1),
                        &param->items_to_copy,
                          sizeof(param->items_to_copy[0])*(copy_func_count+1),
                        &param->keyinfo, sizeof(*param->keyinfo),
                        &param->start_recinfo,
                        sizeof(*param->recinfo)*(field_count*2+4),
                        &tmpname, (uint) strlen(path)+1,
                        &m_bitmaps, bitmap_buffer_size(field_count)*6,
                        NullS))
  {
    DBUG_RETURN(NULL);				/* purecov: inspected */
  }
  strmov(tmpname, path);
  /* make table according to fields */

  bzero((char*) table,sizeof(*table));
  bzero((char*) reg_field, sizeof(Field*) * (field_count+1));
  bzero((char*) m_default_field, sizeof(Field*) * (field_count));

  table->mem_root= own_root;
  mem_root_save= thd->mem_root;
  thd->mem_root= &table->mem_root;

  table->field=reg_field;
  table->alias.set(table_alias->str, table_alias->length, table_alias_charset);

  table->reginfo.lock_type=TL_WRITE;	/* Will be updated */
  table->map=1;
  table->temp_pool_slot= m_temp_pool_slot;
  table->copy_blobs= 1;
  table->in_use= thd;
  table->no_rows_with_nulls= param->force_not_null_cols;

  table->s= share;
  init_tmp_table_share(thd, share, "", 0, "(temporary)", tmpname);
  share->blob_field= blob_field;
  share->table_charset= param->table_charset;
  share->primary_key= MAX_KEY;               // Indicate no primary key
  share->not_usable_by_query_cache= FALSE;
  if (param->schema_table)
    share->db= INFORMATION_SCHEMA_NAME;

  param->using_outer_summary_function= 0;

  share->db_plugin= NULL;
  if (!(table->file= new (&table->mem_root) ha_json_table(share, jt)))
    DBUG_RETURN(NULL);

  table->file->init();

  thd->mem_root= mem_root_save;
  DBUG_RETURN(table);
}


bool Create_json_table::finalize(THD *thd, TABLE *table,
                                 TMP_TABLE_PARAM *param,
                                 Table_function_json_table *jt)
{
  DBUG_ENTER("Create_json_table::finalize");
  DBUG_ASSERT(table);

  uint null_pack_length;
  bool  use_packed_rows= false;
  uchar *pos;
  uchar *null_flags;
  TMP_ENGINE_COLUMNDEF *recinfo;
  TABLE_SHARE  *share= table->s;

  MEM_ROOT *mem_root_save= thd->mem_root;
  thd->mem_root= &table->mem_root;

  DBUG_ASSERT(param->field_count >= share->fields);
  DBUG_ASSERT(param->field_count >= share->blob_fields);

  if (table->file->set_ha_share_ref(&share->ha_share))
  {
    delete table->file;
    goto err;
  }

  if (share->blob_fields == 0)
    m_null_count++;

  null_pack_length= (m_null_count + m_uneven_bit_length + 7) / 8;
  share->reclength+= null_pack_length;
  if (!share->reclength)
    share->reclength= 1;                // Dummy select

  {
    uint alloc_length= ALIGN_SIZE(share->reclength + MI_UNIQUE_HASH_LENGTH+1);
    share->rec_buff_length= alloc_length;
    if (!(table->record[0]= (uchar*)
                            alloc_root(&table->mem_root, alloc_length*3)))
      goto err;
    table->record[1]= table->record[0]+alloc_length;
    share->default_values= table->record[1]+alloc_length;
  }

  setup_tmp_table_column_bitmaps(table, m_bitmaps, table->s->fields);

  recinfo=param->start_recinfo;
  null_flags=(uchar*) table->record[0];
  pos=table->record[0]+ null_pack_length;
  if (null_pack_length)
  {
    bzero((uchar*) recinfo,sizeof(*recinfo));
    recinfo->type=FIELD_NORMAL;
    recinfo->length=null_pack_length;
    recinfo++;
    bfill(null_flags,null_pack_length,255);	// Set null fields

    table->null_flags= (uchar*) table->record[0];
    share->null_fields= m_null_count;
    share->null_bytes= share->null_bytes_for_compare= null_pack_length;
  }
  m_null_count= (share->blob_fields == 0) ? 1 : 0;
  for (uint i= 0; i < share->fields; i++, recinfo++)
  {
    Field *field= table->field[i];
    uint length;
    bzero((uchar*) recinfo,sizeof(*recinfo));

    if (!(field->flags & NOT_NULL_FLAG))
    {
      recinfo->null_bit= (uint8)1 << (m_null_count & 7);
      recinfo->null_pos= m_null_count/8;
      field->move_field(pos, null_flags + m_null_count/8,
			(uint8)1 << (m_null_count & 7));
      m_null_count++;
    }
    else
      field->move_field(pos,(uchar*) 0,0);
    if (field->type() == MYSQL_TYPE_BIT)
    {
      /* We have to reserve place for extra bits among null bits */
      ((Field_bit*) field)->set_bit_ptr(null_flags + m_null_count / 8,
                                        m_null_count & 7);
      m_null_count+= (field->field_length & 7);
    }
    field->reset();

    /*
      Test if there is a default field value. The test for ->ptr is to skip
      'offset' fields generated by initialize_tables
    */
    if (m_default_field[i] && m_default_field[i]->ptr)
    {
      /* 
         default_field[i] is set only in the cases  when 'field' can
         inherit the default value that is defined for the field referred
         by the Item_field object from which 'field' has been created.
      */
      const Field *orig_field= m_default_field[i];
      /* Get the value from default_values */
      if (orig_field->is_null_in_record(orig_field->table->s->default_values))
        field->set_null();
      else
      {
        field->set_notnull();
        memcpy(field->ptr,
               orig_field->ptr_in_record(orig_field->table->s->default_values),
               field->pack_length_in_rec());
      }
    } 

    length=field->pack_length();
    pos+= length;

    /* Make entry for create table */
    recinfo->length=length;
    recinfo->type= field->tmp_engine_column_type(use_packed_rows);

    // fix table name in field entry
    field->set_table_name(&table->alias);
  }

  param->recinfo= recinfo;              	// Pointer to after last field
  store_record(table,s->default_values);        // Make empty default record

  share->max_rows= ~(ha_rows) 0;
  param->end_write_records= HA_POS_ERROR;

  share->db_record_offset= 1;

  if (unlikely(table->file->ha_open(table, table->s->path.str, O_RDWR,
                             HA_OPEN_TMP_TABLE | HA_OPEN_INTERNAL_TABLE)))
    goto err;

  table->db_stat= HA_OPEN_KEYFILE;
  table->set_created();

  thd->mem_root= mem_root_save;

  DBUG_RETURN(false);

err:
  thd->mem_root= mem_root_save;
  DBUG_RETURN(true);
}


/*
  @brief
    Read the JSON_TABLE's field definitions from @jt and add the fields to
    table @table.
*/

bool Create_json_table::add_json_table_fields(THD *thd, TABLE *table,
                                              Table_function_json_table *jt)
{
  TABLE_SHARE *share= table->s;
  Json_table_column *jc;
  uint fieldnr= 0;
  MEM_ROOT *mem_root_save= thd->mem_root;
  List_iterator_fast<Json_table_column> jc_i(jt->m_columns);

  DBUG_ENTER("add_json_table_fields");

  thd->mem_root= &table->mem_root;

  while ((jc= jc_i++))
  {
    Create_field *sql_f= jc->m_field;
    List_iterator_fast<Json_table_column> it2(jt->m_columns);
    Json_table_column *jc2;
     /*
       Initialize length from its original value (number of characters),
       which was set in the parser. This is necessary if we're
       executing a prepared statement for the second time.
    */
    sql_f->length= sql_f->char_length;
    if (!(jc->m_explicit_cs= sql_f->charset))
      sql_f->charset= thd->variables.collation_server;

    if (sql_f->prepare_stage1(thd, thd->mem_root, table->file,
                              table->file->ha_table_flags()))
      goto err_exit;

    while ((jc2= it2++) != jc)
    {
      if (lex_string_cmp(system_charset_info,
            &sql_f->field_name, &jc2->m_field->field_name) == 0)
      {
        my_error(ER_DUP_FIELDNAME, MYF(0), sql_f->field_name.str);
        goto err_exit;
      }
    }
    it2.rewind();
  }

  jc_i.rewind();

  while ((jc= jc_i++))
  {
    Create_field *sql_f= jc->m_field;
    Record_addr addr(!(sql_f->flags & NOT_NULL_FLAG));
    Bit_addr bit(addr.null());

    sql_f->prepare_stage2(table->file, table->file->ha_table_flags());

    if (!sql_f->charset)
      sql_f->charset= &my_charset_utf8mb4_bin;

    Field *f= sql_f->type_handler()->make_table_field_from_def(share,
        thd->mem_root, &sql_f->field_name, addr, bit, sql_f, sql_f->flags);
    if (!f)
      goto err_exit;
    f->init(table);
    add_field(table, f, fieldnr++);
  }

  share->fields= fieldnr;
  share->blob_fields= m_blob_count;
  table->field[fieldnr]= 0;                     // End marker
  share->blob_field[m_blob_count]= 0;           // End marker
  share->column_bitmap_size= bitmap_buffer_size(share->fields);

  thd->mem_root= mem_root_save;

  DBUG_RETURN(FALSE);
err_exit:
  thd->mem_root= mem_root_save;
  DBUG_RETURN(TRUE);
}


/*
  @brief
    Given a TABLE_LIST representing JSON_TABLE(...) syntax, create a temporary
    table for it.

  @detail
    The temporary table will have:
    - fields whose names/datatypes are specified in JSON_TABLE(...) syntax
    - a ha_json_table as the storage engine.

    The uses of the temporary table are:
    - name resolution: the query may have references to the columns of
      JSON_TABLE(...). A TABLE object will allow to resolve them.
    - query execution: ha_json_table will produce JSON_TABLE's rows.
*/

TABLE *create_table_for_function(THD *thd, TABLE_LIST *sql_table)
{
  TMP_TABLE_PARAM tp;
  TABLE *table;
  uint field_count= sql_table->table_function->m_columns.elements+1;
  
  DBUG_ENTER("create_table_for_function");

  tp.init();
  tp.table_charset= system_charset_info;
  tp.field_count= field_count;
  {
    Create_json_table maker(&tp, false);

    if (!(table= maker.start(thd, &tp,
                             sql_table->table_function, &sql_table->alias)) ||
        maker.add_json_table_fields(thd, table, sql_table->table_function) ||
        maker.finalize(thd, table, &tp, sql_table->table_function))
    {
      if (table)
        free_tmp_table(thd, table);
      DBUG_RETURN(NULL);
    }
  }
  sql_table->schema_table_name.length= 0;

  my_bitmap_map* bitmaps=
    (my_bitmap_map*) thd->alloc(bitmap_buffer_size(field_count));
  my_bitmap_init(&table->def_read_set, (my_bitmap_map*) bitmaps, field_count,
                 FALSE);
  table->read_set= &table->def_read_set;
  bitmap_clear_all(table->read_set);
  table->alias_name_used= true;
  table->next= thd->derived_tables;
  thd->derived_tables= table;
  table->s->tmp_table= INTERNAL_TMP_TABLE;
  table->grant.privilege= SELECT_ACL;

  sql_table->table= table;

  DBUG_RETURN(table);
}


int Json_table_column::set(THD *thd, enum_type ctype, const LEX_CSTRING &path)
{
  set(ctype);
  if (json_path_setup(&m_path, thd->variables.collation_connection,
        (const uchar *) path.str, (const uchar *)(path.str + path.length)))
  {
    report_path_error_ex(path.str, &m_path, "JSON_TABLE", 1,
                         Sql_condition::WARN_LEVEL_ERROR);
    return 1;
  }

  /*
    This is done so the ::print function can just print the path string.
    Can be removed if we redo that function to print the path using it's
    anctual content. Not sure though if we should.
  */
  m_path.s.c_str= (const uchar *) path.str;
  return 0;
}


static int print_path(String *str, const json_path_t *p)
{
  return str->append('\'') ||
         str->append_for_single_quote((const char *) p->s.c_str,
                                      p->s.str_end - p->s.c_str) ||
         str->append('\'');
}


/*
  Print the string representation of the Json_table_column.

  @param thd        - the thread
  @param f          - the remaining array of Field-s from the table
                       if the Json_table_column  
  @param str        - the string where to print
*/
int Json_table_column::print(THD *thd, Field **f, String *str)
{
  StringBuffer<MAX_FIELD_WIDTH> column_type(str->charset());

  if (append_identifier(thd, str, &m_field->field_name) ||
      str->append(' '))
    return 1;

  switch (m_column_type)
  {
  case FOR_ORDINALITY:
    if (str->append("FOR ORDINALITY"))
      return 1;
    break;
  case EXISTS_PATH:
  case PATH:
    (*f)->sql_type(column_type);

    if (str->append(column_type) ||
        str->append(m_column_type == PATH ? " PATH " : " EXISTS PATH ") ||
        print_path(str, &m_path))
      return 1;
    break;
  };

  if (m_on_empty.print("EMPTY", str) ||
      m_on_error.print("ERROR", str))
    return 1;

  return 0;
}


int Json_table_nested_path::set_path(THD *thd, const LEX_CSTRING &path)
{
  if (json_path_setup(&m_path, thd->variables.collation_connection,
        (const uchar *) path.str, (const uchar *)(path.str + path.length)))
  {
    report_path_error_ex(path.str, &m_path, "JSON_TABLE", 1,
                         Sql_condition::WARN_LEVEL_ERROR);
    return 1;
  }

  /*
    This is done so the ::print function can just print the path string.
    Can be removed if we redo that function to print the path using its
    actual content. Not sure though if we should.
  */
  m_path.s.c_str= (const uchar *) path.str;
  return 0;
}


/*
  @brief 
    Perform the action of this response on field @f (emit an error, or set @f
    to NULL, or set it to default value).
*/

int Json_table_column::On_response::respond(Json_table_column *jc, Field *f)
{
  switch (m_response)
  {
    case Json_table_column::RESPONSE_NOT_SPECIFIED:
    case Json_table_column::RESPONSE_NULL:
      f->set_null();
      break;
    case Json_table_column::RESPONSE_ERROR:
      f->set_null();
      my_error(ER_JSON_TABLE_ERROR_ON_FIELD, MYF(0),
          f->field_name.str, f->table->alias.ptr());
      return 1;
    case Json_table_column::RESPONSE_DEFAULT:
      f->set_notnull();
      f->store(m_default.str,
          m_default.length, jc->m_defaults_cs);
      break;
  }
  return 0;
}


int Json_table_column::On_response::print(const char *name, String *str) const
{
  const char *resp;
  const LEX_CSTRING *ds= NULL;
  if (m_response == Json_table_column::RESPONSE_NOT_SPECIFIED)
    return 0;

  switch (m_response)
  {
    case Json_table_column::RESPONSE_NULL:
      resp= "NULL";
      break;
    case Json_table_column::RESPONSE_ERROR:
      resp= "ERROR";
      break;
    case Json_table_column::RESPONSE_DEFAULT:
    {
      resp= "DEFAULT";
      ds= &m_default;
      break;
    }
    default:
      resp= NULL;
      DBUG_ASSERT(FALSE); /* should never happen. */
  }

  return
    (str->append(' ') || str->append(resp)  ||
    (ds && (str->append(" '") ||
            str->append_for_single_quote(ds->str, ds->length) ||
            str->append('\''))) ||
    str->append(" ON ") ||
    str->append(name));
}


void Table_function_json_table::start_nested_path(Json_table_nested_path *np)
{
  np->m_parent= cur_parent;
  *last_sibling_hook= np;

  // Make the newly added path the parent
  cur_parent= np;
  last_sibling_hook= &np->m_nested;
}


void Table_function_json_table::end_nested_path()
{
  last_sibling_hook= &cur_parent->m_next_nested;
  cur_parent= cur_parent->m_parent;
}


/*
  @brief
    Perform name-resolution phase tasks

  @detail
    - The only argument that needs resolution is the JSON text
    - Then, we need to set dependencies: if JSON_TABLE refers to table's
      column, e.g.

         JSON_TABLE (t1.col ... ) AS t2

      then it can be computed only after table t1.
    - The dependencies must not form a loop.
*/

int Table_function_json_table::setup(THD *thd, TABLE_LIST *sql_table,
                                     SELECT_LEX *s_lex)
{
  TABLE *t= sql_table->table;

  thd->where= "JSON_TABLE argument";
  {
    bool save_is_item_list_lookup;
    bool res;
    save_is_item_list_lookup= thd->lex->current_select->is_item_list_lookup;
    thd->lex->current_select->is_item_list_lookup= 0;
    s_lex->end_lateral_table= sql_table;
    res= m_json->fix_fields_if_needed(thd, &m_json);
    s_lex->end_lateral_table= NULL;
    thd->lex->current_select->is_item_list_lookup= save_is_item_list_lookup;
    if (res)
      return TRUE;
  }

  {
    List_iterator_fast<Json_table_column> jc_i(m_columns);
    for (uint i= 0; t->field[i]; i++)
    {
      Json_table_column *jc= jc_i++;
      t->field[i]->change_charset(
          jc->m_explicit_cs ? jc->m_explicit_cs : m_json->collation);
      /*
        The m_field->charset is going to be reused if it's the prepared
        statement running several times. So should restored the original
        value.
      */
      jc->m_field->charset= jc->m_explicit_cs;
    }
  }

  m_dep_tables= m_json->used_tables();

  if (m_dep_tables)
  {
    t->no_cache= TRUE;
    if (unlikely(m_dep_tables & sql_table->get_map()))
    {
      /* Table itself is used in the argument. */
      my_error(ER_WRONG_USAGE, MYF(0), "JSON_TABLE", "argument"); 
      return TRUE;
    }
  }

  return FALSE;
}

void Table_function_json_table::get_estimates(ha_rows *out_rows,
                                              double *scan_time,
                                              double *startup_cost)
{
  *out_rows= 40;
  *scan_time= 0.0;
  *startup_cost= 0.0;
}


/*
  Print the string representation of the Json_nested_path object.
  Which is the COLUMNS(...) part of the JSON_TABLE definition.
 
  @param thd         - the thread
  @param f           - the remaining part of the array of Field* objects
                         taken from the TABLE.
                         It's needed as Json_table_column objects
                         don't have links to the related Field-s.
  @param str         - the string where to print
  @param it          - the remaining part of the Json_table_column list
  @param last_column - the last column taken from the list.
*/

int Json_table_nested_path::print(THD *thd, Field ***f, String *str,
                                  List_iterator_fast<Json_table_column> &it,
                                  Json_table_column **last_column)
{
  Json_table_nested_path *c_path= this;
  Json_table_nested_path *c_nested= m_nested;
  Json_table_column *jc= *last_column;
  bool first_column= TRUE;

  if (str->append("COLUMNS ("))
    return 1;

  do
  {
    if (first_column)
      first_column= FALSE;
    else if (str->append(", "))
      return 1;

    if (jc->m_nest == c_path)
    {
      if (jc->print(thd, *f, str))
        return 1;
      if (!(jc= it++))
        goto exit_ok;
      ++(*f);
    }
    else if (jc->m_nest == c_nested)
    {
      if (str->append("NESTED PATH ") ||
          print_path(str, &jc->m_nest->m_path) ||
          c_nested->print(thd, f, str, it, &jc))
        return 1;
      c_nested= c_nested->m_next_nested;
    }
    else
      break;
  } while(jc);

exit_ok:
  if (str->append(")"))
    return 1;

  *last_column= jc;
  return 0;
}


/*
  Print the SQL definition of the JSON_TABLE.
  Used mostly as a part of the CREATE VIEW statement.
 
  @param thd        - the thread
  @param sql_table  - the corresponding TABLE_LIST object
  @param str        - the string where to print
  @param query_type - the query type
*/
int Table_function_json_table::print(THD *thd, TABLE_LIST *sql_table,
                                     String *str, enum_query_type query_type)
{
  List_iterator_fast<Json_table_column> jc_i(m_columns);
  Json_table_column *jc= jc_i++;
  Field **f_list= sql_table->table->field;

  DBUG_ENTER("Table_function_json_table::print");

  if (str->append("JSON_TABLE("))
    DBUG_RETURN(TRUE);

  m_json->print(str, query_type);

  if (str->append(", ") ||
      print_path(str, &m_nested_path.m_path) ||
      str->append(' ') ||
      m_nested_path.print(thd, &f_list, str, jc_i, &jc) ||
      str->append(')'))
    DBUG_RETURN(TRUE);

  DBUG_RETURN(0);
}


void Table_function_json_table::fix_after_pullout(TABLE_LIST *sql_table,
       st_select_lex *new_parent, bool merge)
{
  sql_table->dep_tables&= ~m_dep_tables;

  m_json->fix_after_pullout(new_parent, &m_json, merge);
  m_dep_tables= m_json->used_tables();

  sql_table->dep_tables|= m_dep_tables;
}


/*
  @brief
     Recursively make all tables in the join_list also depend on deps.
*/

static void add_extra_deps(List<TABLE_LIST> *join_list, table_map deps)
{
  TABLE_LIST *table;
  List_iterator<TABLE_LIST> li(*join_list);
  while ((table= li++))
  {
    table->dep_tables |= deps;
    NESTED_JOIN *nested_join;
    if ((nested_join= table->nested_join))
    {
       // set the deps inside, too
       add_extra_deps(&nested_join->join_list, deps);
    }
  }
}


/*
  @brief
    Add table dependencies that are directly caused by table functions, also
    add extra dependencies so that the join optimizer does not construct
    "dead-end" join prefixes.

  @detail
    There are two kinds of limitations on join order:
    1A. Outer joins require that inner tables follow outer.
    1B. Tables within a join nest must be present in the join order
        "without interleaving". See check_interleaving_with_nj for details.

    2. Table function argument may refer to *any* table that precedes the
    current table in the query text. The table maybe outside of the current
    nested join and/or inside another nested join.

    One may think that adding dependency according to #2 would be sufficient,
    but this is not the case.

    @example

      select ...
      from
        t20 left join t21 on t20.a=t21.a
      join
        (t31 left join (t32 join
                        JSON_TABLE(t21.js,
                                   '$' COLUMNS (ab INT PATH '$.a')) AS jt
                       ) on t31.a<3
        )

      Here, jt's argument refers to t21.

      Table dependencies are:
        t21 -> t20
        t32 -> t31
        jt  -> t21 t31  (also indirectly depends on t20 through t21)

      This allows to construct a "dead-end" join prefix, like:

       t31, t32

      Here, "no interleaving" rule requires the next table to be jt, but we
      can't add it, because it depends on t21 which is not in the join prefix.

    @end example

    Dead-end join prefixes do not work with join prefix pruning done for
    @@optimizer_prune_level: it is possible that all non-dead-end prefixes are
    pruned away.

    The solution is as follows: if there is an outer join that contains
    (directly on indirectly) a table function JT which has a reference JREF
    outside of the outer join:

      left join ( T_I ... json_table(JREF, ...) as JT ...)

    then make *all* tables T_I also dependent on outside references in JREF.
    This way, the optimizer will put table T_I into the join prefix only when
    JT can be put there as well, and "dead-end" prefixes will not be built.

  @param  join_list    List of tables to process. Initial invocation should
                       supply the JOIN's top-level table list.
  @param  nest_tables  Bitmap of all tables in the join list.

  @return Bitmap of all outside references that tables in join_list have
*/

table_map add_table_function_dependencies(List<TABLE_LIST> *join_list,
                                          table_map nest_tables)
{
  TABLE_LIST *table;
  table_map res= 0;
  List_iterator<TABLE_LIST> li(*join_list);

  // Recursively compute extra dependencies
  while ((table= li++))
  {
    NESTED_JOIN *nested_join;
    if ((nested_join= table->nested_join))
    {
      res |= add_table_function_dependencies(&nested_join->join_list,
                                             nested_join->used_tables);
    }
    else if (table->table_function)
    {
      table->dep_tables |= table->table_function->used_tables();
      res |= table->dep_tables;
    }
  }
  res= res & ~nest_tables & ~PSEUDO_TABLE_BITS;
  // Then, make all "peers" have them:
  if (res)
    add_extra_deps(join_list,  res);

  return res;
}



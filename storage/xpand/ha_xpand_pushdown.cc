/*****************************************************************************
Copyright (c) 2019, MariaDB Corporation.
*****************************************************************************/

#include "ha_xpand.h"
#include "ha_xpand_pushdown.h"

extern handlerton *xpand_hton;
extern uint xpand_row_buffer;

/*@brief  Fills up array data types, metadata and nullability*/
/************************************************************
 * DESCRIPTION:
 * Fills up three arrays with: field binlog data types, field
 * metadata and nullability bitmask as in Table_map_log_event
 * ctor. Internally creates a temporary table as does
 * Pushdown_select. DH uses the actual temp table w/o
 * b/c create_DH is called later compared to create_SH.
 * More details in server/sql/log_event_server.cc
 * PARAMETERS:
 *  thd - THD*
 *  table__ - TABLE* temp table for the results
 *  sl - SELECT_LEX*
 *  fieldtype - uchar*
 *  field_metadata - uchar*
 *  null_bits   - uchar*
 *  num_null_bytes - null bit size
 *  fields_count   - a number of fields
 * RETURN:
 *  metadata_size int or -1 in case of error
 ************************************************************/
int get_field_types(THD *thd, TABLE *table__, SELECT_LEX *sl, uchar *fieldtype,
                    uchar *field_metadata, uchar *null_bits,
                    const int num_null_bytes, const uint fields_count)
{
  int field_metadata_size = 0;
  int metadata_index = 0;
  TABLE *tmp_table= table__;

  if (!tmp_table) {
    // Construct a tmp table with fields to find out result DTs.
    // This should be reconsidered if it worths the effort.
    List<Item> types;
    TMP_TABLE_PARAM tmp_table_param;
    sl->master_unit()->join_union_item_types(thd, types, 1);
    tmp_table_param.init();
    tmp_table_param.field_count= types.elements;

    tmp_table = create_tmp_table(thd, &tmp_table_param, types, (ORDER *) 0,
                                 false, 0, TMP_TABLE_ALL_COLUMNS, 1,
                                 &empty_clex_str, true, false);
    if (!tmp_table) {
      field_metadata_size = -1;
      goto err;
    }
  }

  for (unsigned int i = 0 ; i < fields_count; ++i) {
    fieldtype[i]= tmp_table->field[i]->binlog_type();
  }

  bzero(field_metadata, (fields_count * 2));
  for (unsigned int i= 0 ; i < fields_count ; i++)
  {
    Binlog_type_info bti= tmp_table->field[i]->binlog_type_info();
    uchar *ptr = reinterpret_cast<uchar*>(&bti.m_metadata);
    memcpy(&field_metadata[metadata_index], ptr, bti.m_metadata_size);
    metadata_index+= bti.m_metadata_size;
  }

  if (metadata_index < 251)
    field_metadata_size += metadata_index + 1;
  else
    field_metadata_size += metadata_index + 3;

  bzero(null_bits, num_null_bytes);
  for (unsigned int i= 0 ; i < fields_count ; ++i) {
    if (tmp_table->field[i]->maybe_null()) {
      null_bits[(i / 8)]+= 1 << (i % 8);
    }
  }

  if (!table__)
    free_tmp_table(thd, tmp_table);
err:
  return field_metadata_size;
}

/*@brief  create_xpand_select_handler- Creates handler*/
/************************************************************
 * DESCRIPTION:
 * Creates a select handler
 * More details in server/sql/select_handler.h
 * PARAMETERS:
 *  thd - THD pointer.
 *  sel - SELECT_LEX* that describes the query.
 * RETURN:
 *  select_handler if possible
 *  NULL otherwise
 ************************************************************/
select_handler*
create_xpand_select_handler(THD* thd, SELECT_LEX* select_lex)
{
  ulonglong *oids = NULL;
  ha_xpand_select_handler *sh = NULL;
  if (!select_handler_setting(thd)) {
    return sh;
  }

  // TODO Return early for EXPLAIN before we run the actual scan.
  // We can send compile request when we separate compilation
  // and execution.
  xpand_connection_cursor *scan = NULL;
  if (thd->lex->describe) {
    sh = new ha_xpand_select_handler(thd, select_lex, scan);
    return sh;
  }

  // Multi-update runs an implicit query to collect constraints.
  // SH couldn't be used for this.
  if (thd->lex->sql_command == SQLCOM_UPDATE_MULTI) {
    return sh;
  }

  String query;
  // Print the query into a string provided
  select_lex->print(thd, &query, QT_ORDINARY);
  int error_code = 0;
  int field_metadata_size = 0;
  xpand_connection *trx = NULL;

  // We presume this number is equal to types.elements in get_field_types
  uint items_number = select_lex->get_item_list()->elements;
  uint num_null_bytes = (items_number + 7) / 8;
  uchar *fieldtype = NULL;
  uchar *null_bits = NULL;
  uchar *field_metadata = NULL;
  uchar *meta_memory= (uchar *)my_multi_malloc(MYF(MY_WME), &fieldtype, items_number,
    &null_bits, num_null_bytes, &field_metadata, (items_number * 2), NULL);

  if (!meta_memory) {
    // The only way to say something here is to raise warning
    // b/c we will fallback to other access methods: derived handler or rowstore.
    goto err;
  }

  if((field_metadata_size =
      get_field_types(thd, NULL, select_lex, fieldtype, field_metadata,
                      null_bits, num_null_bytes, items_number)) < 0) {
    goto err;
  }

  trx = get_trx(thd, &error_code);
  if (!trx)
    goto err;

  if (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
    trx->auto_commit_next();

  oids = xpand_extract_table_oids(thd, select_lex->parent_lex);
  if ((error_code = trx->scan_query(query, fieldtype, items_number, null_bits,
                                    num_null_bytes, field_metadata,
                                    field_metadata_size,
                                    row_buffer_setting(thd), oids, &scan))) {
    goto err;
  }

  sh = new ha_xpand_select_handler(thd, select_lex, scan);

err:
  if (meta_memory)
    my_free(meta_memory);

  if (error_code == HA_ERR_TABLE_DEF_CHANGED)
    xpand_mark_tables_for_discovery(select_lex->parent_lex);

  return sh;
}

/***********************************************************
 * DESCRIPTION:
 * select_handler constructor
 * PARAMETERS:
 *   thd - THD pointer.
 *   select_lex - sematic tree for the query.
 **********************************************************/
ha_xpand_select_handler::ha_xpand_select_handler(
      THD *thd,
      SELECT_LEX* select_lex,
      xpand_connection_cursor *scan_)
  : select_handler(thd, xpand_hton)
{
  thd__ = thd;
  scan = scan_;
  select = select_lex;
  rgi = NULL;
}

/***********************************************************
 * DESCRIPTION:
 * select_handler constructor
 * This frees dynamic memory allocated for bitmap
 * and disables replication to SH temp table.
 **********************************************************/
ha_xpand_select_handler::~ha_xpand_select_handler()
{
  int error_code;
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx) {
    // TBD Log this
  }
  if (trx && scan)
    trx->scan_end(scan);

  // If the ::init_scan has been executed
  if (table__)
    my_bitmap_free(&scan_fields);

  if (rgi)
    remove_current_table_from_rpl_table_list(rgi);
}

/*@brief  Initiate the query for select_handler           */
/***********************************************************
 * DESCRIPTION:
 *  Initializes dynamic structures and sets SH temp table
 *  as RBR replication destination to unpack rows.
 *  * PARAMETERS:
 * RETURN:
 *  rc as int
 * ********************************************************/
int ha_xpand_select_handler::init_scan()
{
  // Save this into the base handler class attribute
  table__ = table;
  // need this bitmap future in next_row()
  if (my_bitmap_init(&scan_fields, NULL, table->read_set->n_bits, false))
    return ER_OUTOFMEMORY;
  bitmap_set_all(&scan_fields);

  add_current_table_to_rpl_table_list(&rgi, thd__, table__);

  return 0;
}

/*@brief  Fetch next row for select_handler           */
/***********************************************************
 * DESCRIPTION:
 * Fetch next row for select_handler.
 * PARAMETERS:
 * RETURN:
 *  rc as int
 * ********************************************************/
int ha_xpand_select_handler::next_row()
{
  int error_code = 0;
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  assert(scan);

  uchar *rowdata;
  ulong rowdata_length;
  if ((error_code = trx->scan_next(scan, &rowdata, &rowdata_length)))
    return error_code;

  uchar const *current_row_end;
  ulong master_reclength;

  error_code = unpack_row(rgi, table, table->s->fields, rowdata,
                          &scan_fields, &current_row_end,
                          &master_reclength, rowdata + rowdata_length);

  if (error_code)
    return error_code;

  return 0;
}

/*@brief  Finishes the scan and clean it up               */
/***********************************************************
 * DESCRIPTION:
 * Finishes the scan for select handler
 * PARAMETERS:
 * RETURN:
 *   rc as int
 ***********************************************************/
int ha_xpand_select_handler::end_scan()
{
  return 0;
}

/*@brief  create_xpand_derived_handler- Creates handler*/
/************************************************************
 * DESCRIPTION:
 * Creates a derived handler
 * More details in server/sql/derived_handler.h
 * PARAMETERS:
 *  thd - THD pointer.
 *  derived - TABLE_LIST* that describes the tables involved
 * RETURN:
 *  derived_handler if possible
 *  NULL otherwise
 ************************************************************/
derived_handler*
create_xpand_derived_handler(THD* thd, TABLE_LIST *derived)
{
  ha_xpand_derived_handler *dh = NULL;
  if (!derived_handler_setting(thd)) {
    return dh;
  }

  SELECT_LEX_UNIT *unit= derived->derived;
  SELECT_LEX *select_lex = unit->first_select();
  String query;

  dh = new ha_xpand_derived_handler(thd, select_lex, NULL);

  return dh;
}

/***********************************************************
 * DESCRIPTION:
 * derived_handler constructor
 * PARAMETERS:
 *   thd - THD pointer.
 *   select_lex - sematic tree for the query.
 **********************************************************/
ha_xpand_derived_handler::ha_xpand_derived_handler(
      THD *thd,
      SELECT_LEX* select_lex,
      xpand_connection_cursor *scan_)
  : derived_handler(thd, xpand_hton)
{
  thd__ = thd;
  scan = scan_;
  select = select_lex;
  rgi = NULL;
}

/***********************************************************
 * DESCRIPTION:
 * derived_handler constructor
 * This frees dynamic memory allocated for bitmap
 * and disables replication to SH temp table.
 **********************************************************/
ha_xpand_derived_handler::~ha_xpand_derived_handler()
{
  int error_code;
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx) {
    // TBD Log this.
  }
  if (trx && scan)
    trx->scan_end(scan);

  // If the ::init_scan has been executed
  if (table__)
    my_bitmap_free(&scan_fields);

  if (rgi)
    remove_current_table_from_rpl_table_list(rgi);
}

/*@brief  Initiate the query for derived_handler           */
/***********************************************************
 * DESCRIPTION:
 *  Initializes dynamic structures and sets SH temp table
 *  as RBR replication destination to unpack rows.
 *  * PARAMETERS:
 * RETURN:
 *  rc as int
 * ********************************************************/
int ha_xpand_derived_handler::init_scan()
{
  String query;
  // Print the query into a string provided
  select->print(thd__, &query, QT_ORDINARY);
  int error_code = 0;
  int field_metadata_size = 0;
  xpand_connection *trx = NULL;
  ulonglong *oids = NULL;

  // We presume this number is equal to types.elements in get_field_types
  uint items_number= select->get_item_list()->elements;
  uint num_null_bytes = (items_number + 7) / 8;
  uchar *fieldtype = NULL;
  uchar *null_bits = NULL;
  uchar *field_metadata = NULL;
  uchar *meta_memory= (uchar *)my_multi_malloc(MYF(MY_WME), &fieldtype, items_number,
    &null_bits, num_null_bytes, &field_metadata, (items_number * 2), NULL);

  if (!meta_memory) {
    // The only way to say something here is to raise warning
    // b/c we will fallback to other access methods: derived handler or rowstore.
    goto err;
  }

  if((field_metadata_size=
      get_field_types(thd__, table, select, fieldtype, field_metadata,
                      null_bits, num_null_bytes, items_number)) < 0) {
    goto err;
  }

  trx = get_trx(thd__, &error_code);
  if (!trx)
    goto err;

  oids = xpand_extract_table_oids(thd__, select->parent_lex);
  if ((error_code = trx->scan_query(query, fieldtype, items_number, null_bits,
                                    num_null_bytes, field_metadata,
                                    field_metadata_size,
                                    row_buffer_setting(thd), oids, &scan))) {
    goto err;
  }

  // Save this into the base handler class attribute
  table__ = table;

  // need this bitmap future in next_row()
  if (my_bitmap_init(&scan_fields, NULL, table->read_set->n_bits, false))
    return ER_OUTOFMEMORY;
  bitmap_set_all(&scan_fields);

  add_current_table_to_rpl_table_list(&rgi, thd__, table__);

err:
  if (meta_memory)
    my_free(meta_memory);

  if (error_code == HA_ERR_TABLE_DEF_CHANGED)
    xpand_mark_tables_for_discovery(select->parent_lex);

  return error_code;
}

/*@brief  Fetch next row for derived_handler           */
/***********************************************************
 * DESCRIPTION:
 * Fetch next row for derived_handler.
 * PARAMETERS:
 * RETURN:
 *  rc as int
 * ********************************************************/
int ha_xpand_derived_handler::next_row()
{
  int error_code = 0;
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  assert(scan);

  uchar *rowdata;
  ulong rowdata_length;
  if ((error_code = trx->scan_next(scan, &rowdata, &rowdata_length)))
    return error_code;

  uchar const *current_row_end;
  ulong master_reclength;

  error_code = unpack_row(rgi, table, table->s->fields, rowdata,
                          &scan_fields, &current_row_end,
                          &master_reclength, rowdata + rowdata_length);

  if (error_code)
    return error_code;

  return 0;
}

/*@brief  Finishes the scan and clean it up               */
/***********************************************************
 * DESCRIPTION:
 * Finishes the scan for derived handler
 * PARAMETERS:
 * RETURN:
 *   rc as int
 ***********************************************************/
int ha_xpand_derived_handler::end_scan()
{
  return 0;
}

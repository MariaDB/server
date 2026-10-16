/* Copyright (c) 2007, 2013, Oracle and/or its affiliates.
   Copyright (c) 2008, 2022, MariaDB Corporation.

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

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "rpl_rli.h"
#include "rpl_record.h"
#include "slave.h"                  // Need to pull in slave_print_msg
#include "rpl_utility.h"
#include "rpl_rli.h"
/* MDEV-39143: Item_func_json_insert / _remove / _format */
#include "item_jsonfunc.h"
#include "sql_array.h"           // Dynamic_array

/*

 This function is used instead of MY_BITMAP for null bits in the
 binary log image. The reason is that MY_BITMAP functions assumes the
 bits are aligned on 8 byte boundaries and all bits on the last 8 bytes
 are accessible. The above is not guaranteed for the row event null bits.
*/

inline bool rpl_bitmap_is_set(const uchar *null_bits, uint col)
{
  return null_bits[col/8] & (1 << (col % 8));
}

/**
   Pack a record of data for a table into a format suitable for
   transfer via the binary log.

   The format for a row in transfer with N fields is the following:

   ceil(N/8) null bytes:
       One null bit for every column *regardless of whether it can be
       null or not*. This simplifies the decoding. Observe that the
       number of null bits is equal to the number of set bits in the
       @c cols bitmap. The number of null bytes is the smallest number
       of bytes necessary to store the null bits.

       Padding bits are 1.

   N packets:
       Each field is stored in packed format.


   @param table    Table describing the format of the record

   @param cols     Bitmap with a set bit for each column that should
                   be stored in the row

   @param row_data Pointer to memory where row will be written

   @param record   Pointer to record that should be packed. It is
                   assumed that the pointer refers to either @c
                   record[0] or @c record[1], but no such check is
                   made since the code does not rely on that.

   @return The number of bytes written at @c row_data.
 */
#if !defined(MYSQL_CLIENT)
size_t
pack_row(TABLE *table, MY_BITMAP const* cols,
         uchar *row_data, const uchar *record)
{
  Field **p_field= table->field, *field;
  int const null_byte_count= (bitmap_bits_set(cols) + 7) / 8;
  uchar *pack_ptr = row_data + null_byte_count;
  uchar *null_ptr = row_data;
  my_ptrdiff_t const rec_offset= record - table->record[0];
  my_ptrdiff_t const def_offset= table->s->default_values - table->record[0];

  DBUG_ENTER("pack_row");

  /*
    We write the null bits and the packed records using one pass
    through all the fields. The null bytes are written little-endian,
    i.e., the first fields are in the first byte.
   */
  unsigned int null_bits= (1U << 8) - 1;
  // Mask to mask out the correct but among the null bits
  unsigned int null_mask= 1U;
  for ( ; (field= *p_field) ; p_field++)
  {
    if (bitmap_is_set(cols, (uint)(p_field - table->field)))
    {
      my_ptrdiff_t offset;
      if (field->is_null(rec_offset))
      {
        offset= def_offset;
        null_bits |= null_mask;
      }
      else
      {
        offset= rec_offset;
        null_bits &= ~null_mask;

        /*
          We only store the data of the field if it is non-null

          For big-endian machines, we have to make sure that the
          length is stored in little-endian format, since this is the
          format used for the binlog.
        */
#if !defined DBUG_OFF && defined DBUG_TRACE
        const uchar *old_pack_ptr= pack_ptr;
#endif
        pack_ptr= field->pack(pack_ptr, field->ptr + offset);
        DBUG_PRINT("rpl_record",
                   ("field: %s; real_type: %d, pack_ptr: %p;"
                    " pack_ptr':%p; bytes: %d",
                    field->field_name.str, field->real_type(), old_pack_ptr,
                    pack_ptr, (int) (pack_ptr - old_pack_ptr)));
        DBUG_DUMP("packed_data", old_pack_ptr, pack_ptr - old_pack_ptr);
      }

      null_mask <<= 1;
      if ((null_mask & 0xFF) == 0)
      {
        DBUG_ASSERT(null_ptr < row_data + null_byte_count);
        null_mask = 1U;
        *null_ptr++ = null_bits;
        null_bits= (1U << 8) - 1;
      }
    }
  }

  /*
    Write the last (partial) byte, if there is one
  */
  if ((null_mask & 0xFF) > 1)
  {
    DBUG_ASSERT(null_ptr < row_data + null_byte_count);
    *null_ptr++ = null_bits;
  }

  /*
    The null pointer should now point to the first byte of the
    packed data. If it doesn't, something is very wrong.
  */
  DBUG_ASSERT(null_ptr == row_data + null_byte_count);
  DBUG_DUMP("row_data", row_data, pack_ptr - row_data);
  DBUG_RETURN(static_cast<size_t>(pack_ptr - row_data));
}
#endif

#if !defined(MYSQL_CLIENT) && defined(HAVE_REPLICATION)

struct Unpack_record_state
{
  uchar const *const row_data;
  uchar const *const row_end;
  size_t const master_null_byte_count;
  uchar const *null_ptr;
  uchar const *pack_ptr;

  /** Current offset in null_ptr */
  unsigned int null_pos;
  Unpack_record_state(uchar const *const row_data,
                      uchar const *const row_end,
                      size_t const master_null_byte_count)
  : row_data(row_data), row_end(row_end),
    master_null_byte_count(master_null_byte_count),
    null_ptr(row_data), pack_ptr(row_data + master_null_byte_count),
    null_pos(0)
  {}

  /*
    When unpacking a row, if no value was provided for a field (i.e. it is
    NULL), the field needs to be prepared because there may be an existing
    record in the table with data. Nullable fields are reset, and non-null
    fields are set to their default value.
  */
  void prepare_null_field(Field *f)
  {
    /*
      Ensure that the null bit for the current field is set. One is subtracted
      from the null_pos because the null_pos is incremented before calling into
      this function.
    */
    DBUG_ASSERT(this->null_pos);
    DBUG_ASSERT(rpl_bitmap_is_set(this->null_ptr, this->null_pos - 1));

    if (f->maybe_null())
    {
      DBUG_FIX_WRITE_SET(f, f->table->write_set);
      f->reset();
      DBUG_RESTORE_WRITE_SET(f, f->table->write_set);
      f->set_null();
    }
    else
    {
      THD *thd= f->table->in_use;

      f->set_default();
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_BAD_NULL_ERROR, ER_THD(thd, ER_BAD_NULL_ERROR),
                          f->field_name.str);
    }
  };

  /*
    Unpack the value from a packed row into a field. Field must be non-null.

    Returns true if the field was unpacked successfully, false otherwise.
  */
  bool unpack_field(const table_def *tabledef, Field *f, uint master_field_idx)
  {
    DBUG_ASSERT(!f->is_null());
    uint16 const metadata= tabledef->field_metadata(master_field_idx);
#ifdef DBUG_TRACE
    uchar const *const old_pack_ptr= this->pack_ptr;
#endif
    this->pack_ptr= f->unpack(f->ptr, this->pack_ptr, this->row_end, metadata);
    DBUG_PRINT("rpl_record",
               ("field: %s; metadata: 0x%x;"
                " pack_ptr: %p; pack_ptr': %p; bytes: %d",
                f->field_name.str, metadata, old_pack_ptr, this->pack_ptr,
                (int) (this->pack_ptr - old_pack_ptr)));

    return MY_TEST(this->pack_ptr);
  };
};


/*
  While unpacking a row, if the type of the field on the master is different
  from the type on the slave, convert the value to match the type on the slave.
  That is, the value should be initially unpacked into a conversion table
  field, so here we copy and convert the value from the conversion table field
  to the result field.

  @retval false Success.
  @retval true  The conversion failed - currently only possible for a MySQL
               JSON source, where the conversion field's val_str()
               (Field_mysql_json::val_str(), called via Copy_field's
               do_save_blob()) decodes corrupt binary JSON: it pushes an
               error to the THD but still returns an (empty) string, so
               do_copy() "succeeds" and would otherwise store that empty
               value with no indication anything was wrong.  An error
               already on the THD before this call (unrelated to this
               conversion) would also be reported here, but no caller
               reaches this function with one pending.
*/
static bool convert_field(Field *result_field, Field *conv_field)
{
  DBUG_ASSERT(result_field);
  DBUG_ASSERT(conv_field);
#ifndef DBUG_OFF
  char type_buf[MAX_FIELD_WIDTH];
  char value_buf[MAX_FIELD_WIDTH];
  String source_type(type_buf, sizeof(type_buf), system_charset_info);
  String value_string(value_buf, sizeof(value_buf), system_charset_info);
  conv_field->sql_type(source_type);
  conv_field->val_str(&value_string);
  DBUG_PRINT("rpl_record",
             ("Copying field '%s' of type '%s' with value '%s'",
              result_field->field_name.str, source_type.c_ptr_safe(),
              value_string.c_ptr_safe()));
#endif

  Copy_field copy;
  copy.set(result_field, conv_field, TRUE);
  (*copy.do_copy)(&copy);

  if (conv_field->table->in_use->is_error())
    return true;

#ifndef DBUG_OFF
  String target_type(type_buf, sizeof(type_buf), system_charset_info);
  result_field->sql_type(target_type);
  result_field->val_str(&value_string);
  DBUG_PRINT("rpl_record",
             ("Value of field '%s' of type '%s' is now '%s'",
              result_field->field_name.str, target_type.c_ptr_safe(),
              value_string.c_ptr_safe()));
#endif
  return false;
}


/*
  One decoded MySQL JSON diff from a PARTIAL_UPDATE_ROWS_EVENT.  'path' points
  at the JSON path text and 'value' at the diff's new value in MySQL binary
  JSON (JSONB); both point into the row buffer.  'value' is NULL for REMOVE.
*/
enum enum_mysql_json_diff_op
{
  MYSQL_JSON_DIFF_REPLACE= 0,
  MYSQL_JSON_DIFF_INSERT=  1,
  MYSQL_JSON_DIFF_REMOVE=  2
};

struct Mysql_json_diff
{
  enum_mysql_json_diff_op op;
  const char  *path;
  size_t       path_length;
  const uchar *value;                           // NULL for REMOVE
  size_t       value_length;
};


/**
   Decode a MySQL Json_diff_vector, the payload of one partial JSON column in
   the after-image of a PARTIAL_UPDATE_ROWS_EVENT.  The format, specified by
   MySQL WL#2955 section 4, F3, is

     [ length : 4 bytes ] [ diff_1 ] ... [ diff_N ]

   where 'length' counts the bytes of the diffs that follow (not itself), and
   each diff is

     [ op : 1 ] [ path_length : net_field_length ] [ path : path_length ]
     ( [ value_length : net_field_length ] [ value : value_length ] )?

   the value part being present if and only if op is not REMOVE.  The number
   of diffs is not stored; they are read until 'length' bytes are consumed.

   @param[in,out] row_image_pos
                  The caller's read cursor into the row image.  On entry it
                  points at the 4-byte length prefix; on success it is
                  advanced to one past the last diff, i.e. to the next
                  column's data.  On failure it is left untouched, so the
                  caller must not rely on it to locate the error.
   @param[in]     end
                  One past the last readable byte of the row image.  Only an
                  upper bound for validation: the extent of the vector itself
                  comes from its own length prefix, which is checked to fit
                  within [*row_image_pos, end).  Every path and value length
                  is likewise checked against the vector's end, so a corrupt
                  length cannot make this function read outside the buffer.
   @param[out]    out
                  Decoded diffs are *appended*; existing elements are kept, so
                  the caller passes a fresh array unless it deliberately wants
                  to accumulate.  On failure 'out' may already hold the diffs
                  decoded before the bad one - a caller that continues after an
                  error must clear it.

                  The 'path' and 'value' members point *into* the row image
                  rather than at copies, so the decoded diffs are only valid
                  while that buffer lives.  'value' is NULL and 'value_length'
                  is 0 for a REMOVE diff.  Values are still MySQL binary JSON
                  here; decoding them to text is the caller's job.

   @retval false  Success; the whole vector was consumed.
   @retval true   The buffer is truncated or corrupt (a length runs past the
                  end of the vector, or an unknown operation code), or the
                  array could not grow.  Note the MariaDB convention: true
                  means failure.
*/
static bool read_mysql_json_diff_vector(const uchar **row_image_pos,
                                        const uchar *end,
                                        Dynamic_array<Mysql_json_diff> &out)
{
  const uchar *ptr= *row_image_pos;
  if (end - ptr < 4)
    return true;
  size_t length= uint4korr(ptr);
  ptr+= 4;
  if ((size_t) (end - ptr) < length)
    return true;
  const uchar *const diffs_end= ptr + length;

  while (ptr < diffs_end)
  {
    Mysql_json_diff diff;
    diff.op= (enum_mysql_json_diff_op) *ptr++;
    if (diff.op > MYSQL_JSON_DIFF_REMOVE)
      return true;

    size_t avail= (size_t) (diffs_end - ptr);
    diff.path_length= (size_t) safe_net_field_length_ll((uchar**) &ptr, avail);
    /*
      safe_net_field_length_ll() (sql-common/pack.c) sets its cursor to NULL
      on a truncated encoding, rather than leaving it unmoved.  Checking the
      bounds below without first checking for that would compute
      (diffs_end - NULL), a huge bogus value that passes any length check
      and leaves ptr poisoned at NULL for every subsequent read in this
      vector - including the unconditional `*ptr++` at the top of the next
      loop iteration, which would then crash.
    */
    if (!ptr || (size_t) (diffs_end - ptr) < diff.path_length)
      return true;
    diff.path= (const char*) ptr;
    ptr+= diff.path_length;

    if (diff.op == MYSQL_JSON_DIFF_REMOVE)
    {
      diff.value= NULL;
      diff.value_length= 0;
    }
    else
    {
      avail= (size_t) (diffs_end - ptr);
      diff.value_length= (size_t) safe_net_field_length_ll((uchar**) &ptr,
                                                           avail);
      if (!ptr || (size_t) (diffs_end - ptr) < diff.value_length)
        return true;
      diff.value= ptr;
      ptr+= diff.value_length;
    }

    if (out.append(diff))
      return true;                              // out of memory
  }

  *row_image_pos= diffs_end;
  return false;
}


/**
   Decide which MariaDB function an INSERT diff corresponds to.

   MySQL's Json_diff has a single INSERT operation, but its meaning depends
   on what the path points at: sql-common/json_diff.h and WL#2955 section 4,
   F3 define it as JSON_ARRAY_INSERT() when the path names an array cell and
   JSON_INSERT() when it names an object member.  The two differ for a cell
   that already exists - JSON_ARRAY_INSERT shifts the remaining elements
   right, while JSON_INSERT does nothing at all because the path is already
   present - so picking the wrong one corrupts the document silently.

   @param path_obj    Scratch path parser, its steps array already
                      initialised; reused across the diffs of one column.
   @param path        The diff's path text.
   @param path_length Its length in bytes.
   @param[out] is_array_cell
                      True if the path's last step selects an array cell.
   @param[out] is_from_end
                      True if that step counts from the end of the array,
                      i.e. it was written as '[last]' or '[last-N]'.  Such a
                      path is stored with a negative n_item.  The caller
                      refuses these for now; see the note in
                      apply_partial_json_column().

   @retval false  Path parsed; both out-parameters are set.
   @retval true   The path could not be parsed.
*/
static bool json_diff_path_is_array_cell(json_path_t *path_obj,
                                         const char *path,
                                         size_t path_length,
                                         bool *is_array_cell,
                                         bool *is_from_end)
{
  if (json_path_setup(path_obj, &my_charset_utf8mb4_bin,
                      (const uchar *) path,
                      (const uchar *) path + path_length))
    return true;

  const json_path_step_t *last_step=
    ((const json_path_step_t *) path_obj->steps.buffer) +
    path_obj->last_step_idx;
  *is_array_cell= (last_step->type & JSON_PATH_ARRAY) != 0;
  *is_from_end= *is_array_cell && last_step->n_item < 0;
  return false;
}


/**
   Check whether a path resolves to an existing value in a JSON document.

   MySQL's own slave applier (sql/item_json_func.cc, apply_json_diff())
   rejects a diff - and MariaDB's apply_json_diffs() (sql/field.cc) stops
   replication with ER_COULD_NOT_APPLY_JSON_DIFF for it - whenever the
   document does not match what the diff expects: REPLACE and REMOVE
   require the target path to already exist; an INSERT on an object member
   requires it not to (an INSERT on an array cell has no such requirement -
   MySQL only emits one for a cell that does not exist, by construction).
   This uses JSON_CONTAINS_PATH(doc, 'one', path) to answer that question,
   reusing the same Item-based approach the rest of this file already uses.

   @param thd          Used to build and evaluate the Item.
   @param doc          The current document, as text.
   @param path         The diff's path text.
   @param path_length  Its length in bytes.
   @param[out] exists  Set on success: whether the path resolves.

   @retval false  Success; *exists is set.
   @retval true   The check itself failed (OOM, or the path/document could
                  not be evaluated) - distinct from the path simply not
                  existing, which is a normal, successful outcome.
*/
static bool json_diff_path_exists(THD *thd, const String &doc,
                                  const char *path, size_t path_length,
                                  bool *exists)
{
  List<Item> args;
  Item *doc_it= new (thd->mem_root)
    Item_string(thd, doc.ptr(), (uint) doc.length(), doc.charset());
  Item *mode_it= new (thd->mem_root)
    Item_string(thd, STRING_WITH_LEN("one"), &my_charset_utf8mb4_bin);
  Item *path_it= new (thd->mem_root)
    Item_string(thd, path, (uint) path_length, &my_charset_utf8mb4_bin);
  if (!doc_it || !mode_it || !path_it ||
      args.push_back(doc_it, thd->mem_root) ||
      args.push_back(mode_it, thd->mem_root) ||
      args.push_back(path_it, thd->mem_root))
    return true;

  Item *contains= new (thd->mem_root) Item_func_json_contains_path(thd, args);
  if (!contains || contains->fix_fields(thd, &contains))
    return true;

  longlong found= contains->val_int();
  if (contains->null_value)
    return true;
  *exists= found != 0;
  return false;
}


/**
   Report that a partial JSON update could not be applied to a column, and
   stop replication.  Used both for a structurally-corrupt diff vector (no
   path available yet) and for a well-formed diff that is REJECTED - the
   slave's document does not match what the diff expects, which MySQL
   itself treats as a hard error (ER_COULD_NOT_APPLY_JSON_DIFF), not as a
   silent no-op - or that otherwise fails to apply.

   This pushes onto the THD's diagnostics area via my_printf_error(), not
   rgi->rli->report(): do_exec_row()'s caller always re-derives Last_SQL_Error
   from thd->is_error() once the row event returns an error (see
   convert_handler_error() and slave_rows_error_report() in
   log_event_server.cc), and unconditionally overwrites whatever
   rgi->rli->report() had already recorded. Only a message already sitting in
   the diagnostics area when do_exec_row() returns survives that overwrite.

   @param table        The slave table, for the error message.
   @param result_field The column being updated, for the error message.
   @param path         The diff's path, or NULL if not applicable (e.g. a
                       corrupt diff vector, or no base document at all).
   @param path_length  Its length in bytes; ignored if path is NULL.
   @param reason       Human-readable reason, appended to the message.
*/
static void report_pj_apply_error(TABLE *table, Field *result_field,
                                  const char *path, size_t path_length,
                                  const char *reason)
{
  /*
    Some callers report after a failed sub-operation (val_str() on corrupt
    JSONB, a json_diff_path_exists() evaluation failure, fix_fields()) that
    has already pushed its own error onto the diagnostics area. Pushing a
    second one would hit set_error_status()'s "no previous error" assertion
    (sql_error.cc), so leave the existing, already-specific message alone.
  */
  THD *thd= table->in_use;
  if (thd->is_error())
    return;
  char msg[MYSQL_ERRMSG_SIZE];
  if (path)
    my_snprintf(msg, sizeof(msg),
               "Could not apply a partial JSON update to column '%s' "
               "of table '%s.%s' at path '%.*s': %s",
               result_field->field_name.str, table->s->db.str,
               table->s->table_name.str,
               (int) path_length, path, reason);
  else
    my_snprintf(msg, sizeof(msg),
               "Could not apply a partial JSON update to column '%s' "
               "of table '%s.%s': %s",
               result_field->field_name.str, table->s->db.str,
               table->s->table_name.str, reason);
  my_printf_error(ER_UNKNOWN_ERROR, "%s", MYF(0), msg);
}


/**
   Apply one partial-JSON column of a MySQL PARTIAL_UPDATE_ROWS_EVENT
   after-image.

   A partial column carries a Json_diff vector rather than a value, so it
   cannot go through the normal unpack path: there is nothing to unpack into
   the conversion table and copy out with convert_field().  Instead the base
   document is taken from the matched row, the diffs are applied to it in
   order, and the resulting text is stored into the slave field directly.

   The base document is the *before-image*, which lives in record[1] while
   record[0] is being built.  The field is temporarily shifted there to read
   it (move_field_offset()), matching MySQL, which applies diffs to the
   field's current value.

   Each diff is folded through MariaDB's own JSON functions, so the result
   matches what the corresponding SQL function would produce:

     REPLACE -> JSON_REPLACE, INSERT -> JSON_INSERT, REMOVE -> JSON_REMOVE

   applied one at a time, each result feeding the next.  A diff value arrives
   as MySQL binary JSON and is decoded to text by storing it into
   \a conv_field (a Field_mysql_json) and reading it back; the text is then
   wrapped in JSON_COMPACT() so that JSON_INSERT/JSON_REPLACE treat it as a
   JSON value rather than as a quoted string.

   The Items built for this are created on a private THD::free_list which is
   released here, so their internal buffers do not accumulate until the end
   of the statement.

   @param rgi     Relay group info; supplies the THD the Items are built on.
   @param table   The slave table.  Used to locate the before-image, i.e.
                  the distance between record[0] and record[1].
   @param result_field
                  The slave's JSON (or text) field, in record[0].  Receives
                  the document produced by applying the diffs, and is set
                  NOT NULL.
   @param conv_field
                  The conversion-table Field_mysql_json for this column,
                  used purely as a binary-JSON-to-text decoder for the diff
                  values.  Must not be NULL: a partial JSON column always
                  has a conversion field, since the source type is never
                  native to MariaDB.
   @param[in,out] state
                  Unpack state.  Its pack_ptr is advanced past the diff
                  vector that this call consumes, so the caller continues
                  with the next column.  Left unchanged if the vector is
                  corrupt.
   @param row_end One past the last readable byte of the row image, bounding
                  the diff decoding.
   @param only_seek
                  True when there is no matched row to apply to - the
                  caller (Update_rows_log_event::do_exec_row()) is only
                  unpacking the after-image to skip past it in the event
                  stream after find_row() failed, and record[1] does not
                  hold this row's data.  The diff vector is still decoded,
                  to advance state.pack_ptr correctly, but is then discarded
                  without being applied or touching result_field.

   @retval 0                    Success; result_field holds the new document
                                (unless only_seek).
   @retval HA_ERR_CORRUPT_EVENT The diff vector is truncated or malformed.
                                Reported via report_pj_apply_error().
   @retval HA_ERR_GENERIC       There was no base document to apply to, a
                                diff was REJECTED (its path does not match
                                the slave's document - the same condition
                                MySQL's own applier treats as a hard error,
                                not a silent no-op, per
                                sql/item_json_func.cc apply_json_diffs() and
                                sql/field.cc Field_json::unpack_diff()), or
                                building/evaluating a JSON function failed.
                                Reported via report_pj_apply_error().
*/
static int apply_partial_json_column(const rpl_group_info *rgi, TABLE *table,
                                     Field *result_field, Field *conv_field,
                                     Unpack_record_state &state,
                                     const uchar *row_end, bool only_seek)
{
  Dynamic_array<Mysql_json_diff> diffs((PSI_memory_key) PSI_NOT_INSTRUMENTED);
  const uchar *ptr= state.pack_ptr;
  if (read_mysql_json_diff_vector(&ptr, row_end, diffs))
  {
    report_pj_apply_error(table, result_field, NULL, 0,
                          "the diff vector is truncated or corrupt");
    return HA_ERR_CORRUPT_EVENT;
  }
  state.pack_ptr= ptr;

  if (only_seek)
    return 0;

  THD *thd= rgi->thd;
  DBUG_ASSERT(conv_field);

  /* Read the before-image JSON value (found row is in record[1]). */
  my_ptrdiff_t bi_offset= (my_ptrdiff_t) (table->record[1] - table->record[0]);
  result_field->move_field_offset(bi_offset);
  bool base_is_null= result_field->is_null();
  String base_buf;
  String *base= base_is_null ? NULL : result_field->val_str(&base_buf);
  result_field->move_field_offset(-bi_offset);
  if (base_is_null || !base)
  {
    report_pj_apply_error(table, result_field, NULL, 0,
                          "the slave's current value is NULL, so there is "
                          "no document to apply the update to");
    return HA_ERR_GENERIC;
  }

  String cur;
  if (cur.copy(*base))
  {
    report_pj_apply_error(table, result_field, NULL, 0,
                          "out of memory");
    return HA_ERR_GENERIC;
  }

  /*
    Build and evaluate the JSON functions on a private free_list and a
    private MEM_ROOT, then release both here instead of letting them
    accumulate until end of statement.  free_items() alone is not enough:
    it only runs destructors, it does not reclaim the memory the Items and
    their buffers were allocated from, so if they were left on
    thd->mem_root (the statement-level arena, never shrunk mid-statement)
    a multi-row UPDATE touching many JSON rows would grow it for the whole
    statement's duration.  free_root() below reclaims it per row instead.
  */
  Item *const save_free_list= thd->free_list;
  thd->free_list= NULL;
  MEM_ROOT json_apply_root;
  init_alloc_root(PSI_NOT_INSTRUMENTED, &json_apply_root, 1024, 0, MYF(0));
  MEM_ROOT *const save_mem_root= thd->mem_root;
  thd->mem_root= &json_apply_root;
  int rc= 0;

  /* Reused by every INSERT diff of this column to classify its path. */
  json_path_t diff_path;
  mem_root_dynamic_array_init(thd->mem_root, PSI_INSTRUMENT_MEM,
                              &diff_path.steps, sizeof(json_path_step_t),
                              NULL, JSON_DEPTH_DEFAULT, JSON_DEPTH_INC,
                              MYF(0));

  for (size_t k= 0; k < diffs.elements() && !rc; k++)
  {
    const Mysql_json_diff &diff= diffs.at(k);
    List<Item> args;
    Item *doc_it= new (thd->mem_root)
      Item_string(thd, cur.ptr(), (uint) cur.length(), cur.charset());
    Item *path_it= new (thd->mem_root)
      Item_string(thd, diff.path, (uint) diff.path_length,
                  &my_charset_utf8mb4_bin);
    if (!doc_it || !path_it ||
        args.push_back(doc_it, thd->mem_root) ||
        args.push_back(path_it, thd->mem_root))
    {
      report_pj_apply_error(table, result_field, diff.path,
                            diff.path_length, "out of memory");
      rc= HA_ERR_GENERIC; break;
    }

    /*
      A diff is REJECTED - and, per MySQL's own applier
      (sql/item_json_func.cc apply_json_diffs(), sql/field.cc
      Field_json::unpack_diff()), that is a hard error, not a silent
      no-op - when the slave's document does not match what the diff
      expects: REPLACE and REMOVE require the target path to already
      exist; an INSERT on an object member requires it not to.  An INSERT
      on an array cell has no such requirement: MySQL only emits one for a
      cell that does not exist, by construction.
    */
    bool target_exists;
    if (diff.op != MYSQL_JSON_DIFF_INSERT)
    {
      bool exists_check_failed=
        json_diff_path_exists(thd, cur, diff.path, diff.path_length,
                              &target_exists);
      if (exists_check_failed)
      {
        report_pj_apply_error(table, result_field, diff.path,
                              diff.path_length,
                              "could not check whether the path exists "
                              "in the current document");
        rc= HA_ERR_GENERIC; break;
      }
      if (!target_exists)
      {
        report_pj_apply_error(table, result_field, diff.path,
                              diff.path_length,
                              diff.op == MYSQL_JSON_DIFF_REMOVE ?
                              "REJECTED - the path to remove does not "
                              "exist in the slave's document" :
                              "REJECTED - the path to replace does not "
                              "exist in the slave's document");
        rc= HA_ERR_GENERIC; break;
      }
    }

    Item *func;
    if (diff.op == MYSQL_JSON_DIFF_REMOVE)
      func= new (thd->mem_root) Item_func_json_remove(thd, args);
    else
    {
      /*
        Point conv_field directly at the diff's raw JSONB bytes, the same way
        Field_blob::unpack() hands a normal blob column its wire bytes.
        Field::store() must not be used here: Field_mysql_json is built with
        a fixed field_charset() of utf8mb4_bin (mysql_json.cc), so store()
        would run the bytes through well_formed_copy_with_check() and reject
        them the moment a structural byte (e.g. a multi-byte JSONB length,
        needed once a string exceeds 127 bytes) is not valid standalone
        UTF-8.  JSONB is binary data, not text, and must reach val_str()
        unmodified; set_ptr() bypasses charset handling entirely, exactly
        like the ordinary full-document unpack path already does.
      */
      ((Field_blob *) conv_field)->set_ptr(diff.value_length,
                                           const_cast<uchar *>(diff.value));
      String vbuf, *vtext= conv_field->val_str(&vbuf);
      if (!vtext || thd->is_error())
      {
        /*
          Field_mysql_json::val_str() (plugin/type_mysql_json/type.cc) never
          actually returns NULL: on corrupt JSONB it pushes ER_UNKNOWN_ERROR
          to the THD and returns an empty string instead, which would
          otherwise sail through JSON_COMPACT() as a seemingly-valid empty
          value and get committed - so the THD error state, not the return
          value, is what must be checked here.
        */
        report_pj_apply_error(table, result_field, diff.path,
                              diff.path_length,
                              "the diff's value is corrupt or unsupported "
                              "MySQL binary JSON");
        rc= HA_ERR_GENERIC; break;
      }
      /*
        Copy the decoded value onto the statement mem_root (val_str's buffer is
        transient) and wrap it in JSON_COMPACT, so JSON_INSERT/REPLACE inserts
        it as a JSON value rather than a quoted string.
      */
      char *vdup= (char *) thd->memdup(vtext->ptr(), vtext->length());
      Item *v_it= vdup ? new (thd->mem_root)
        Item_string(thd, vdup, (uint) vtext->length(), vtext->charset())
        : NULL;
      Item *val_json= v_it ? new (thd->mem_root)
        Item_func_json_format(thd, v_it, Item_func_json_format::COMPACT)
        : NULL;
      if (!val_json || args.push_back(val_json, thd->mem_root))
      {
        report_pj_apply_error(table, result_field, diff.path,
                              diff.path_length, "out of memory");
        rc= HA_ERR_GENERIC; break;
      }
      if (diff.op == MYSQL_JSON_DIFF_INSERT)
      {
        /*
          An INSERT on an array cell is JSON_ARRAY_INSERT, on an object
          member JSON_INSERT.  See json_diff_path_is_array_cell().
        */
        bool is_array_cell= false, is_from_end= false;
        if (json_diff_path_is_array_cell(&diff_path, diff.path,
                                         diff.path_length, &is_array_cell,
                                         &is_from_end))
        {
          report_pj_apply_error(table, result_field, diff.path,
                                diff.path_length, "unparsable JSON path");
          rc= HA_ERR_GENERIC;
          break;
        }
        if (!is_array_cell)
        {
          bool member_exists;
          if (json_diff_path_exists(thd, cur, diff.path, diff.path_length,
                                    &member_exists))
          {
            report_pj_apply_error(table, result_field, diff.path,
                                  diff.path_length,
                                  "could not check whether the path exists "
                                  "in the current document");
            rc= HA_ERR_GENERIC; break;
          }
          if (member_exists)
          {
            report_pj_apply_error(table, result_field, diff.path,
                                  diff.path_length,
                                  "REJECTED - the object member to insert "
                                  "already exists in the slave's document");
            rc= HA_ERR_GENERIC; break;
          }
        }
        /*
          Refuse rather than diverge silently.  MySQL emits an array-cell
          INSERT only for a cell that does not exist, which for a
          from-the-end index means it clamped the position to 0 and
          prepended.  MariaDB's JSON_ARRAY_INSERT resolves '[last-N]'
          differently - it appends when the index goes negative, and is off
          by one even in range - so applying the diff here would store a
          document that differs from the source with no error raised.  Stop
          the SQL thread instead; a wrong row that replication reports as
          applied is worse than a halt.  Lift this once JSON_ARRAY_INSERT
          resolves such paths the same way the other JSON functions do.
        */
        if (is_array_cell && is_from_end)
        {
          report_pj_apply_error(table, result_field, diff.path,
                                diff.path_length,
                                "cannot apply an INSERT at a from-the-end "
                                "array index - MariaDB's JSON_ARRAY_INSERT "
                                "resolves '[last-N]' differently from "
                                "MySQL, so the result would silently "
                                "differ from the source");
          rc= HA_ERR_GENERIC;
          break;
        }
        func= is_array_cell ?
          (Item *) new (thd->mem_root) Item_func_json_array_insert(thd, args)
          : (Item *) new (thd->mem_root) Item_func_json_insert(true, false,
                                                               thd, args);
      }
      else
        func= new (thd->mem_root) Item_func_json_insert(false, true,
                                                        thd, args);
    }
    if (!func || func->fix_fields(thd, &func))
    {
      report_pj_apply_error(table, result_field, diff.path,
                            diff.path_length,
                            "internal error building the JSON function");
      rc= HA_ERR_GENERIC; break;
    }
    String rbuf, *res= func->val_str(&rbuf);
    if (!res || func->null_value || cur.copy(*res))
    {
      report_pj_apply_error(table, result_field, diff.path,
                            diff.path_length,
                            "the JSON function produced no result");
      rc= HA_ERR_GENERIC; break;
    }
  }

  thd->free_items();
  thd->free_list= save_free_list;
  thd->mem_root= save_mem_root;
  free_root(&json_apply_root, MYF(0));
  if (rc)
    return rc;

  result_field->set_notnull();
  if (result_field->store(cur.ptr(), cur.length(), cur.charset()))
  {
    report_pj_apply_error(table, result_field, NULL, 0,
                          "could not store the resulting document");
    return HA_ERR_GENERIC;
  }
  return 0;
}


/**
   Unpack a row into @c table->record[0].

   The function will always unpack into the @c table->record[0]
   record.  This is because there are too many dependencies on where
   the various member functions of Field and subclasses expect to
   write.

   The row is assumed to only consist of the fields for which the
   corresponding bit in bitset @c cols is set; the other parts of the
   record are left alone.

   At most @c master_cols columns are read: if the table is larger than
   that, the remaining fields are not filled in.

   @note The relay log information can be NULL, which means that no
   checking or comparison with the source table is done, simply
   because it is not used.  This feature is used by MySQL Backup to
   unpack a row from the backup image, but can be used for other
   purposes as well.

   @param rgi     Relay group info
   @param table   Table to unpack into
   @param master_cols  Number of columns to read from record
   @param row_data
                  Packed row datanull_ptr
   @param cols    Pointer to bitset describing columns to fill in
   @param curr_row_end
                  Pointer to variable that will hold the value of the
                  one-after-end position for the current row
   @param row_end
                  Pointer to variable that will hold the value of the
                  end position for the data in the row event
   @param is_partial_json_after_image
                  True when unpacking the after-image of a MySQL
                  PARTIAL_UPDATE_ROWS_EVENT (binlog_row_value_options=
                  PARTIAL_JSON on the source).  Such an image starts with a
                  value_options integer and, if that has PARTIAL_JSON_UPDATES
                  set, a partial_bits bitmap with one bit per JSON column of
                  the source table; a JSON column whose bit is set carries a
                  diff vector instead of a full value.  Must be false for a
                  before-image, which never has that header and never carries
                  diffs, and for every other event type.
   @param only_seek
                  True when there is no matched row to unpack into - the
                  caller is only advancing past this row image in the event
                  stream (e.g. Update_rows_log_event::do_exec_row() after
                  find_row() failed), not applying it.  Only meaningful
                  together with is_partial_json_after_image: a partial JSON
                  column's diff vector is still decoded, to correctly
                  advance past it, but is discarded rather than applied -
                  see apply_partial_json_column().  Ignored otherwise, since
                  ordinary field unpacking never reads record[1], so it is
                  harmless regardless of whether a real row was matched.

   @retval 0 No error

   @retval HA_ERR_GENERIC
   A generic, internal, error caused the unpacking to fail.
   @retval HA_ERR_CORRUPT_EVENT
   Found error when trying to unpack fields.
 */
int unpack_row(const rpl_group_info *rgi, TABLE *table, uint const master_cols,
               uchar const *const row_data, MY_BITMAP const *cols,
               uchar const **const current_row_end,
               uchar const *const row_end,
               bool is_partial_json_after_image,
               bool only_seek)
{
  int error;
  bool null_value;
  DBUG_ENTER("unpack_row");
  DBUG_ASSERT(row_data);
  DBUG_ASSERT(table);
  DBUG_ASSERT(rgi);

  const uchar *rec_ptr= row_data;

  /*
    A MySQL PARTIAL_UPDATE_ROWS_EVENT update after-image is prefixed with a
    value_options integer and, when it has the PARTIAL_JSON_UPDATES bit set, a
    partial_bits bitmap carrying one bit per JSON column in the master table
    (see MySQL sql/log_event.cc).  Read past this header so the null bits and
    column values are unpacked from the correct offset.
  */
  const uchar *pj_partial_bits= NULL;   // set for a PARTIAL_JSON after-image
  uint pj_json_col= 0;             // running index over master JSON columns
  if (is_partial_json_after_image)
  {
    const uint PARTIAL_JSON_UPDATES= 1;
    const table_def *pj_tabledef=
      &((RPL_TABLE_LIST*) table->pos_in_table_list)->m_tabledef;
    size_t avail= (size_t) (row_end - rec_ptr);
    ulonglong value_options=
      safe_net_field_length_ll((uchar**) &rec_ptr, avail);
    /*
      safe_net_field_length_ll() sets its cursor to NULL on a truncated
      encoding (e.g. the event ends right where the after-image header
      should start) instead of leaving it unmoved - see the identical
      hazard documented in read_mysql_json_diff_vector().  MySQL also
      treats any value_options bit beyond PARTIAL_JSON_UPDATES (bit 0) as a
      corrupt event, since none is defined; a value greater than 1 means
      either corruption or a future MySQL version using bits this code does
      not understand, so it must not be read as if bit 0 were the only one.
    */
    if (!rec_ptr || value_options > PARTIAL_JSON_UPDATES)
    {
      /*
        my_printf_error(), not rgi->rli->report(): see the comment on
        report_pj_apply_error() above for why only a message already in the
        diagnostics area when do_exec_row() returns survives to become
        Last_SQL_Error.
      */
      char msg[MYSQL_ERRMSG_SIZE];
      my_snprintf(msg, sizeof(msg),
                 "Could not read the partial JSON update header for "
                 "table '%s.%s'", table->s->db.str, table->s->table_name.str);
      my_printf_error(ER_UNKNOWN_ERROR, "%s", MYF(0), msg);
      DBUG_RETURN(HA_ERR_CORRUPT_EVENT);
    }
    if (value_options & PARTIAL_JSON_UPDATES)
    {
      pj_partial_bits= rec_ptr;
      size_t partial_bits_len= (pj_tabledef->json_column_count() + 7) / 8;
      if (partial_bits_len > (size_t) (row_end - rec_ptr))
      {
        char msg[MYSQL_ERRMSG_SIZE];
        my_snprintf(msg, sizeof(msg),
                   "Could not read the partial JSON update bitmap "
                   "for table '%s.%s'", table->s->db.str,
                   table->s->table_name.str);
        my_printf_error(ER_UNKNOWN_ERROR, "%s", MYF(0), msg);
        DBUG_RETURN(HA_ERR_CORRUPT_EVENT);
      }
      rec_ptr+= partial_bits_len;
    }
  }

  Unpack_record_state st(rec_ptr, row_end, (bitmap_bits_set(cols) + 7) / 8);

  if (bitmap_is_clear_all(cols))
  {
    /**
       There was no data sent from the master, so there is
       nothing to unpack.
     */
    *current_row_end= st.pack_ptr;
    DBUG_RETURN(0);
  }

  Rpl_table_data rpl_data= *(RPL_TABLE_LIST*)table->pos_in_table_list;
  const table_def *tabledef= rpl_data.tabledef;
  const TABLE *conv_table= rpl_data.conv_table;
  uint conv_table_idx= 0;
  DBUG_PRINT("rpl_record", ("Table data: tabldef: %p, conv_table: %p",
                            tabledef, conv_table));

  /*
    When unpacking a row for replication (rather than online alter), it needs
    its own additional checks. The slave must account for its tables having
    either columns in different positions, or with different types, than on the
    master.
  */
  if (!rpl_data.is_online_alter())
  {
    Field *result_field= NULL;
    /*
      Two phases:
        1. First, perform sanity checks to see if the value should actually be
           unpacked, i.e. if nothing was binlogged (NULL) or the column doesn't
           exist on the slave. If there is nothing to unpack, we can just skip
           that column; but the unpack state needs to be maintained (i.e.
           pack_ptr and conv_table_idx need to be incremented appropriately), and
           the field must be configured with the correct default value (or NULL).

        2. Unpack the actual value into the slave field with any necessary
           conversions.
    */
    for (uint master_idx= 0; master_idx < master_cols; master_idx++)
    {
      Field *field=NULL, *conv_field= NULL;
      /*
        A partial JSON after-image carries one partial_bits entry per master
        JSON column, in column order, regardless of image/null.  Advance the
        index for every JSON column and remember whether this one is a diff.
      */
      bool is_partial_col= false;
      if (pj_partial_bits &&
          tabledef->binlog_type(master_idx) == MYSQL_TYPE_JSON_MYSQL)
      {
        is_partial_col= rpl_bitmap_is_set(pj_partial_bits, pj_json_col);
        pj_json_col++;
      }
      /*
        Check 1: Skip unpacking if the field wasn't written in this record.
        This can happen for update row events when the before_image and
        after_image are disjoint (e.g. when binlogged with
        binlog_row_image=MINIMAL).
      */
      if (!bitmap_is_set(cols, master_idx))
      {
        /*
          Check if we need to update the conv_table_idx. A field is only added
          to the conv_table when it exists on the slave.

          conv_table_idx tracks the index of the field in the conversion
          table, but
        */
        if (!(tabledef->master_to_slave_error[master_idx])) // field exists on slave
          conv_table_idx++;
        continue;
      }

      /*
        Check 2: Skip unpacking if the field was written in this record, but
        the slave doesn't have the column. Note though that because the field
        is set on the master, we still have to update the null_pos and
        pack_ptr.
      */
      null_value= rpl_bitmap_is_set(st.null_ptr, st.null_pos++);
      if (tabledef->master_to_slave_error[master_idx])
      {
        /* Column does not exist on slave, skip over it */
        if (!null_value)
          st.pack_ptr+=
              tabledef->calc_field_size(master_idx, (uchar *) st.pack_ptr);
        continue;
      }

      uint slave_idx= tabledef->master_to_slave_map[master_idx];
      DBUG_ASSERT(bitmap_is_set(table->write_set, slave_idx) ||
                  bitmap_is_set(table->read_set, slave_idx));
      result_field= field= table->field[slave_idx];
      result_field->set_has_explicit_value();

      /*
        Check 3: Skip unpacking if NULL is explicitly provided for the field.
        In this case, note that the field must be prepared with the correct
        default value,
      */
      if (null_value)
      {
        st.prepare_null_field(field);
        conv_table_idx++;
        continue;
      }
      result_field->set_notnull();

      /*
        Phase 2: Unpack the actual value into the slave table with any
        necessary conversions.

        If there is a conversion table, we pick up the field pointer to
        the conversion table.  If the conversion table or the field
        pointer is NULL, no conversions are necessary.
      */
      if (conv_table && (conv_field= conv_table->field[conv_table_idx++]))
        field= conv_field;

      DBUG_PRINT("rpl_record", ("Conversion %srequired for field '%s' (#%u)",
                                conv_field ? "" : "not ",
                                result_field->field_name.str, master_idx));
      DBUG_ASSERT(field != NULL);

      if (is_partial_col)
      {
        int pj_err= apply_partial_json_column(rgi, table, result_field,
                                              conv_field, st, row_end,
                                              only_seek);
        if (pj_err)
          DBUG_RETURN(pj_err);
      }
      else
      {
      bool unpack_result= st.unpack_field(tabledef, field, master_idx);
      if (!unpack_result)
      {
        rgi->rli->report(ERROR_LEVEL, ER_SLAVE_CORRUPT_EVENT,
                    rgi->gtid_info(),
                    "Could not read field '%s' of table '%s.%s'",
                    field->field_name.str, table->s->db.str,
                    table->s->table_name.str);
        DBUG_RETURN(HA_ERR_CORRUPT_EVENT);
      }

      /*
        If conv_field is set, then we are doing a conversion. In this
        case, we have unpacked the master data to the conversion
        table, so we need to copy the value stored in the conversion
        table into the final table and do the conversion at the same time.

        If copy_fields is set, it means we are doing an online alter table,
        and will use copy_fields set up in copy_data_between_tables
       */
      if (conv_field && convert_field(result_field, conv_field))
      {
        /*
          convert_field() only returns true once thd->is_error() is already
          true (it checks that itself, after Copy_field has run) - that
          existing, more specific message (e.g. the corrupt-JSONB error from
          Field_mysql_json::val_str()) is what must survive to become
          Last_SQL_Error, so it is left alone rather than pushing a second
          one: do_exec_row()'s caller re-derives Last_SQL_Error from
          thd->is_error() once the row event returns an error (see
          convert_handler_error() in log_event_server.cc), and a second
          my_error() call here would hit set_error_status()'s "no previous
          error" assertion (sql_error.cc).
        */
        DBUG_RETURN(HA_ERR_GENERIC);
      }
      }
    }
  }
  else
  {
    /*
      For Online Alter, iterate through old table fields to unpack,
      then iterate through copy_field array to copy to the new table's record.
     */

    DBUG_ASSERT(master_cols == conv_table->s->fields);
    for (uint master_idx= 0; master_idx < master_cols; master_idx++)
    {
      DBUG_ASSERT(bitmap_is_set(cols, master_idx));
      Field *f= conv_table->field[master_idx];
      if (rpl_bitmap_is_set(st.null_ptr, st.null_pos++))
      {
        st.prepare_null_field(f);
        continue;
      }
      f->set_notnull();
      f->set_has_explicit_value();
      bool result __attribute__((unused))=
          st.unpack_field(tabledef, f, master_idx);
      DBUG_ASSERT(result);
    }

    for (const Copy_field *copy=rpl_data.copy_fields;
         copy != rpl_data.copy_fields_end; copy++)
    {
      copy->to_field->set_has_explicit_value();
      copy->do_copy(copy);
    }
  } // if (rpl_data.is_online_alter())

  /*
    We should now have read all the null bytes, otherwise something is
    really wrong.
   */
  DBUG_ASSERT(((st.null_pos + 7) / 8) == st.master_null_byte_count);
  DBUG_DUMP("row_data", row_data, st.pack_ptr - row_data);

  *current_row_end = st.pack_ptr;

  DBUG_ASSERT(!table->default_field || table->default_field[0]);
  DBUG_ASSERT(!table->vfield || table->vfield[0]);

  if (table->default_field &&
      (rpl_data.is_online_alter() ||
       LOG_EVENT_IS_WRITE_ROW(rgi->current_event->get_type_code())))
  {
    error= table->update_default_fields(table->in_use->lex->ignore);
    if (unlikely(error))
      DBUG_RETURN(error);
  }
  if (table->vfield)
  {
    /*
      TODO MDEV-36892: Data Loss Replicating Persistent Fields if Slave Has
                       Different Function

      If a master provides values for a persisted virtual column, the slave
      overwrites these values using its own function.
    */
    error= table->update_virtual_fields(table->file, VCOL_UPDATE_FOR_WRITE);
    if (unlikely(error))
      DBUG_RETURN(error);
  }

  if (rpl_data.is_online_alter())
  {
    /* we only check constraints for ALTER TABLE */
    DBUG_ASSERT(table->in_use->lex->ignore == FALSE);
    error = table->verify_constraints(false);
    DBUG_ASSERT(error != VIEW_CHECK_SKIP);
    if (error)
      DBUG_RETURN(HA_ERR_GENERIC);
  }

  DBUG_RETURN(0);
}

/**
  Fills @c table->record[0] with default values.

  First @c restore_record() is called to restore the default values for
  record concerning the given table. Then, if @c check is true, 
  a check is performed to see if fields are have default value or can 
  be NULL. Otherwise error is reported.
 
  @param table  Table whose record[0] buffer is prepared. 

  @returns 0 on success
 */ 
int prepare_record(TABLE *const table)
{
  uint col= 0;
  DBUG_ENTER("prepare_record");

  restore_record(table, s->default_values);

  /*
    All fields are used
  */
  if (bitmap_is_set_all(table->write_set))
    DBUG_RETURN(0);

  /*
    For fields on the slave that are not going to be updated from the row image,
    we check if they have a default.
    The check follows the same rules as the INSERT query without specifying an
    explicit value for a field not having the explicit default 
    (@c check_that_all_fields_are_given_values()).
  */
  col= bitmap_get_first_clear(table->write_set);
  for (Field **field_ptr= table->field + col; *field_ptr; field_ptr++, col++)
  {
    if (!bitmap_is_set(table->write_set, col))
      continue;

    Field *const f= *field_ptr;
    DBUG_ASSERT(!((f->flags & NO_DEFAULT_VALUE_FLAG) && f->vcol_info)); // QQ
    if ((f->flags & NO_DEFAULT_VALUE_FLAG) &&
        (f->real_type() != MYSQL_TYPE_ENUM) && !f->vcol_info)
    {
      THD *thd= f->table->in_use;
      f->set_default();
      push_warning_printf(thd,
                          Sql_condition::WARN_LEVEL_WARN,
                          ER_NO_DEFAULT_FOR_FIELD,
                          ER_THD(thd, ER_NO_DEFAULT_FOR_FIELD),
                          f->field_name.str);
    }
  }

  DBUG_RETURN(0);
}
#endif // HAVE_REPLICATION

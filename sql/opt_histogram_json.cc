/*
   Copyright (c) 2021, 2022, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "mariadb.h"
#include "sql_base.h"
#include "my_json_writer.h"
#include "sql_statistics.h"
#include "opt_histogram_json.h"


/*
  @brief
    Un-escape a JSON string and save it into *out.

  @detail
    There's no way to tell how much space is needed for the output.
    Start with a small string and increase its size until json_unescape()
    succeeds.
*/

static bool json_unescape_to_string(const char *val, int val_len, String* out)
{
  // Make sure 'out' has some memory allocated.
  if (!out->alloced_length() && out->alloc(128))
    return true;

  while (1)
  {
    uchar *buf= (uchar*)out->ptr();
    out->length(out->alloced_length());

    int res= json_unescape(&my_charset_utf8mb4_bin,
                           (const uchar*)val,
                           (const uchar*)val + val_len,
                           out->charset(),
                           buf, buf + out->length());
    if (res >= 0)
    {
      out->length(res);
      return false; // Ok
    }

    // We get here if the unescaped string didn't fit into memory.
    if (out->alloc(out->alloced_length()*2))
      return true;
  }
}


/*
  @brief
    Escape a JSON string and save it into *out.

  @detail
    There's no way to tell how much space is needed for the output.
    Start with a small string and increase its size until json_escape()
    succeeds.
*/

static int json_escape_to_string(const String *str, String* out)
{
  // Make sure 'out' has some memory allocated.
  if (!out->alloced_length() && out->alloc(128))
    return JSON_ERROR_OUT_OF_SPACE;

  while (1)
  {
    uchar *buf= (uchar*)out->ptr();
    out->length(out->alloced_length());
    const uchar *str_ptr= (const uchar*)str->ptr();

    int res= json_escape(str->charset(),
                         str_ptr,
                         str_ptr + str->length(),
                         &my_charset_utf8mb4_bin,
                         buf, buf + out->length());
    if (res >= 0)
    {
      out->length(res);
      return 0; // Ok
    }

    if (res != JSON_ERROR_OUT_OF_SPACE)
      return res; // Some conversion error

    // Out of space error. Try with a bigger buffer
    if (out->alloc(out->alloced_length()*2))
      return JSON_ERROR_OUT_OF_SPACE;
  }
}


class Histogram_json_builder : public Histogram_builder
{
  Histogram_json_hb *histogram;
  /* Number of buckets in the histogram */
  uint hist_width;

  /*
    Number of rows that we intend to have in the bucket. That is, this is

      n_rows_in_table / hist_width

    Actual number of rows in the buckets we produce may vary because of
    "popular values" and rounding.
  */
  longlong bucket_capacity;

  /* Number of the buckets already collected */
  uint n_buckets_collected;

  /*
    TRUE means do not try to represent values as UTF-8 text in histogram
    storage. Use start_hex/end_hex for all values.
  */
  bool force_binary;

  /* Data about the bucket we are filling now */
  struct CurBucket
  {
    /* Number of values in the bucket so far. */
    longlong size;

    /* Number of distinct values in the bucket */
    int ndv;
  };
  CurBucket bucket;

  /* Used to create the JSON representation of the histogram. */
  Json_writer writer;

public:

  Histogram_json_builder(Histogram_json_hb *hist, Field *col, uint col_len,
                         ha_rows rows)
    : Histogram_builder(col, col_len, rows), histogram(hist)
  {
    /*
      When computing number of rows in the bucket, round it UP. This way, we
      will not end up with a histogram that has more buckets than intended.

      We may end up producing a histogram with fewer buckets than intended, but
      this is considered tolerable.
    */
    bucket_capacity= (longlong)round(rows2double(records) / histogram->get_width() + 0.5);
    if (bucket_capacity == 0)
      bucket_capacity= 1;
    hist_width= histogram->get_width();
    n_buckets_collected= 0;
    bucket.ndv= 0;
    bucket.size= 0;
    force_binary= (col->type() == MYSQL_TYPE_BIT);

    writer.start_object();
    append_histogram_params();

    writer.add_member(Histogram_json_hb::JSON_NAME).start_array();
  }

  ~Histogram_json_builder() override = default;

private:
  bool bucket_is_empty() { return bucket.ndv == 0; }

  void append_histogram_params()
  {
    char buf[128];
    String str(buf, sizeof(buf), system_charset_info);
    THD *thd= current_thd;
    timeval tv= {thd->query_start(), 0}; // we do not need microseconds

    Timestamp(tv).to_datetime(thd).to_string(&str, 0);
    writer.add_member("target_histogram_size").add_ull(hist_width);
    writer.add_member("collected_at").add_str(str.ptr());
    writer.add_member("collected_by").add_str(server_version);
  }
  /*
    Flush the current bucket out (to JSON output), and set it to be empty.
  */
  void finalize_bucket()
  {
    double fract= (double) bucket.size / records;
    writer.add_member("size").add_double(fract);
    writer.add_member("ndv").add_ll(bucket.ndv);
    writer.end_object();
    n_buckets_collected++;

    bucket.ndv= 0;
    bucket.size= 0;
  }

  /*
    Same as finalize_bucket() but also provide the bucket's end value.
  */
  bool finalize_bucket_with_end_value(void *elem)
  {
    if (append_column_value(elem, false))
      return true;
    finalize_bucket();
    return false;
  }

  /*
    Write the first value group to the bucket.
    @param elem  The value we are writing
    @param cnt   The number of such values.
  */
  bool start_bucket(void *elem, longlong cnt)
  {
    DBUG_ASSERT(bucket.size == 0);
    writer.start_object();
    if (append_column_value(elem, true))
      return true;

    bucket.ndv= 1;
    bucket.size= cnt;
    return false;
  }
  
  /*
    Append the passed value into the JSON writer as string value
  */
  bool append_column_value(void *elem, bool is_start)
  {
    StringBuffer<MAX_FIELD_WIDTH> val;

    // Get the text representation of the value
    column->store_field_value((uchar*) elem, col_length);
    String *str= column->val_str(&val);

    // Escape the value for JSON
    StringBuffer<MAX_FIELD_WIDTH> escaped_val;
    int rc= JSON_ERROR_ILLEGAL_SYMBOL;
    if (!force_binary)
    {
      rc= json_escape_to_string(str, &escaped_val);
      if (!rc)
      {
        writer.add_member(is_start? "start": "end");
        writer.add_str(escaped_val.c_ptr_safe());
        return false;
      }
    }
    if (rc == JSON_ERROR_ILLEGAL_SYMBOL)
    {
      escaped_val.set_hex(val.ptr(), val.length());
      writer.add_member(is_start? "start_hex": "end_hex");
      writer.add_str(escaped_val.c_ptr_safe());
      return false;
    }
    return true;
  }

  /*
    Append a value group of cnt values.
  */
  void append_to_bucket(longlong cnt)
  {
    bucket.ndv++;
    bucket.size += cnt;
  }

public:
  /*
    @brief
      Add data to the histogram.

    @detail
      The call signals to add a "value group" of elem_cnt rows, each of which
      has the same value that is provided in *elem.

      Subsequent next() calls will add values that are greater than the
      current one.

    @return
      0 - OK
  */
  int next(void *elem, element_count elem_cnt) override
  {
    counters.next(elem, elem_cnt);
    ulonglong count= counters.get_count();

    /*
      Ok, we've got a "value group" of elem_cnt identical values.

      If we take the values from the value group and put them into
      the current bucket, how many values will be left after we've
      filled the bucket?
    */
    longlong overflow= bucket.size + elem_cnt - bucket_capacity;

    /*
      Case #1: This value group should be put into a separate bucket, if
       A. It fills the current bucket and also fills the next bucket, OR
       B. It fills the current bucket, which was empty.
    */
    if (overflow >= bucket_capacity || (bucket_is_empty() && overflow >= 0))
    {
      // Finalize the current bucket
      if (!bucket_is_empty())
        finalize_bucket();

      // Start/end the separate bucket for this value group.
      if (start_bucket(elem, elem_cnt))
        return 1; // OOM

      if (records == count)
      {
        if (finalize_bucket_with_end_value(elem))
          return 1;
      }
      else
        finalize_bucket();
    }
    else if (overflow >= 0)
    {
      /*
        Case #2: is when Case#1 doesn't hold, but we can still fill the
        current bucket.
      */

      // If the bucket was empty, it would have been case #1.
      DBUG_ASSERT(!bucket_is_empty());

      /*
        Finalize the current bucket. Put there enough values to make it hold
        bucket_capacity values.
      */
      append_to_bucket(bucket_capacity - bucket.size);
      if (records == count && !overflow)
      {
        if (finalize_bucket_with_end_value(elem))
          return 1;
      }
      else
        finalize_bucket();

      if (overflow > 0)
      {
        // Then, start the new bucket with the remaining values.
        if (start_bucket(elem, overflow))
          return 1;
      }
    }
    else
    {
      // Case #3: there's not enough values to fill the current bucket.
      if (bucket_is_empty())
      {
        if (start_bucket(elem, elem_cnt))
          return 1;
      }
      else
        append_to_bucket(elem_cnt);
    }

    if (records == count)
    {
      // This is the final value group.
      if (!bucket_is_empty())
      {
        if (finalize_bucket_with_end_value(elem))
          return 1;
      }
    }
    return 0;
  }

  /*
    @brief
      Finalize the creation of histogram
  */
  void finalize() override
  {
    writer.end_array();
    writer.end_object();
    Binary_string *json_string= (Binary_string *) writer.output.get_string();
    histogram->set_json_text(n_buckets_collected,
                             json_string->c_ptr(),
                             (size_t)json_string->length());
  }
};


Histogram_builder *Histogram_json_hb::create_builder(Field *col, uint col_len,
                                                     ha_rows rows)
{
  return new Histogram_json_builder(this, col, col_len, rows);
}


void Histogram_json_hb::init_for_collection(MEM_ROOT *mem_root,
                                            Histogram_type htype_arg,
                                            ulonglong size_arg)
{
  DBUG_ASSERT(htype_arg == JSON_HB);
  size= (size_t)size_arg;
}


/*
  A syntax sugar interface to json_string_t
*/
class Json_string
{
  json_string_t str;
public:
  explicit Json_string(const char *name)
  {
    json_string_set_str(&str, (const uchar*)name,
                        (const uchar*)name + strlen(name));
    json_string_set_cs(&str, system_charset_info);
  }
  json_string_t *get() { return &str; }
};


/*
  This [partially] saves the JSON parser state and then can rollback the parser
  to it.

  The goal of this is to be able to make multiple json_key_matches() calls:

    Json_saved_parser_state save(je);
    if (json_key_matches(je, KEY_NAME_1)) {
      ...
      return;
    }
    save.restore_to(je);
    if (json_key_matches(je, KEY_NAME_2)) {
      ...
    }

  This allows one to parse JSON objects where [optional] members come in any
  order.
*/

class Json_saved_parser_state
{
  const uchar *c_str;
  my_wc_t c_next;
  int state;
public:
  explicit Json_saved_parser_state(const json_engine_t *je) :
    c_str(je->s.c_str),
    c_next(je->s.c_next),
    state(je->state)
  {}
  void restore_to(json_engine_t *je)
  {
    je->s.c_str= c_str;
    je->s.c_next= c_next;
    je->state= state;
  }
};


/*
  @brief
    Read a constant from JSON document and save it in *out.

  @detail
    The JSON document stores constant in text form, we need to save it in
    KeyTupleFormat. String constants in JSON may be escaped.
*/

bool read_bucket_endpoint(json_engine_t *je, Field *field, String *out,
                          const char **err)
{
  if (json_read_value(je))
    return true;

  if (je->value_type != JSON_VALUE_STRING &&
      je->value_type != JSON_VALUE_NUMBER)
  {
    *err= "String or number expected";
    return true;
  }

  const char* je_value= (const char*)je->value;
  if (je->value_type == JSON_VALUE_STRING && je->value_escaped)
  {
    StringBuffer<128> unescape_buf;
    if (json_unescape_to_string(je_value, je->value_len, &unescape_buf))
    {
      *err= "Un-escape error";
      return true;
    }
    field->store_text(unescape_buf.ptr(), unescape_buf.length(),
                      unescape_buf.charset());
  }
  else
    field->store_text(je_value, je->value_len, &my_charset_utf8mb4_bin);

  out->alloc(field->pack_length());
  uint bytes= field->get_key_image((uchar*)out->ptr(),
                                   field->key_length(), Field::itRAW);
  out->length(bytes);
  return false;
}


bool read_hex_bucket_endpoint(json_engine_t *je, Field *field, String *out,
                              const char **err)
{
  if (json_read_value(je))
    return true;

  if (je->value_type != JSON_VALUE_STRING || je->value_escaped ||
      (je->value_len & 1))
  {
    *err= "Expected a hex string";
    return true;
  }
  StringBuffer<128> buf;
    
  for (auto pc= je->value; pc < je->value + je->value_len; pc+=2)
  {
    int hex_char1= hexchar_to_int(pc[0]);
    int hex_char2= hexchar_to_int(pc[1]);
    if (hex_char1 == -1 || hex_char2 == -1)
    {
      *err= "Expected a hex string";
      return true;
    }
    buf.append((hex_char1 << 4) | hex_char2);
  }

  field->store_text(buf.ptr(), buf.length(), field->charset());
  out->alloc(field->pack_length());
  uint bytes= field->get_key_image((uchar*)out->ptr(),
                                   field->key_length(), Field::itRAW);
  out->length(bytes);
  return false;
}


/*
  @brief  Parse a JSON reprsentation for one histogram bucket

  @param je     The JSON parser object
  @param field  Table field we are using histogram (used to convert
                               endpoints from text representation to binary)
  @param total_size  INOUT  Fraction of the table rows in the buckets parsed so
                            far.
  @param assigned_last_end  OUT  TRUE<=> The bucket had "end" members, the
                                 function has saved it in
                                 this->last_bucket_end_endp
  @param err  OUT  If function returns 1, this *may* be set to point to text
                   describing the error.

  @detail

    Parse a JSON object in this form:

      { "start": "value", "size":nnn.nn, "ndv": nnn, "end": "value"}

   Unknown members are ignored.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
int Histogram_json_hb::parse_bucket(json_engine_t *je, Field *field,
                                    double *total_size,
                                    bool *assigned_last_end,
                                    const char **err)
{
  *assigned_last_end= false;
  if (json_scan_next(je))
    return 1;
  if (je->state != JST_VALUE)
  {
    if (je->state == JST_ARRAY_END)
      return -1; // EOF
    else
      return 1; // An error
  }

  if (json_scan_next(je) || je->state != JST_OBJ_START)
  {
    *err= "Expected an object in the buckets array";
    return 1;
  }

  bool have_start= false;
  bool have_size= false;
  bool have_ndv= false;

  double size_d;
  longlong ndv_ll= 0;
  StringBuffer<128> value_buf;
  int rc;

  while (!(rc= json_scan_next(je)) && je->state != JST_OBJ_END)
  {
    Json_saved_parser_state save1(je);
    Json_string start_str("start");
    if (json_key_matches(je, start_str.get()))
    {
      if (read_bucket_endpoint(je, field, &value_buf, err))
        return 1;

      have_start= true;
      continue;
    }
    save1.restore_to(je);

    Json_string size_str("size");
    if (json_key_matches(je, size_str.get()))
    {
      if (json_read_value(je))
        return 1;

      const char *size= (const char*)je->value_begin;
      char *size_end= (char*)je->value_end;
      int conv_err;
      size_d= my_strtod(size, &size_end, &conv_err);
      if (conv_err)
      {
        *err= ".size member must be a floating-point value";
        return 1;
      }
      have_size= true;
      continue;
    }
    save1.restore_to(je);

    Json_string ndv_str("ndv");
    if (json_key_matches(je, ndv_str.get()))
    {
      if (json_read_value(je))
        return 1;

      const char *ndv= (const char*)je->value_begin;
      char *ndv_end= (char*)je->value_end;
      int conv_err;
      ndv_ll= my_strtoll10(ndv, &ndv_end, &conv_err);
      if (conv_err)
      {
        *err= ".ndv member must be an integer value";
        return 1;
      }
      have_ndv= true;
      continue;
    }
    save1.restore_to(je);

    Json_string end_str("end");
    if (json_key_matches(je, end_str.get()))
    {
      if (read_bucket_endpoint(je, field, &value_buf, err))
        return 1;
      last_bucket_end_endp.assign(value_buf.ptr(), value_buf.length());
      *assigned_last_end= true;
      continue;
    }
    save1.restore_to(je);

    // Less common endoints:
    Json_string start_hex_str("start_hex");
    if (json_key_matches(je, start_hex_str.get()))
    {
      if (read_hex_bucket_endpoint(je, field, &value_buf, err))
        return 1;

      have_start= true;
      continue;
    }
    save1.restore_to(je);

    Json_string end_hex_str("end_hex");
    if (json_key_matches(je, end_hex_str.get()))
    {
      if (read_hex_bucket_endpoint(je, field, &value_buf, err))
        return 1;
      last_bucket_end_endp.assign(value_buf.ptr(), value_buf.length());
      *assigned_last_end= true;
      continue;
    }
    save1.restore_to(je);


    // Some unknown member. Skip it.
    if (json_skip_key(je))
      return 1;
  }

  if (rc)
    return 1;

  if (!have_start)
  {
    *err= "\"start\" element not present";
    return 1;
  }
  if (!have_size)
  {
    *err= "\"size\" element not present";
    return 1;
  }
  if (!have_ndv)
  {
    *err= "\"ndv\" element not present";
    return 1;
  }

  *total_size += size_d;

  buckets.push_back({std::string(value_buf.ptr(), value_buf.length()),
                     *total_size, ndv_ll});

  return 0; // Ok, continue reading
}


/*
  @brief
    Parse the histogram from its on-disk JSON representation

  @detail
    See opt_histogram_json.h, class Histogram_json_hb for description of the
    data format.

  @return
     false  OK
     True   Error
*/

bool Histogram_json_hb::parse(MEM_ROOT *mem_root, const char *db_name,
                              const char *table_name, Field *field,
                              Histogram_type type_arg,
                              const char *hist_data, size_t hist_data_len)
{
  json_engine_t je;
  int rc;
  const char *err= "JSON parse error";
  double total_size;
  int end_element;
  bool end_assigned;
  DBUG_ENTER("Histogram_json_hb::parse");
  DBUG_ASSERT(type_arg == JSON_HB);

  json_scan_start(&je, &my_charset_utf8mb4_bin,
                  (const uchar*)hist_data,
                  (const uchar*)hist_data+hist_data_len);

  if (json_scan_next(&je))
    goto err;

  if (je.state != JST_OBJ_START)
  {
    err= "Root JSON element must be a JSON object";
    goto err;
  }

  while (1)
  {
    if (json_scan_next(&je))
      goto err;
    if (je.state == JST_OBJ_END)
      break; // End of object

    if (je.state != JST_KEY)
      goto err; // Can' really have this: JSON object has keys in it

    Json_string hist_key_name(JSON_NAME);
    if (json_key_matches(&je, hist_key_name.get()))
    {
      total_size= 0.0;
      end_element= -1;
      if (json_scan_next(&je))
        goto err;

      if (je.state != JST_ARRAY_START)
      {
        err= "histogram_hb must contain an array";
        goto err;
      }

      while (!(rc= parse_bucket(&je, field, &total_size, &end_assigned, &err)))
      {
        if (end_assigned && end_element != -1)
          end_element= (int)buckets.size();
      }
      if (rc > 0)  // Got error other than EOF
        goto err;
    }
    else
    {
      // Some unknown member. Skip it.
      if (json_skip_key(&je))
        return 1;
    }
  }

  if (buckets.size() < 1)
  {
    err= "Histogram must have at least one bucket";
    goto err;
  }

  if (end_element == -1)
  {
    buckets.back().start_value= last_bucket_end_endp;
  }
  else if (end_element < (int)buckets.size())
  {
    err= ".end is only allowed in the last bucket";
    goto err;
  }

  DBUG_RETURN(false); // Ok
err:
  THD *thd= current_thd;
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_JSON_HISTOGRAM_PARSE_FAILED,
                      ER_THD(thd, ER_JSON_HISTOGRAM_PARSE_FAILED),
                      db_name, table_name,
                      err, (je.s.c_str - (const uchar*)hist_data));
  sql_print_error(ER_THD(thd, ER_JSON_HISTOGRAM_PARSE_FAILED),
                  db_name, table_name, err,
                  (je.s.c_str - (const uchar*)hist_data));

  DBUG_RETURN(true);
}


static
void store_key_image_to_rec_no_null(Field *field, const char *ptr, size_t len)
{
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(field->table,
                                    &field->table->write_set);
  field->set_key_image((const uchar*)ptr, (uint)len);
  dbug_tmp_restore_column_map(&field->table->write_set, old_map);
}


static
double position_in_interval(Field *field, const  uchar *key, uint key_len,
                            const std::string& left, const std::string& right)
{
  double res;
  if (field->pos_through_val_str())
  {
    StringBuffer<64> buf1, buf2, buf3;

    store_key_image_to_rec_no_null(field, left.data(), left.size());
    String *min_str= field->val_str(&buf1);
    /*
      Make sure we've saved a copy of the data, not a pointer into the
      field->ptr. We will overwrite the contents of field->ptr with the next
      store_key_image_to_rec_no_null call
    */
    if (&buf1 != min_str)
      buf1.copy(*min_str);
    else
      buf1.copy();

    store_key_image_to_rec_no_null(field, right.data(), right.size());
    String *max_str= field->val_str(&buf2);
    /* Same as above */
    if (&buf2 != max_str)
      buf2.copy(*max_str);
    else
      buf2.copy();

    store_key_image_to_rec_no_null(field, (const char*)key, key_len);
    String *midp_str= field->val_str(&buf3);

    res= pos_in_interval_for_string(field->charset(),
           (const uchar*)midp_str->ptr(), midp_str->length(),
           (const uchar*)buf1.ptr(), buf1.length(),
           (const uchar*)buf2.ptr(), buf2.length());
  }
  else
  {
    store_key_image_to_rec_no_null(field, left.data(), field->key_length());
    double min_val_real= field->val_real();

    store_key_image_to_rec_no_null(field, right.data(), field->key_length());
    double max_val_real= field->val_real();

    store_key_image_to_rec_no_null(field, (const char*)key, field->key_length());
    double midp_val_real= field->val_real();

    res= pos_in_interval_for_double(midp_val_real, min_val_real, max_val_real);
  }
  return res;
}


double Histogram_json_hb::point_selectivity(Field *field, key_range *endpoint,
                                            double avg_sel)
{
  const uchar *key = endpoint->key;
  if (field->real_maybe_null())
    key++;

  // If the value is outside of the histogram's range, this will "clip" it to
  // first or last bucket.
  int endp_cmp;
  int idx= find_bucket(field, key, &endp_cmp);

  double sel;

  if (buckets[idx].ndv == 1 && (endp_cmp!=0))
  {
    /*
      The bucket has a single value and it doesn't match! Return a very
      small value.
    */
    sel= 0.0;
  }
  else
  {
    /*
      We get here when:
      * The bucket has one value and this is the value we are looking for.
      * The bucket has multiple values. Then, assume
    */
    sel= (buckets[idx].cum_fract - get_left_fract(idx)) / buckets[idx].ndv;
  }
  return sel;
}


double Histogram_json_hb::get_left_fract(int idx)
{
  if (!idx)
    return 0.0;
  else
    return buckets[idx-1].cum_fract;
}

std::string& Histogram_json_hb::get_end_value(int idx)
{
  if (idx == (int)buckets.size()-1)
    return last_bucket_end_endp;
  else
    return buckets[idx+1].start_value;
}

/*
  @param field    The table field histogram is for.  We don't care about the
                  field's current value, we only need its virtual functions to
                  perform various operations

  @param min_endp Left endpoint, or NULL if there is none
  @param max_endp Right endpoint, or NULL if there is none
*/

double Histogram_json_hb::range_selectivity(Field *field, key_range *min_endp,
                                            key_range *max_endp, double avg_sel)
{
  double min, max;

  if (min_endp && !(field->real_maybe_null() && min_endp->key[0]))
  {
    bool exclusive_endp= (min_endp->flag == HA_READ_AFTER_KEY)? true: false;
    const uchar *min_key= min_endp->key;
    uint min_key_len= min_endp->length;
    if (field->real_maybe_null())
    {
      min_key++;
      min_key_len--;
    }

    // Find the leftmost bucket that contains the lookup value.
    // (If the lookup value is to the left of all buckets, find bucket #0)
    int endp_cmp;
    int idx= find_bucket(field, min_key, &endp_cmp);

    double sel;
    // Special handling for buckets with ndv=1:
    if (buckets[idx].ndv == 1)
    {
      if (endp_cmp < 0)
        sel= 0.0;
      else if (endp_cmp > 0)
        sel= 1.0;
      else // endp_cmp == 0.0
        sel= (exclusive_endp)? 1.0 : 0.0;
    }
    else
    {
      sel= position_in_interval(field, min_key, min_key_len,
				buckets[idx].start_value,
				get_end_value(idx));
    }
    double left_fract= get_left_fract(idx);
    min= left_fract + sel * (buckets[idx].cum_fract - left_fract);
  }
  else
    min= 0.0;

  if (max_endp)
  {
    // The right endpoint cannot be NULL
    DBUG_ASSERT(!(field->real_maybe_null() && max_endp->key[0]));
    bool inclusive_endp= (max_endp->flag == HA_READ_AFTER_KEY)? true: false;
    const uchar *max_key= max_endp->key;
    uint max_key_len= max_endp->length;
    if (field->real_maybe_null())
    {
      max_key++;
      max_key_len--;
    }
    int endp_cmp;
    int idx= find_bucket(field, max_key, &endp_cmp);

    if ((endp_cmp == 0) && !inclusive_endp)
    {
      /*
        The range is "col < $CONST" and we've found a bucket starting with
        $CONST.
      */
      if (idx > 0)
      {
        // Move to the previous bucket
        endp_cmp= 1;
        idx--;
      }
      else
        endp_cmp= -1;
    }
    double sel;

    // Special handling for buckets with ndv=1:
    if (buckets[idx].ndv == 1)
    {
      if (endp_cmp < 0)
        sel= 0.0;
      else if (endp_cmp > 0)
        sel= 1.0;
      else // endp_cmp == 0.0
        sel= inclusive_endp? 1.0 : 0.0;
    }
    else
    {
      sel= position_in_interval(field, max_key, max_key_len,
                                buckets[idx].start_value,
                                get_end_value(idx));
    }
    double left_fract= get_left_fract(idx);
    max= left_fract + sel * (buckets[idx].cum_fract - left_fract);
  }
  else
    max= 1.0;

  return max - min;
}


void Histogram_json_hb::serialize(Field *field)
{
  field->store(json_text.data(), json_text.size(), &my_charset_bin);
}


#ifndef DBUG_OFF
static int SGN(int x)
{
  if (!x)
    return 0;
  return (x < 0)? -1 : 1;
}
#endif


/*
  @brief
   Find the leftmost histogram bucket such that "lookup_val >= start_value".

  @param field        Field object (used to do value comparisons)
  @param lookup_val   The lookup value in KeyTupleFormat.
  @param cmp  OUT     How the lookup_val compares to found_bucket.left_bound:
                      0  - lookup_val == bucket.left_bound
                      >0 - lookup_val > bucket.left_bound (the most typical)
                      <0 - lookup_val < bucket.left_bound. This can only happen
                      for the first bucket, for all other buckets we would just
                      pick the previous bucket and have cmp>=0.
  @return
     The bucket index
*/

int Histogram_json_hb::find_bucket(const Field *field, const uchar *lookup_val,
                                   int *cmp)
{
  int res;
  int low= 0;
  int high= (int)buckets.size() - 1;
  *cmp= 1; // By default, (bucket[retval].start_value < *lookup_val)

  while (low + 1 < high)
  {
    int middle= (low + high) / 2;
    res= field->key_cmp((uchar*)buckets[middle].start_value.data(), lookup_val);
    if (!res)
    {
      *cmp= res;
      low= middle;
      goto end;
    }
    else if (res < 0)
      low= middle;
    else //res > 0
      high= middle;
  }

  /*
    If low and high were assigned a value in the above loop and we got here,
    then the following holds:

      bucket[low].start_value < lookup_val < bucket[high].start_value

    Besides that, there are two special cases: low=0 and high=last_bucket.
    Handle them below.
  */
  if (low == 0)
  {
    res= field->key_cmp(lookup_val, (uchar*)buckets[0].start_value.data());
    if (res <= 0)
      *cmp= res;
    else // res>0, lookup_val > buckets[0].start_value
    {
      res= field->key_cmp(lookup_val, (uchar*)buckets[high].start_value.data());
      if (res >= 0)  // lookup_val >= buckets[high].start_value
      {
        // Move to that bucket
        low= high;
        *cmp= res;
      }
      else
        *cmp= 1;
    }
  }
  else if (high == (int)buckets.size() - 1)
  {
    res= field->key_cmp(lookup_val, (uchar*)buckets[high].start_value.data());
    if (res >= 0)
    {
      // Ok the value is in the last bucket.
      *cmp= res;
      low= high;
    }
    else
    {
      // The value is in the 'low' bucket.
      res= field->key_cmp(lookup_val, (uchar*)buckets[low].start_value.data());
      *cmp= res;
    }
  }

end:
  // Verification: *cmp has correct value
  DBUG_ASSERT(SGN(*cmp) ==
              SGN(field->key_cmp(lookup_val,
                                 (uchar*)buckets[low].start_value.data())));
  // buckets[low] <= lookup_val, with one exception of the first bucket.
  DBUG_ASSERT(low == 0 ||
              field->key_cmp((uchar*)buckets[low].start_value.data(), lookup_val)<= 0);
  // buckets[low+1] > lookup_val, with one exception of the last bucket
  DBUG_ASSERT(low == (int)buckets.size()-1 ||
              field->key_cmp((uchar*)buckets[low+1].start_value.data(), lookup_val)> 0);
  return low;
}

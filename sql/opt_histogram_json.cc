/*
   Copyright (c) 2021, MariaDB Corporation.

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
  Un-escape a JSON string and save it into *out.
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
  Escape a JSON string and save it into *out.
*/

static bool json_escape_to_string(const String *str, String* out)
{
  // Make sure 'out' has some memory allocated.
  if (!out->alloced_length() && out->alloc(128))
    return true;

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
      return false; // Ok
    }

    if (res != JSON_ERROR_OUT_OF_SPACE)
      return true; // Some conversion error

    // Out of space error. Try with a bigger buffer
    if (out->alloc(out->alloced_length()*2))
      return true;
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

    writer.start_object();
    writer.add_member(Histogram_json_hb::JSON_NAME).start_array();
  }

  ~Histogram_json_builder() override = default;

private:
  bool bucket_is_empty() { return bucket.ndv == 0; }

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
    writer.add_member("end");
    if (append_column_value(elem))
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
    writer.add_member("start");
    if (append_column_value(elem))
      return true;

    bucket.ndv= 1;
    bucket.size= cnt;
    return false;
  }

  /*
    Append the passed value into the JSON writer as string value
  */
  bool append_column_value(void *elem)
  {
    StringBuffer<MAX_FIELD_WIDTH> val;

    // Get the text representation of the value
    column->store_field_value((uchar*) elem, col_length);
    String *str= column->val_str(&val);

    // Escape the value for JSON
    StringBuffer<MAX_FIELD_WIDTH> escaped_val;
    if (json_escape_to_string(str, &escaped_val))
      return true;

    // Note: The Json_writer does NOT do escapes (perhaps this should change?)
    writer.add_str(escaped_val.c_ptr_safe());
    return false;
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
  @brief
    Parse the histogram from its on-disk representation

  @return
     false  OK
     True   Error
*/

bool Histogram_json_hb::parse(MEM_ROOT *mem_root, Field *field,
                              Histogram_type type_arg, const char *hist_data,
                              size_t hist_data_len)
{
  const char *err;
  DBUG_ENTER("Histogram_json_hb::parse");
  DBUG_ASSERT(type_arg == JSON_HB);
  const char *err_pos= hist_data;
  const char *obj1;
  int obj1_len;
  double cumulative_size= 0.0;
  size_t end_member_index= (size_t)-1;
  StringBuffer<128> value_buf;
  StringBuffer<128> unescape_buf;

  if (JSV_OBJECT != json_type(hist_data, hist_data + hist_data_len,
                              &obj1, &obj1_len))
  {
    err= "Root JSON element must be a JSON object";
    err_pos= hist_data;
    goto error;
  }

  const char *hist_array;
  int hist_array_len;
  if (JSV_ARRAY != json_get_object_key(obj1, obj1 + obj1_len,
                                       JSON_NAME, &hist_array,
                                       &hist_array_len))
  {
    err_pos= obj1;
    err= "A JSON array expected";
    goto error;
  }

  for (int i= 0;; i++)
  {
    const char *bucket_info;
    int bucket_info_len;
    enum json_types ret= json_get_array_item(hist_array, hist_array+hist_array_len,
                                             i, &bucket_info,
                                             &bucket_info_len);
    if (ret == JSV_NOTHING)
      break;
    if (ret == JSV_BAD_JSON)
    {
      err_pos= hist_array;
      err= "JSON parse error";
      goto error;
    }
    if (ret != JSV_OBJECT)
    {
      err_pos= hist_array;
      err= "Object expected";
      goto error;
    }

    // Ok, now we are parsing the JSON object describing the bucket
    // Read the "start" field.
    const char *val;
    int val_len;
    ret= json_get_object_key(bucket_info, bucket_info+bucket_info_len,
                             "start", &val, &val_len);
    if (ret != JSV_STRING && ret != JSV_NUMBER)
    {
      err_pos= bucket_info;
      err= ".start member must be present and be a scalar";
      goto error;
    }

    // Read the "size" field.
    const char *size;
    int size_len;
    ret= json_get_object_key(bucket_info, bucket_info+bucket_info_len,
                             "size", &size, &size_len);
    if (ret != JSV_NUMBER)
    {
      err_pos= bucket_info;
      err= ".size member must be present and be a scalar";
      goto error;
    }

    int conv_err;
    char *size_end= (char*)size + size_len;
    double size_d= my_strtod(size, &size_end, &conv_err);
    if (conv_err)
    {
      err_pos= size;
      err= ".size member must be a floating-point value";
      goto error;
    }
    cumulative_size += size_d;

    // Read the "ndv" field
    const char *ndv;
    int ndv_len;
    ret= json_get_object_key(bucket_info, bucket_info+bucket_info_len,
                             "ndv", &ndv, &ndv_len);
    if (ret != JSV_NUMBER)
    {
      err_pos= bucket_info;
      err= ".ndv member must be present and be a scalar";
      goto error;
    }
    char *ndv_end= (char*)ndv + ndv_len;
    longlong ndv_ll= my_strtoll10(ndv, &ndv_end, &conv_err);
    if (conv_err)
    {
      err_pos= ndv;
      err= ".ndv member must be an integer value";
      goto error;
    }

    unescape_buf.set_charset(field->charset());
    uint len_to_copy= field->key_length();
    if (json_unescape_to_string(val, val_len, &unescape_buf))
    {
      err_pos= ndv;
      err= "Out of memory";
      goto error;
    }
    field->store_text(unescape_buf.ptr(), unescape_buf.length(),
                      unescape_buf.charset());
    value_buf.alloc(field->pack_length());
    uint bytes= field->get_key_image((uchar*)value_buf.ptr(), len_to_copy,
                                     Field::itRAW);
    buckets.push_back({std::string(value_buf.ptr(), bytes), cumulative_size,
                       ndv_ll});

    // Read the "end" field
    const char *end_val;
    int end_val_len;
    ret= json_get_object_key(bucket_info, bucket_info+bucket_info_len,
                             "end", &end_val, &end_val_len);
    if (ret != JSV_NOTHING && ret != JSV_STRING && ret !=JSV_NUMBER)
    {
      err_pos= bucket_info;
      err= ".end member must be a scalar";
      goto error;
    }
    if (ret != JSV_NOTHING)
    {
      if (json_unescape_to_string(end_val, end_val_len, &unescape_buf))
      {
        err_pos= bucket_info;
        err= "Out of memory";
        goto error;
      }
      field->store_text(unescape_buf.ptr(), unescape_buf.length(),
                        &my_charset_bin);
      value_buf.alloc(field->pack_length());
      uint bytes= field->get_key_image((uchar*)value_buf.ptr(), len_to_copy,
                                       Field::itRAW);
      last_bucket_end_endp.assign(value_buf.ptr(), bytes);
      if (end_member_index == (size_t)-1)
        end_member_index= buckets.size();
    }
  }
  size= buckets.size();

  if (end_member_index != buckets.size())
  {
    err= ".end must be present in the last bucket and only there";
    err_pos= hist_data;
    goto error;
  }
  if (!buckets.size())
  {
    err= ".end member is allowed only in last bucket";
    err_pos= hist_data;
    goto error;
  }

  DBUG_RETURN(false);
error:
  my_error(ER_JSON_HISTOGRAM_PARSE_FAILED, MYF(0), err, err_pos - hist_data);
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
                                            double avg_sel, double total_rows)
{
  const uchar *key = endpoint->key;
  if (field->real_maybe_null())
    key++;

  // If the value is outside of the histogram's range, this will "clip" it to
  // first or last bucket.
  bool equal;
  int idx= find_bucket(field, key, &equal);

  double sel;

  if (buckets[idx].ndv == 1 && !equal)
  {
    /*
      The bucket has a single value and it doesn't match! Return a very
      small value.
    */
    sel= 1.0 / total_rows;
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
                                            key_range *max_endp)
{
  double min, max;

  if (min_endp && !(field->null_ptr && min_endp->key[0]))
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
    bool equal;
    int idx= find_bucket(field, min_key, &equal);
    if (equal && exclusive_endp && buckets[idx].ndv==1 &&
        idx < (int)buckets.size()-1)
    {
      /*
        The range is "col > $CONST" and we've found a bucket that contains
        only the value $CONST. Move to the next bucket.
        TODO: what if the last value in the histogram is a popular one?
      */
      idx++;
    }
    double left_fract= get_left_fract(idx);
    double sel= position_in_interval(field, min_key, min_key_len,
                                     buckets[idx].start_value,
                                     get_end_value(idx));

    min= left_fract + sel * (buckets[idx].cum_fract - left_fract);
  }
  else
    min= 0.0;

  if (max_endp)
  {
    // The right endpoint cannot be NULL
    DBUG_ASSERT(!(field->null_ptr && max_endp->key[0]));
    bool inclusive_endp= (max_endp->flag == HA_READ_AFTER_KEY)? true: false;
    const uchar *max_key= max_endp->key;
    uint max_key_len= max_endp->length;
    if (field->real_maybe_null())
    {
      max_key++;
      max_key_len--;
    }
    bool equal;
    int idx= find_bucket(field, max_key, &equal);

    if (equal && !inclusive_endp && idx > 0)
    {
      /*
        The range is "col < $CONST" and we've found a bucket starting with
        $CONST. Move to the previous bucket.
        TODO: what if the first value is the popular one?
      */
      idx--;
    }
    double left_fract= get_left_fract(idx);

    double sel;
    /* Special handling for singleton buckets */
    if (buckets[idx].ndv == 1 && equal)
    {
      if (inclusive_endp)
        sel= 1.0;
      else
        sel= 0.0;
    }
    else
    {
      sel= position_in_interval(field, max_key, max_key_len,
                                buckets[idx].start_value,
                                get_end_value(idx));
    }
    max= left_fract + sel * (buckets[idx].cum_fract - left_fract);
  }
  else
    max= 1.0;

  double sel = max - min;
  return sel;
}


void Histogram_json_hb::serialize(Field *field)
{
  field->store(json_text.data(), json_text.size(), &my_charset_bin);
}


/*
  @brief
   Find the leftmost histogram bucket such that "lookup_val >= start_value".

  @param field        Field object (used to do value comparisons)
  @param lookup_val   The lookup value in KeyTupleFormat.
  @param equal  OUT   TRUE<=> the found bucket has left_bound=lookup_val

  @return
     The bucket index
*/

int Histogram_json_hb::find_bucket(const Field *field, const uchar *lookup_val,
                                   bool *equal)
{
  int res;
  int low= 0;
  int high= (int)buckets.size() - 1;
  *equal= false;

  while (low + 1 < high)
  {
    int middle= (low + high) / 2;
    res= field->key_cmp((uchar*)buckets[middle].start_value.data(), lookup_val);
    if (!res)
    {
      *equal= true;
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
    res= field->key_cmp((uchar*)buckets[0].start_value.data(), lookup_val);
    if (!res)
      *equal= true;
    else if (res < 0) //  buckets[0] < lookup_val
    {
      res= field->key_cmp((uchar*)buckets[high].start_value.data(), lookup_val);
      if (!res)
        *equal= true;
      if (res <= 0) // buckets[high] <= lookup_val
        low= high;
    }
  }
  else if (high == (int)buckets.size() - 1)
  {
    res= field->key_cmp((uchar*)buckets[high].start_value.data(), lookup_val);
    if (!res)
      *equal= true;
    if (res <= 0)
      low= high;
  }

end:
  // Verification: *equal==TRUE <=> lookup value is equal to the found bucket.
  DBUG_ASSERT(*equal == !(field->key_cmp((uchar*)buckets[low].start_value.data(),
                                         lookup_val)));
  // buckets[low] <= lookup_val, with one exception of the first bucket.
  DBUG_ASSERT(low == 0 ||
              field->key_cmp((uchar*)buckets[low].start_value.data(), lookup_val)<= 0);
  // buckets[low+1] > lookup_val, with one exception of the last bucket
  DBUG_ASSERT(low == (int)buckets.size()-1 ||
              field->key_cmp((uchar*)buckets[low+1].start_value.data(), lookup_val)> 0);
  return low;
}

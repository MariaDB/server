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

class Histogram_json_builder : public Histogram_builder
{
  Histogram_json_hb *histogram;
  /* Number of buckets in the histogram */
  uint hist_width;

  /*
    Number of rows that we intend to have in the bucket. That is, this is

      n_rows_in_table / histo_width

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
    bucket_capacity= records / histogram->get_width();
    hist_width= histogram->get_width();
    n_buckets_collected= 0;
    bucket.ndv= 0;
    bucket.size= 0;

    writer.start_object();
    writer.add_member(Histogram_json_hb::JSON_NAME).start_array();
  }

  ~Histogram_json_builder() override = default;

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
  void finalize_bucket_with_end_value(void *elem)
  {
    column->store_field_value((uchar*) elem, col_length);
    StringBuffer<MAX_FIELD_WIDTH> val;
    column->val_str(&val);
    writer.add_member("end").add_str(val.c_ptr());
    finalize_bucket();
  }

  /*
    Write the first value group to the bucket.
    @param elem  The value we are writing
    @param cnt   The number of such values.
  */
  void start_bucket(void *elem, element_count cnt)
  {
    DBUG_ASSERT(bucket.size == 0);
    column->store_field_value((uchar*) elem, col_length);
    StringBuffer<MAX_FIELD_WIDTH> val;
    column->val_str(&val);

    writer.start_object();
    writer.add_member("start").add_str(val.c_ptr());

    bucket.ndv= 1;
    bucket.size= cnt;
  }

  /*
    Append a value group of cnt values.
  */
  void append_to_bucket(element_count cnt)
  {
    bucket.ndv++;
    bucket.size += cnt;
  }

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
      start_bucket(elem, elem_cnt);
      if (records == count)
        finalize_bucket_with_end_value(elem);
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
        finalize_bucket_with_end_value(elem);
      else
        finalize_bucket();

      if (overflow > 0)
      {
        // Then, start the new bucket with the remaining values.
        start_bucket(elem, overflow);
      }
    }
    else
    {
      // Case #3: there's not enough values to fill the current bucket.
      if (bucket_is_empty())
        start_bucket(elem, elem_cnt);
      else
        append_to_bucket(elem_cnt);
    }

    if (records == count)
    {
      // This is the final value group.
      if (!bucket_is_empty())
        finalize_bucket_with_end_value(elem);
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
                             (uchar *) json_string->c_ptr());
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
  const char *obj1;
  int obj1_len;
  double cumulative_size= 0.0;

  if (JSV_OBJECT != json_type(hist_data, hist_data + hist_data_len,
                              &obj1, &obj1_len))
  {
    err= "Root JSON element must be a JSON object";
    goto error;
  }

  const char *hist_array;
  int hist_array_len;
  if (JSV_ARRAY != json_get_object_key(obj1, obj1 + obj1_len,
                                       "histogram_hb_v2", &hist_array,
                                       &hist_array_len))
  {
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
      err= "JSON parse error";
      goto error;
    }
    if (ret != JSV_OBJECT)
    {
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
      err= ".size member must be present and be a scalar";
      goto error;
    }

    int conv_err;
    char *size_end= (char*)size + size_len;
    double size_d= my_strtod(size, &size_end, &conv_err);
    if (conv_err)
    {
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
      err= ".ndv member must be present and be a scalar";
      goto error;
    }
    char *ndv_end= (char*)ndv + ndv_len;
    longlong ndv_ll= my_strtoll10(ndv, &ndv_end, &conv_err);
    if (conv_err)
    {
      err= ".ndv member must be an integer value";
      goto error;
    }

    const char *end_val;
    int end_val_len;
    ret= json_get_object_key(bucket_info, bucket_info+bucket_info_len,
                             "end", &end_val, &end_val_len);
    if (ret != JSV_NOTHING && ret != JSV_STRING && ret !=JSV_NUMBER)
    {
      err= ".end member must be a scalar";
      goto error;
    }
    if (ret != JSV_NOTHING)
      last_bucket_end_endp.assign(end_val, end_val_len);

    buckets.push_back({std::string(val, val_len), NULL, cumulative_size,
                       ndv_ll});

    if (buckets.size())
    {
      auto& prev_bucket= buckets[buckets.size()-1];
      if (prev_bucket.ndv == 1)
        prev_bucket.end_value= &prev_bucket.start_value;
      else
        prev_bucket.end_value= &buckets.back().start_value;
    }
  }
  buckets.back().end_value= &last_bucket_end_endp;
  size= buckets.size();

  DBUG_RETURN(false);
error:
  my_error(ER_JSON_HISTOGRAM_PARSE_FAILED, MYF(0), err,
           12345);
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
    String empty_buf1, empty_buf2, empty_buf3;

    store_key_image_to_rec_no_null(field, left.data(), left.size());
    String *min_str= field->val_str(&buf1, &empty_buf1);

    store_key_image_to_rec_no_null(field, right.data(), right.size());
    String *max_str= field->val_str(&buf2, &empty_buf2);

    store_key_image_to_rec_no_null(field, (const char*)key, key_len);
    String *midp_str= field->val_str(&buf3, &empty_buf3);

    res= pos_in_interval_for_string(field->charset(),
           (const uchar*)midp_str->ptr(), midp_str->length(),
           (const uchar*)min_str->ptr(), min_str->length(),
           (const uchar*)max_str->ptr(), max_str->length());
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
  int idx= find_bucket(field, key, false);

  double sel;

  if (buckets[idx].ndv == 1 &&
      field->key_cmp((uchar*)buckets[idx].start_value.data(), key))
  {
    // The bucket has a single value and it doesn't match! Use the global
    // average.
    sel= avg_sel;
  }
  else
  {
    /*
      We get here when:
      * The bucket has one value and this is the value we are looking for.
      * The bucket has multiple values. Then, assume
    */
    sel= (get_left_fract(idx) - buckets[idx].cum_fract) / buckets[idx].ndv;
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
    int idx= find_bucket(field, min_key, exclusive_endp);
    double left_fract= get_left_fract(idx);
    double sel= position_in_interval(field, min_key, min_key_len,
                                     buckets[idx].start_value,
                                     *buckets[idx].end_value);

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

    int idx= find_bucket(field, max_key, inclusive_endp);
    double left_fract= get_left_fract(idx);
    double sel= position_in_interval(field, max_key, max_key_len,
                                     buckets[idx].start_value,
                                     *buckets[idx].end_value);
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
  Find the rightmost histogram bucket such that "lookup_val $GT start_value".

  $GT is either '>' or '>=' depending on equal_is_less parameter.

  @param equal_is_less Controls what to do if a histogram bound is equal to the
                       lookup_val.

  @detail
    Possible cases:
    1. The regular case: the value falls into some bucket.

    2. The value is less than the minimum of the first bucket
    3. The value is greater than the maximum of the last bucket
      In these cases we "clip" to the first/last bucket.

    4. The value hits the bucket boundary. Then, we need to know whether the
       point of interest is to the left the constant, or to the right of it.
*/

int Histogram_json_hb::find_bucket(Field *field, const uchar *lookup_val,
                                   bool equal_is_less)
{
  int low= 0;
  int high= (int)buckets.size() - 1;

  while (low + 1 < high)
  {
    int middle= (low + high) / 2;
    int res= field->key_cmp((uchar*)buckets[middle].start_value.data(), lookup_val);
    if (!res)
      res= equal_is_less? -1: 1;
    if (res < 0)
      low= middle;
    else //res > 0
      high= middle;
  }

  return low;
}

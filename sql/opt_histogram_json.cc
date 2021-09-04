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
  uint hist_width;         /* the number of points in the histogram        */
  double bucket_capacity;  /* number of rows in a bucket of the histogram  */
  uint curr_bucket;        /* number of the current bucket to be built     */

  std::vector<std::string> bucket_bounds;
  bool first_value= true;
public:

  Histogram_json_builder(Histogram_json_hb *hist, Field *col, uint col_len,
                         ha_rows rows)
    : Histogram_builder(col, col_len, rows), histogram(hist)
  {
    bucket_capacity= (double)records / histogram->get_width();
    hist_width= histogram->get_width();
    curr_bucket= 0;
  }

  ~Histogram_json_builder() override = default;

  /*
    @brief
      Add data to the histogram. This call adds elem_cnt rows, each
      of which has value of *elem.

    @detail
      Subsequent next() calls will add values that are greater than *elem.
  */
  int next(void *elem, element_count elem_cnt) override
  {
    counters.next(elem, elem_cnt);
    ulonglong count= counters.get_count();

    if (curr_bucket == hist_width)
      return 0;
    if (first_value)
    {
      first_value= false;
      column->store_field_value((uchar*) elem, col_length);
      StringBuffer<MAX_FIELD_WIDTH> val;
      column->val_str(&val);
      bucket_bounds.push_back(std::string(val.ptr(), val.length()));
    }

    if (count > bucket_capacity * (curr_bucket + 1))
    {
      column->store_field_value((uchar*) elem, col_length);
      StringBuffer<MAX_FIELD_WIDTH> val;
      column->val_str(&val);
      bucket_bounds.emplace_back(val.ptr(), val.length());

      curr_bucket++;
      while (curr_bucket != hist_width &&
             count > bucket_capacity * (curr_bucket + 1))
      {
        bucket_bounds.push_back(std::string(val.ptr(), val.length()));
        curr_bucket++;
      }
    }

    if (records == count && bucket_bounds.size() == hist_width)
    {
      column->store_field_value((uchar*) elem, col_length);
      StringBuffer<MAX_FIELD_WIDTH> val;
      column->val_str(&val);
      bucket_bounds.push_back(std::string(val.ptr(), val.length()));
    }
    return 0;
  }

  /*
    @brief
      Finalize the creation of histogram
  */
  void finalize() override
  {
    Json_writer writer;
    writer.start_object();
    writer.add_member(Histogram_json_hb::JSON_NAME).start_array();

    for(auto& value: bucket_bounds) {
      writer.add_str(value.c_str());
    }
    writer.end_array();
    writer.end_object();
    Binary_string *json_string= (Binary_string *) writer.output.get_string();
    histogram->set_json_text(bucket_bounds.size()-1,
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
  DBUG_ENTER("Histogram_json_hb::parse");
  DBUG_ASSERT(type_arg == JSON_HB);
  const char *err;
  json_engine_t je;
  json_string_t key_name;

  json_scan_start(&je, &my_charset_utf8mb4_bin,
                  (const uchar*)hist_data,
                  (const uchar*)hist_data+hist_data_len);

  if (json_read_value(&je) || je.value_type != JSON_VALUE_OBJECT)
  {
    err= "Root JSON element must be a JSON object";
    goto error;
  }

  json_string_set_str(&key_name, (const uchar*)JSON_NAME,
                      (const uchar*)JSON_NAME + strlen(JSON_NAME));
  json_string_set_cs(&key_name, system_charset_info);

  if (json_scan_next(&je) || je.state != JST_KEY ||
      !json_key_matches(&je, &key_name))
  {
    err= "The first key in the object must be histogram_hb_v1";
    goto error;
  }

  // The value must be a JSON array
  if (json_read_value(&je) || (je.value_type != JSON_VALUE_ARRAY))
  {
    err= "A JSON array expected";
    goto error;
  }

  // Read the array
  while (!json_scan_next(&je))
  {
    switch(je.state)
    {
      case JST_VALUE:
      {
        const char *val;
        int val_len;
        json_smart_read_value(&je, &val, &val_len);
        if (je.value_type != JSON_VALUE_STRING &&
            je.value_type != JSON_VALUE_NUMBER &&
            je.value_type != JSON_VALUE_TRUE &&
            je.value_type != JSON_VALUE_FALSE)
        {
          err= "Scalar value expected";
          goto error;
        }
        uchar buf[MAX_KEY_LENGTH];
        uint len_to_copy= field->key_length();
        field->store_text(val, val_len, &my_charset_bin);
        uint bytes= field->get_key_image(buf, len_to_copy, Field::itRAW);
        histogram_bounds.push_back(std::string((char*)buf, bytes));
        // TODO: Should we also compare this endpoint with the previous
        // to verify that the ordering is right?
        break;
      }
      case JST_ARRAY_END:
        break;
    }
  }
  // n_buckets = n_bounds - 1 :
  size= histogram_bounds.size()-1;
  DBUG_RETURN(false);

error:
  my_error(ER_JSON_HISTOGRAM_PARSE_FAILED, MYF(0), err,
           je.s.c_str - (const uchar*)hist_data);
  DBUG_RETURN(true);
}


static
void store_key_image_to_rec_no_null(Field *field, const uchar *ptr)
{
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(field->table,
                                    &field->table->write_set);
  field->set_key_image(ptr, field->key_length());
  dbug_tmp_restore_column_map(&field->table->write_set, old_map);
}


static
double position_in_interval(Field *field, const  uchar *key,
                            const std::string& left, const std::string& right)
{
  double res;
  if (field->pos_through_val_str())
  {
    uint32 min_len= uint2korr(left.data());
    uint32 max_len= uint2korr(right.data());
    uint32 midp_len= uint2korr(key);

    res= pos_in_interval_for_string(field->charset(),
           key + HA_KEY_BLOB_LENGTH,
           midp_len,
           (const uchar*)left.data() + HA_KEY_BLOB_LENGTH,
           min_len,
           (const uchar*)right.data() + HA_KEY_BLOB_LENGTH,
           max_len);
  }
  else
  {
    store_key_image_to_rec_no_null(field, (const uchar*)left.data());
    double min_val_real= field->val_real();
    
    store_key_image_to_rec_no_null(field, (const uchar*)right.data());
    double max_val_real= field->val_real();

    store_key_image_to_rec_no_null(field, key);
    double midp_val_real= field->val_real();

    res= pos_in_interval_for_double(midp_val_real, min_val_real, max_val_real);
  }
  return res;
}


double Histogram_json_hb::point_selectivity(Field *field, key_range *endpoint,
                                            double avg_sel)
{
  double sel;
  store_key_image_to_rec(field, (uchar *) endpoint->key,
                         field->key_length());
  const uchar *min_key = endpoint->key;
  if (field->real_maybe_null())
    min_key++;
  uint min_idx= find_bucket(field, min_key, false);

  uint max_idx= find_bucket(field, min_key, true);
#if 0
  // find how many buckets this value occupies
  while ((max_idx + 1 < get_width() ) &&
         (field->key_cmp((uchar *)histogram_bounds[max_idx + 1].data(), min_key) == 0)) {
    max_idx++;
  }
#endif
  if (max_idx > min_idx)
  {
    // value spans multiple buckets
    double bucket_sel= 1.0/(get_width() + 1);
    sel= bucket_sel * (max_idx - min_idx + 1);
  }
  else
  {
    // the value fits within a single bucket
    sel = MY_MIN(avg_sel, 1.0/get_width());
  }
  return sel;
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
  double width= 1.0 / histogram_bounds.size();

  if (min_endp && !(field->null_ptr && min_endp->key[0]))
  {
    bool exclusive_endp= (min_endp->flag == HA_READ_AFTER_KEY)? true: false;
    const uchar *min_key= min_endp->key;
    if (field->real_maybe_null())
      min_key++;

    // Find the leftmost bucket that contains the lookup value.
    // (If the lookup value is to the left of all buckets, find bucket #0)
    int idx= find_bucket(field, min_key, exclusive_endp);
    double min_sel= position_in_interval(field, (const uchar*)min_key,
                                         histogram_bounds[idx],
                                         histogram_bounds[idx+1]);
    min= idx*width + min_sel*width;
  }
  else
    min= 0.0;

  if (max_endp)
  {
    // The right endpoint cannot be NULL
    DBUG_ASSERT(!(field->null_ptr && max_endp->key[0]));
    bool inclusive_endp= (max_endp->flag == HA_READ_AFTER_KEY)? true: false;
    const uchar *max_key= max_endp->key;
    if (field->real_maybe_null())
      max_key++;

    int idx= find_bucket(field, max_key, inclusive_endp);
    double max_sel= position_in_interval(field, (const uchar*)max_key,
                                         histogram_bounds[idx],
                                         histogram_bounds[idx+1]);
    max= idx*width + max_sel*width;
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
  Find the histogram bucket that contains the value.

  @param equal_is_less Controls what to do if a histogram bound is equal to the
                       lookup_val.
*/

int Histogram_json_hb::find_bucket(Field *field, const uchar *lookup_val,
                                   bool equal_is_less)
{
  int low= 0;
  int high= histogram_bounds.size() - 1;
  int middle;

  while (low + 1 < high)
  {
    middle= (low + high) / 2;
    int res= field->key_cmp((uchar*)histogram_bounds[middle].data(), lookup_val);
    if (!res)
      res= equal_is_less? -1: 1;
    if (res < 0)
      low= middle;
    else //res > 0
      high= middle;
  }

  return low;
}

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

#include "sql_statistics.h"

/*
  An equi-height histogram which stores real values for bucket bounds.

  Handles @@histogram_type=JSON_HB

  Histogram format in JSON:

  {
    // The next three are saved but not currently analyzed:
    "target_histogram_size": nnn,
    "collected_at": "(date and time)",
    "collected_by": "(server version)",

    "histogram_hb": [
      { "start": "value", "size":nnn.nn, "ndv": nnn },
      ...

      // Optionally, start and/or end can be replaced with _hex variant
      { "start_hex: "value", "size":nnn.nn, "ndv":nnn},

      ...
      { "start": "value", "size":nnn.nn, "ndv": nnn, "end": "value"},
    ]
  }

  Histogram is a JSON object. It has some global properties and "histogram_hb"
  member whose value is a JSON array of histogram buckets.

  Each bucket is an object with these members:
    "start" - the first value in the bucket.
    "size"  - fraction of table rows that is contained in the bucket.
    "ndv"   - Number of Distinct Values in the bucket.
    "end"   - Optionally, the last value in the bucket.

  A bucket is a single-point bucket if it has ndv=1.

  Most buckets have no "end" member: the bucket is assumed to contain all
  values up to the "start" of the next bucket.

  The exception is single-point buckets where last value is the same as the
  first value.

  start/end can be replaced with start_hex/end_hex. In _hex variant, the
  constant is encoded in hex. This encoding is used to handle so called 
  "unassigned characters": some non-UTF8 charsets have byte combinations that
  are not mapped to any UTF8 character.
*/

class Histogram_json_hb : public Histogram_base
{
  size_t size; /* Number of elements in the histogram */

  /* Collection-time only: collected histogram in the JSON form. */
  std::string json_text;

  struct Bucket
  {
    // The left endpoint in KeyTupleFormat. The endpoint is inclusive, this
    // value is in this bucket.
    std::string start_value;

    // Cumulative fraction: The fraction of table rows that fall into this
    //  and preceding buckets.
    double cum_fract;

    // Number of distinct values in the bucket.
    longlong ndv;
  };

  std::vector<Bucket> buckets;

  std::string last_bucket_end_endp;

public:
  static constexpr const char* JSON_NAME="histogram_hb";

  bool parse(MEM_ROOT *mem_root, const char *db_name, const char *table_name,
             Field *field, Histogram_type type_arg,
             const char *hist_data, size_t hist_data_len) override;

  void serialize(Field *field) override;

  Histogram_builder *create_builder(Field *col, uint col_len,
                                    ha_rows rows) override;

  // returns number of buckets in the histogram
  uint get_width() override
  {
    return (uint)size;
  }

  Histogram_type get_type() override
  {
    return JSON_HB;
  }

  /*
    @brief
      This used to be the size of the histogram on disk, which was redundant
      (one can check the size directly). Return the number of buckets instead.
  */
  uint get_size() override
  {
    return (uint)size;
  }

  void init_for_collection(MEM_ROOT *mem_root, Histogram_type htype_arg,
                           ulonglong size) override;

  double point_selectivity(Field *field, key_range *endpoint,
                           double avg_sel) override;
  double range_selectivity(Field *field, key_range *min_endp,
                           key_range *max_endp, double avg_sel) override;

  void set_json_text(ulonglong sz, const char *json_text_arg,
                     size_t json_text_len)
  {
    size= (size_t) sz;
    json_text.assign(json_text_arg, json_text_len);
  }

private:
  int parse_bucket(json_engine_t *je, Field *field, double *cumulative_size,
                   bool *assigned_last_end, const char **err);

  double get_left_fract(int idx);
  std::string& get_end_value(int idx);
  int find_bucket(const Field *field, const uchar *lookup_val, int *cmp);
};


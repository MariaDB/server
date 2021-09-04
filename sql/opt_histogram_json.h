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
*/

class Histogram_json_hb : public Histogram_base
{
  size_t size; /* Number of elements in the histogram */

  /* Collection-time only: collected histogram in the JSON form. */
  std::string json_text;

  // Array of histogram bucket endpoints in KeyTupleFormat.
  std::vector<std::string> histogram_bounds;

public:
  static constexpr const char* JSON_NAME="histogram_hb_v1";

  bool parse(MEM_ROOT *mem_root, Field *field, Histogram_type type_arg,
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
                           double avg_selection) override;
  double range_selectivity(Field *field, key_range *min_endp,
                           key_range *max_endp) override;

  void set_json_text(ulonglong sz, uchar *json_text_arg)
  {
    size= (size_t) sz;
    json_text.assign((const char*)json_text_arg,
                     strlen((const char*)json_text_arg));
  }

private:
  int find_bucket(Field *field, const uchar *lookup_val, bool equal_is_less);
};


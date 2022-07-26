#include <vector>
#include <unordered_set>

#define TABLE_TYPE 510		//Magic number for table.frm files
#define VIEW_TYPE 22868		//Magic number for view.frm files

#define c_malloc(size)                                                        \
  ((char *) my_malloc(PSI_NOT_INSTRUMENTED, (size), MYF(MY_WME)))

struct label
{
  label(){};
  std::vector<LEX_CSTRING> names;
};

struct column
{
  column(){};
  LEX_CSTRING name;
  uint length;
  uint flags;   //field.h
  //enum utype unireg_check;
  uint unireg_check;
  enum enum_field_types type;
  LEX_CSTRING comment;
  uint charset_id;
  uint defaults_offset;
  uint null_byte;
  LEX_CSTRING default_value;
  int label_id;
};

struct key
{
  key(){};
  LEX_CSTRING name;
  LEX_CSTRING comment;
  uint flags;
  uint field_number, length;
  LEX_CSTRING column_name;
  bool is_unique;

  uint parts_count;
  enum ha_key_alg algorithm;
  uint key_block_size;
};

struct frm_file_data
{
  frm_file_data(){};
  uint mysql_version, keyinfo_offset, keyinfo_length;
  uint defaults_offset, defaults_length;
  uint extrainfo_offset, extrainfo_length;
  uint magic_number;
  uint names_length, forminfo_offset;
  uint screens_length;
  uint null_fields, column_count, labels_length, comments_length,
      metadata_offset, metadata_length;
  uint table_charset, min_rows, max_rows, avg_row_length, row_format;
  uint charset_primary_number;
  LEX_CSTRING table_cs_name;
  LEX_CSTRING table_coll_name;
  enum row_type  rtype;
  uint key_block_size, handler_option;
  LEX_CSTRING connect_string;
  LEX_CSTRING engine_name;
  enum legacy_db_type legacy_db_type_1, legacy_db_type_2;
  uint partition_info_str_len;
  char *partition_info_str;

  char *connection;
  uint null_bit;
  column *columns;
  label *labels;

  uint key_count;
  key *keys;
  uint key_parts_count;
  uint key_extra_length;
  uint key_extra_info_offset;
  uint key_comment_offset;
};

#define BYTES_PER_KEY 8
#define BYTES_PER_KEY_PART 9

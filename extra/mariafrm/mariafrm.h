#define TABLE_TYPE 510		//Magic number for table.frm files
#define VIEW_TYPE 22868		//Magic number for view.frm files

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
};
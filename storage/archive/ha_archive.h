/* Copyright (c) 2003, 2011, Oracle and/or its affiliates. All rights reserved.

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

#include <zlib.h>
#include "azlib.h"

/*
  Please read ha_archive.cc first. If you are looking for more general
  answers on how storage engines work, look at ha_example.cc and
  ha_example.h.
*/

typedef struct st_archive_record_buffer {
  uchar *buffer;
  uint32 length;
} archive_record_buffer;


class Archive_share : public Handler_share
{
public:
  mysql_mutex_t mutex;
  THR_LOCK lock;
  azio_stream archive_write;     /* Archive file we are working with */
  ha_rows rows_recorded;    /* Number of rows in tables */
  char table_name[FN_REFLEN];
  char data_file_name[FN_REFLEN];
  bool in_optimize;
  bool archive_write_open;
  bool dirty;               /* Flag for if a flush should occur */
  bool crashed;             /* Meta file is crashed */
  Archive_share();
  virtual ~Archive_share();
  int init_archive_writer();
  void close_archive_writer();
  int write_v1_metafile();
  int read_v1_metafile();
};

/*
  Version for file format.
  1 - Initial Version (Never Released)
  2 - Stream Compression, seperate blobs, no packing
  3 - One stream (row and blobs), with packing
*/
#define ARCHIVE_VERSION 3

class ha_archive final : public handler
{
  THR_LOCK_DATA lock;        /* MySQL lock */
  Archive_share *share;      /* Shared lock info */
  
  azio_stream archive;            /* Archive file we are working with */
  my_off_t current_position;  /* The position of the row we just read */
  uchar byte_buffer[IO_SIZE]; /* Initial buffer for our string */
  String buffer;             /* Buffer used for blob storage */
  ha_rows scan_rows;         /* Number of rows left in scan */
  bool delayed_insert;       /* If the insert is delayed */
  bool bulk_insert;          /* If we are performing a bulk insert */
  const uchar *current_key;
  uint current_key_len;
  uint current_k_offset;
  archive_record_buffer *record_buffer;
  bool archive_reader_open;

  archive_record_buffer *create_record_buffer(unsigned int length);
  void destroy_record_buffer(archive_record_buffer *r);
  int frm_copy(azio_stream *src, azio_stream *dst);
  int frm_compare(azio_stream *src);
  unsigned int pack_row_v1(const uchar *record);

public:
  ha_archive(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_archive() = default;
  const char *index_type(uint inx) override { return "NONE"; }
  ulonglong table_flags() const override
  {
    return (HA_NO_TRANSACTIONS | HA_REC_NOT_IN_SEQ | HA_CAN_BIT_FIELD |
            HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE |
            HA_STATS_RECORDS_IS_EXACT | HA_CAN_EXPORT |
            HA_HAS_RECORDS | HA_CAN_REPAIR | HA_SLOW_RND_POS |
            HA_FILE_BASED | HA_CAN_INSERT_DELAYED | HA_CAN_GEOMETRY);
  }
  ulong index_flags(uint idx, uint part, bool all_parts) const override
  {
    return HA_ONLY_WHOLE_INDEX;
  }
  virtual void get_auto_increment(ulonglong offset, ulonglong increment,
                                  ulonglong nb_desired_values,
                                  ulonglong *first_value,
                                  ulonglong *nb_reserved_values) override;
  uint max_supported_keys() const override { return 1; }
  uint max_supported_key_length() const override { return sizeof(ulonglong); }
  uint max_supported_key_part_length() const override
    { return sizeof(ulonglong); }
  ha_rows records() override { return share->rows_recorded; }
  IO_AND_CPU_COST scan_time() override;
  IO_AND_CPU_COST keyread_time(uint index, ulong ranges, ha_rows rows,
                               ulonglong blocks) override;
  IO_AND_CPU_COST rnd_pos_time(ha_rows rows) override;
  int index_init(uint keynr, bool sorted) override;
  virtual int index_read(uchar * buf, const uchar * key,
			 uint key_len, enum ha_rkey_function find_flag)
                         override;
  virtual int index_read_idx(uchar * buf, uint index, const uchar * key,
			     uint key_len, enum ha_rkey_function find_flag);
  int index_next(uchar * buf) override;
  int open(const char *name, int mode, uint test_if_locked) override;
  int close(void) override;
  int write_row(const uchar * buf) override;
  int real_write_row(const uchar *buf, azio_stream *writer);
  int truncate() override;
  int rnd_init(bool scan=1) override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar * buf, uchar *pos) override;
  int get_row(azio_stream *file_to_read, uchar *buf);
  int get_row_version2(azio_stream *file_to_read, uchar *buf);
  int get_row_version3(azio_stream *file_to_read, uchar *buf);
  Archive_share *get_share(const char *table_name, int *rc);
  int init_archive_reader();
  // Always try auto_repair in case of HA_ERR_CRASHED_ON_USAGE
  bool auto_repair(int error) const override
  { return error == HA_ERR_CRASHED_ON_USAGE; }
  int read_data_header(azio_stream *file_to_read);
  void position(const uchar *record) override;
  int info(uint) override;
  int extra(enum ha_extra_function operation) override;
  void update_create_info(HA_CREATE_INFO *create_info) override;
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info)
    override;
  int optimize(THD* thd, HA_CHECK_OPT* check_opt) override;
  int repair(THD* thd, HA_CHECK_OPT* check_opt) override;
  int check_for_upgrade(HA_CHECK_OPT *check_opt) override;
  void start_bulk_insert(ha_rows rows, uint flags) override;
  int end_bulk_insert() override;
  enum row_type get_row_type() const override
  { 
    return ROW_TYPE_COMPRESSED;
  }
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;
  bool is_crashed() const override;
  int check(THD* thd, HA_CHECK_OPT* check_opt) override;
  bool check_and_repair(THD *thd) override;
  uint32 max_row_length(const uchar *buf);
  bool fix_rec_buff(unsigned int length);
  int unpack_row(azio_stream *file_to_read, uchar *record);
  unsigned int pack_row(const uchar *record, azio_stream *writer);
  bool check_if_incompatible_data(HA_CREATE_INFO *info, uint table_changes)
    override;
  int external_lock(THD *thd, int lock_type) override;
private:
  void flush_and_clear_pending_writes();
};


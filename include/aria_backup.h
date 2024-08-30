/* Copyright (C) 2018,2020 MariaDB Corporation Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/* Interfaces for doing backups of Aria tables */

C_MODE_START

typedef struct st_aria_table_capabilities
{
  my_off_t header_size;
  ulong bitmap_pages_covered;
  uint block_size;
  uint keypage_header;
  enum data_file_type data_file_type;
  my_bool checksum;
  my_bool transactional;
  my_bool encrypted;
  /* This is true if the table can be copied without any locks */
  my_bool online_backup_safe;
  /* s3 capabilities */
  ulong s3_block_size;
  uint8 compression;
} ARIA_TABLE_CAPABILITIES;

int aria_get_capabilities(File kfile, ARIA_TABLE_CAPABILITIES *cap);
int aria_read_index(File kfile, ARIA_TABLE_CAPABILITIES *cap, ulonglong block,
                    uchar *buffer);
int aria_read_data(File dfile, ARIA_TABLE_CAPABILITIES *cap, ulonglong block,
                   uchar *buffer, size_t *bytes_read);
C_MODE_END

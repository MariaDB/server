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

#ifndef ARIA_BACKUP_INCLUDED
#define ARIA_BACKUP_INCLUDED

C_MODE_START

typedef struct st_aria_table_capabilities
{
  my_off_t header_size;
  MARIA_CRYPT_DATA *crypt_data;
  /*
    Buffer used when decrypting a page to check its checksum.
    Allocated by aria_get_capabilities() for encrypted tables.
    A capabilities object must not be used by two threads at the same
    time, as they would share this buffer.
  */
  uchar *crypt_buffer;
  uint crypt_page_header_space;
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
  char filename[FN_REFLEN];
} ARIA_TABLE_CAPABILITIES;

int aria_get_capabilities(File kfile, const char *table_name, ARIA_TABLE_CAPABILITIES *cap);
void aria_free_capabilities(ARIA_TABLE_CAPABILITIES *cap);
int aria_read_index(File kfile, ARIA_TABLE_CAPABILITIES *cap, ulonglong block,
                    uchar *buffer, size_t buffer_size, size_t *bytes_read);
int aria_read_data(File dfile, ARIA_TABLE_CAPABILITIES *cap, ulonglong block,
                   uchar *buffer, size_t buffer_size, size_t *bytes_read);

/*
  Simple interface for copying an Aria table.

  The context stores the files and the position in them, which means
  that the caller only has to call aria_read_index_file() and
  aria_read_data_file() until they return 0 (end of file).

  The server opens the files with aria_open_files_for_backup(), see
  aria_server_backup.h. Other users of this interface, like
  mariadb-backup, fill in the context themselves.
*/

typedef struct st_aria_backup_context
{
  File kfile, dfile;
  ulonglong kblock;                             /* Position in key file */
  ulonglong dblock;                             /* Position in data file */
  ARIA_TABLE_CAPABILITIES capabilities;
} ARIA_BACKUP_CONTEXT;

/*
  Read the next part of the index or the data file.
  buff_length must be a multiple of capabilities.block_size.

  @return number of bytes read
  @retval 0             end of file
  @retval < 0           error. The error has been printed.
*/

longlong aria_read_index_file(ARIA_BACKUP_CONTEXT *context,
                              uchar *buffer, size_t buff_length);
longlong aria_read_data_file(ARIA_BACKUP_CONTEXT *context,
                             uchar *buffer, size_t buff_length);

C_MODE_END
#endif /* ARIA_BACKUP_INCLUDED */

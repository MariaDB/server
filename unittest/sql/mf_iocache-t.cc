/* Copyright (C) 2015 MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include <my_global.h>
#include <my_sys.h>
#include <my_crypt.h>
#include <tap.h>

/*** tweaks and stubs for encryption code to compile ***************/
#define KEY_SIZE (128/8)

my_bool encrypt_tmp_files;
int init_io_cache_encryption();

uint encryption_key_get_latest_version_func(uint)
{
  return 1;
}

uint encryption_key_id_exists_func(uint)
{
  return 1;
}

uint encryption_key_version_exists_func(uint, uint)
{
  return 1;
}

uint encryption_key_get_func(uint, uint, uchar* key, uint* size)
{
  if (*size < KEY_SIZE)
  {
    *size= KEY_SIZE;
    return ENCRYPTION_KEY_BUFFER_TOO_SMALL;
  }
  memset(key, KEY_SIZE, *size= KEY_SIZE);
  return 0;
}

#ifdef HAVE_EncryptAes128Gcm
enum my_aes_mode aes_mode= MY_AES_GCM;
#else
enum my_aes_mode aes_mode= MY_AES_CBC;
#endif

int encryption_ctx_init_func(void *ctx, const unsigned char* key, unsigned int klen,
                                const unsigned char* iv, unsigned int ivlen,
                                int flags, unsigned int key_id,
                                unsigned int key_version)
{
  return my_aes_crypt_init(ctx, aes_mode, flags, key, klen, iv, ivlen);
}

uint encryption_encrypted_length_func(unsigned int slen, unsigned int key_id, unsigned int key_version)
{
  return my_aes_get_size(aes_mode, slen);
}

struct encryption_service_st encryption_handler=
{
  encryption_key_get_latest_version_func,
  encryption_key_get_func,
  (uint (*)(unsigned int, unsigned int))my_aes_ctx_size,
  encryption_ctx_init_func,
  my_aes_crypt_update,
  my_aes_crypt_finish,
  encryption_encrypted_length_func
};

void sql_print_information(const char *format, ...)
{
}

void sql_print_error(const char *format, ...)
{
}

/*** end of encryption tweaks and stubs ****************************/

IO_CACHE info;
#define CACHE_SIZE 16384

#define INFO_TAIL ", pos_in_file = %llu, pos_in_mem = %lu", \
    info.pos_in_file, (ulong) ((info.type == READ_CACHE ? info.read_pos : info.write_pos) - info.request_pos)

#define FILL 0x5A

int data_bad(const uchar *buf, size_t len)
{
  const uchar *end= buf + len;
  while (buf < end)
    if (*buf++ != FILL)
      return 1;
  return 0;
}

void temp_io_cache()
{
  int res;
  uchar buf[CACHE_SIZE + 200];
  memset(buf, FILL, sizeof(buf));

  diag("temp io_cache with%s encryption", encrypt_tmp_files?"":"out");

  init_io_cache_encryption();  
  
  res= open_cached_file(&info, 0, 0, CACHE_SIZE, 0);
  ok(res == 0, "open_cached_file" INFO_TAIL);

  res= my_b_write(&info, buf, 100);
  ok(res == 0 && info.pos_in_file == 0, "small write" INFO_TAIL );

  res= my_b_write(&info, buf, sizeof(buf));
  ok(res == 0 && info.pos_in_file == CACHE_SIZE, "large write" INFO_TAIL);

  res= reinit_io_cache(&info, WRITE_CACHE, 250, 0, 0);
  ok(res == 0, "reinit with rewind" INFO_TAIL);

  res= my_b_write(&info, buf, sizeof(buf));
  ok(res == 0, "large write" INFO_TAIL);

  res= my_b_flush_io_cache(&info, 1);
  ok(res == 0, "flush" INFO_TAIL);

  res= reinit_io_cache(&info, READ_CACHE, 0, 0, 0);
  ok(res == 0, "reinit READ_CACHE" INFO_TAIL);

  res= (int)my_pread(info.file, buf, 50, 50, MYF(MY_NABP));
  ok(res == 0 && data_bad(buf, 50) == encrypt_tmp_files,
     "file must be %sreadable", encrypt_tmp_files ?"un":"");

  res= my_b_read(&info, buf, 50) || data_bad(buf, 50);
  ok(res == 0 && info.pos_in_file == 0, "small read" INFO_TAIL);

  res= my_b_read(&info, buf, sizeof(buf)) || data_bad(buf, sizeof(buf));
  ok(res == 0 && info.pos_in_file == CACHE_SIZE, "large read" INFO_TAIL);

  close_cached_file(&info);
}

void mdev9044()
{
  int res;
  uchar buf[CACHE_SIZE + 200];

  diag("MDEV-9044 Binlog corruption in Galera");

  res= open_cached_file(&info, 0, 0, CACHE_SIZE, 0);
  ok(res == 0, "open_cached_file" INFO_TAIL);

  res= my_b_write(&info, USTRING_WITH_LEN("first write\0"));
  ok(res == 0, "first write" INFO_TAIL);

  res= my_b_flush_io_cache(&info, 1);
  ok(res == 0, "flush" INFO_TAIL);

  res= reinit_io_cache(&info, WRITE_CACHE, 0, 0, 0);
  ok(res == 0, "reinit WRITE_CACHE" INFO_TAIL);

  res= my_b_write(&info, USTRING_WITH_LEN("second write\0"));
  ok(res == 0, "second write" INFO_TAIL );

  res= reinit_io_cache(&info, READ_CACHE, 0, 0, 0);
  ok(res == 0, "reinit READ_CACHE" INFO_TAIL);

  res= (int)my_b_fill(&info);
  ok(res == 0, "fill" INFO_TAIL);

  res= reinit_io_cache(&info, READ_CACHE, 0, 0, 0);
  ok(res == 0, "reinit READ_CACHE" INFO_TAIL);

  res= my_b_read(&info, buf, sizeof(buf));
  ok(res == 1 && strcmp((char*)buf, "second write") == 0, "read '%s'", buf);

  close_cached_file(&info);
}

/* 2 Reads (with my_b_fill) in cache makes second read to fail */
void mdev10259()
{
  int res;
  uchar buf[200];
  memset(buf, FILL, sizeof(buf));

  diag("MDEV-10259- mysqld crash with certain statement length and order with"
         " Galera and encrypt-tmp-files=1");

  init_io_cache_encryption();

  res= open_cached_file(&info, 0, 0, CACHE_SIZE, 0);
  ok(res == 0, "open_cached_file" INFO_TAIL);

  res= my_b_write(&info, buf, sizeof(buf));
  ok(res == 0 && info.pos_in_file == 0, "200 write" INFO_TAIL);

  res= my_b_flush_io_cache(&info, 1);
  ok(res == 0, "flush" INFO_TAIL);

  my_off_t saved_pos=  my_b_tell(&info);
  res= reinit_io_cache(&info, READ_CACHE, 0, 0, 0);
  ok(res == 0, "reinit READ_CACHE" INFO_TAIL);

  size_t s= my_b_fill(&info);
  ok(s == 200, "fill" INFO_TAIL);

  s= my_b_fill(&info);
  ok(s == 0, "fill" INFO_TAIL);

  s= my_b_fill(&info);
  ok(s == 0, "fill" INFO_TAIL);

  res= reinit_io_cache(&info, WRITE_CACHE, saved_pos, 0, 0);
  ok(res == 0, "reinit WRITE_CACHE" INFO_TAIL);

  res= reinit_io_cache(&info, READ_CACHE, 0, 0, 0);
  ok(res == 0, "reinit READ_CACHE" INFO_TAIL);

  ok(200 == my_b_bytes_in_cache(&info),"my_b_bytes_in_cache == 200");

  s= my_b_fill(&info);
  ok(s == 0, "fill" INFO_TAIL);

  s= my_b_fill(&info);
  ok(s == 0, "fill" INFO_TAIL);

  s= my_b_fill(&info);
  ok(s == 0, "fill" INFO_TAIL);

  res= reinit_io_cache(&info, WRITE_CACHE, saved_pos, 0, 0);
  ok(res == 0, "reinit WRITE_CACHE" INFO_TAIL);

  res= reinit_io_cache(&info, READ_CACHE, 0, 0, 0);
  ok(res == 0, "reinit READ_CACHE" INFO_TAIL);

  ok(200 == my_b_bytes_in_cache(&info),"my_b_bytes_in_cache == 200");

  res= my_b_read(&info, buf, sizeof(buf)) || data_bad(buf, sizeof(buf));
  ok(res == 0 && info.pos_in_file == 0, "large read" INFO_TAIL);

  close_cached_file(&info);

}

void mdev14014()
{
  int res;
  uchar buf_o[200];
  uchar buf_i[200];
  memset(buf_i,    0, sizeof( buf_i));
  memset(buf_o, FILL, sizeof(buf_o));

  diag("MDEV-14014 Dump thread reads past last 'officially' written byte");

  init_io_cache_encryption();

  res= open_cached_file(&info, 0, 0, CACHE_SIZE, 0);
  ok(res == 0, "open_cached_file" INFO_TAIL);

  res= my_b_write(&info, buf_o, sizeof(buf_o));
  ok(res == 0, "buffer is written" INFO_TAIL);

  res= my_b_flush_io_cache(&info, 1);
  ok(res == 0, "flush" INFO_TAIL);

  res= reinit_io_cache(&info, READ_CACHE, 0, 0, 0);
  ok(res == 0, "reinit READ_CACHE" INFO_TAIL);

  info.end_of_file= 100;
  res= my_b_read(&info, buf_i, sizeof(buf_i));
  ok(res == 1 && buf_i[100] == 0 && buf_i[200-1] == 0,
     "short read leaves buf_i[100..200-1] == 0");

  close_cached_file(&info);
}


int main(int argc __attribute__((unused)),char *argv[])
{
  MY_INIT(argv[0]);
  plan(51);

  /* temp files with and without encryption */
  encrypt_tmp_files= 1;
  temp_io_cache();

  encrypt_tmp_files= 0;
  temp_io_cache();

  /* regression tests */
  mdev9044();

  encrypt_tmp_files= 1;
  mdev10259();
  encrypt_tmp_files= 0;

  mdev14014();

  my_end(0);
  return exit_status();
}


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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <my_sys.h>
#include <my_crypt.h>
#include <tap.h>

/*** tweaks and stubs for encryption code to compile ***************/
#define KEY_SIZE (128/8)

my_bool encrypt_tmp_files;
void init_io_cache_encryption();

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

struct encryption_service_st encryption_handler=
{
  encryption_key_get_latest_version_func,
  encryption_key_id_exists_func,
  encryption_key_version_exists_func,
  encryption_key_get_func,
#ifdef HAVE_EncryptAes128Gcm
  (encrypt_decrypt_func)my_aes_encrypt_gcm,
  (encrypt_decrypt_func)my_aes_decrypt_gcm
#else
  (encrypt_decrypt_func)my_aes_encrypt_cbc,
  (encrypt_decrypt_func)my_aes_decrypt_cbc
#endif
};

void sql_print_information(const char *format, ...) 
{
}

/*** end of encryption tweaks and stubs ****************************/

IO_CACHE info;
#define CACHE_SIZE 16384

#define INFO_TAIL ", pos_in_file = %llu, pos_in_mem = %lu", \
                info.pos_in_file, (*info.current_pos - info.request_pos)

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

  res= my_pread(info.file, buf, 50, 50, MYF(MY_NABP));
  ok(res == 0 && data_bad(buf, 50) == encrypt_tmp_files,
     "check encryption, file must be %sreadable", encrypt_tmp_files ?"un":"");

  res= my_b_read(&info, buf, 50) || data_bad(buf, 50);
  ok(res == 0 && info.pos_in_file == 0, "small read" INFO_TAIL);

  res= my_b_read(&info, buf, sizeof(buf)) || data_bad(buf, sizeof(buf));
  ok(res == 0 && info.pos_in_file == CACHE_SIZE, "large read" INFO_TAIL);

  close_cached_file(&info);
}

int main(int argc __attribute__((unused)),char *argv[])
{
  MY_INIT(argv[0]);
  plan(20);

  /* temp files */
  encrypt_tmp_files= 0;
  temp_io_cache();

  encrypt_tmp_files= 1;
  temp_io_cache();

  my_end(0);
  return exit_status();
}


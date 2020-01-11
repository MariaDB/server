/*
  Copyright (c) 2014 Google Inc.
  Copyright (c) 2014, 2015 MariaDB Corporation

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

/**
  Example key management plugin. It demonstrates how to return
  keys on request, how to change them. That the engine can have
  different pages in the same tablespace encrypted with different keys
  and what the background re-encryption thread does.

  THIS IS AN EXAMPLE ONLY! ENCRYPTION KEYS ARE HARD-CODED AND *NOT* SECRET!
  DO NOT USE THIS PLUGIN IN PRODUCTION! EVER!
*/

#include <my_global.h>
#include <my_pthread.h>
#include <mysql/plugin_encryption.h>
#include <my_crypt.h>

/* rotate key randomly between 45 and 90 seconds */
#define KEY_ROTATION_MIN 45
#define KEY_ROTATION_MAX 90

static time_t key_version = 0;
static time_t next_key_version = 0;
static pthread_mutex_t mutex;


/* Random double value in 0..1 range */
static double double_rnd()
{
  return ((double)rand()) / RAND_MAX;
}


static unsigned int
get_latest_key_version(unsigned int key_id)
{
  time_t now = time(0);
  pthread_mutex_lock(&mutex);
  if (now >= next_key_version)
  {
    key_version = now;
    unsigned int interval = KEY_ROTATION_MAX - KEY_ROTATION_MIN;
    next_key_version = (time_t) (now + KEY_ROTATION_MIN +
                                 double_rnd() * interval);
  }
  pthread_mutex_unlock(&mutex);

  return (unsigned int) key_version;
}

static unsigned int
get_key(unsigned int key_id, unsigned int version,
        unsigned char* dstbuf, unsigned *buflen)
{
  if (*buflen < MY_MD5_HASH_SIZE)
  {
    *buflen= MY_MD5_HASH_SIZE;
    return ENCRYPTION_KEY_BUFFER_TOO_SMALL;
  }
  *buflen= MY_MD5_HASH_SIZE;
  if (!dstbuf)
    return 0;

  my_md5_multi(dstbuf, (const char*)&key_id, sizeof(key_id),
                       (const char*)&version, sizeof(version), NULL);

  return 0;
}

/*
  for the sake of an example, let's use different encryption algorithms/modes
  for different keys versions:
*/
static inline enum my_aes_mode mode(unsigned int key_version)
{
  return key_version & 1 ? MY_AES_ECB : MY_AES_CBC;
}

int ctx_init(void *ctx, const unsigned char* key, unsigned int klen, const
             unsigned char* iv, unsigned int ivlen, int flags, unsigned int
             key_id, unsigned int key_version)
{
  return my_aes_crypt_init(ctx, mode(key_version), flags, key, klen, iv, ivlen);
}

static unsigned int get_length(unsigned int slen, unsigned int key_id,
                               unsigned int key_version)
{
  return my_aes_get_size(mode(key_version), slen);
}

static int example_key_management_plugin_init(void *p)
{
  /* init */
  pthread_mutex_init(&mutex, NULL);
  get_latest_key_version(1);

  return 0;
}

static int example_key_management_plugin_deinit(void *p)
{
  pthread_mutex_destroy(&mutex);
  return 0;
}


static int ctx_update(void *ctx, const unsigned char *src, unsigned int slen,
  unsigned char *dst, unsigned int *dlen)
{
  return my_aes_crypt_update(ctx, src, slen, dst, dlen);
}


int ctx_finish(void *ctx, unsigned char *dst, unsigned int *dlen)
{
  return my_aes_crypt_finish(ctx, dst, dlen);
}

static  uint ctx_size(unsigned int , unsigned int key_version)
{
  return my_aes_ctx_size(mode(key_version));
}

struct st_mariadb_encryption example_key_management_plugin= {
  MariaDB_ENCRYPTION_INTERFACE_VERSION,
  get_latest_key_version,
  get_key,
  ctx_size,
  ctx_init,
  ctx_update,
  ctx_finish,
  get_length
};

/*
  Plugin library descriptor
*/
maria_declare_plugin(example_key_management)
{
  MariaDB_ENCRYPTION_PLUGIN,
  &example_key_management_plugin,
  "example_key_management",
  "Jonas Oreland",
  "Example key management plugin",
  PLUGIN_LICENSE_GPL,
  example_key_management_plugin_init,
  example_key_management_plugin_deinit,
  0x0100 /* 1.0 */,
  NULL,	/* status variables */
  NULL,	/* system variables */
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;

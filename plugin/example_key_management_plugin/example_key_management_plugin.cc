// Copyright (C) 2014 Google Inc.

#include <mysql_version.h>
#include <my_global.h>
#include <my_pthread.h>
#include <my_aes.h>
#include <my_crypt_key_management.h>
#include <my_md5.h>
#include <my_rnd.h>

/* rotate key randomly between 45 and 90 seconds */
#define KEY_ROTATION_MIN 45
#define KEY_ROTATION_MAX 90

static struct my_rnd_struct seed;
static unsigned int key_version = 0;
static unsigned int next_key_version = 0;
static pthread_mutex_t mutex;

static
int
get_latest_key_version()
{
  uint now = time(0);
  pthread_mutex_lock(&mutex);
  if (now >= next_key_version)
  {
    key_version = now;
    unsigned int interval = KEY_ROTATION_MAX - KEY_ROTATION_MIN;
    next_key_version = now + KEY_ROTATION_MIN + my_rnd(&seed) * interval;
  }
  pthread_mutex_unlock(&mutex);

  return key_version;
}

static
int
get_key(unsigned int version, unsigned char* dstbuf, unsigned buflen)
{
  char *dst = (char*)dstbuf; // md5 function takes char* as argument...
  unsigned len = 0;
  for (; len + MD5_HASH_SIZE <= buflen; len += MD5_HASH_SIZE)
  {
    compute_md5_hash(dst, (const char*)&version, sizeof(version));
    dst += MD5_HASH_SIZE;
    version++;
  }
  if (len < buflen)
  {
    memset(dst, 0, buflen - len);
  }
  return 0;
}

static unsigned int has_key_func(unsigned int keyID)
{
  return true;
}

static int get_key_size(unsigned int keyID)
{
	return 16;
}

static int get_iv(unsigned int keyID, unsigned char* dstbuf, unsigned buflen)
{
  if (buflen < 16)
  {
	  return CRYPT_BUFFER_TO_SMALL;
  }

  for (int i=0; i<16; i++)
	  dstbuf[i] = 0;

  return CRYPT_KEY_OK;
}


static int example_key_management_plugin_init(void *p)
{
  /* init */
  my_rnd_init(&seed, time(0), 0);
  get_latest_key_version();

  my_aes_init_dynamic_encrypt(MY_AES_ALGORITHM_CTR);

  pthread_mutex_init(&mutex, NULL);

  struct CryptoKeyFuncs_t func;
  func.getLatestCryptoKeyVersionFunc = get_latest_key_version;
  func.hasCryptoKeyFunc = has_key_func;
  func.getCryptoKeySize = get_key_size;
  func.getCryptoKeyFunc = get_key;
  func.getCryptoIVFunc = get_iv;
  InstallCryptoKeyFunctions(&func);
  return 0;
}

static int example_key_management_plugin_deinit(void *p)
{
  pthread_mutex_destroy(&mutex);
  return 0;
}

struct st_mysql_daemon example_key_management_plugin= {
  MYSQL_DAEMON_INTERFACE_VERSION
};

/*
  Plugin library descriptor
*/
maria_declare_plugin(example_key_management_plugin)
{
  MYSQL_DAEMON_PLUGIN,
  &example_key_management_plugin,
  "example_key_management_plugin",
  "Jonas Oreland",
  "Example key management plugin",
  PLUGIN_LICENSE_GPL,
  example_key_management_plugin_init,
  example_key_management_plugin_deinit,
  0x0100 /* 1.0 */,
  NULL,	/* status variables */
  NULL,	/* system variables */
  "1.0",
  MariaDB_PLUGIN_MATURITY_UNKNOWN
}
maria_declare_plugin_end;

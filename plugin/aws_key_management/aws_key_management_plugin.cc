/*
  Copyright (c) 2016 MariaDB Corporation

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


#include <my_global.h>
#include <my_pthread.h>
#include <my_sys.h>
#include <my_dir.h>
#include <mysql/plugin_encryption.h>
#include <my_crypt.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <mysqld_error.h>
#include <base64.h>
#include <map>
#include <algorithm>
#include <string>
#include <vector>
#include <iterator>
#include <sstream>
#include <fstream>

#include <aws/core/client/AWSError.h>
#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/logging/ConsoleLogSystem.h>
#include <aws/kms/KMSClient.h>
#include <aws/kms/model/DecryptRequest.h>
#include <aws/kms/model/DecryptResult.h>
#include <aws/kms/model/GenerateDataKeyWithoutPlaintextRequest.h>
#include <aws/kms/model/GenerateDataKeyWithoutPlaintextResult.h>
#include <aws/core/utils/Outcome.h>

using namespace std;
using namespace Aws::KMS;
using namespace Aws::KMS::Model;
using namespace Aws::Utils::Logging;
extern void sql_print_error(const char *format, ...);
extern void sql_print_warning(const char *format, ...);
extern void sql_print_information(const char *format, ...);


/* Plaintext key info struct */
struct KEY_INFO
{
  unsigned int  key_id;
  unsigned int  key_version;
  unsigned int  length;
  unsigned char data[MY_AES_MAX_KEY_LENGTH];
  bool          load_failed; /* if true, do not attempt to reload?*/
public:
  KEY_INFO() : key_id(0), key_version(0), length(0), load_failed(false){};
};
#define KEY_ID_AND_VERSION(key_id,version) ((longlong)key_id << 32 | version)

/* Cache for the latest version, per key id */
static std::map<uint, uint> latest_version_cache;

/* Cache for plaintext keys */
static std::map<ulonglong, KEY_INFO>  key_info_cache;

static const char *key_spec_names[]={ "AES_128", "AES_256", 0 };

/* Plugin variables */
static char* master_key_id;
static unsigned long key_spec;
static unsigned long log_level;
static int rotate_key;

/* AWS functionality*/
static int aws_decrypt_key(const char *path, KEY_INFO *info);
static int aws_generate_datakey(uint key_id, uint version);

static int extract_id_and_version(const char *name, uint *id, uint *ver);
static unsigned int get_latest_key_version(unsigned int key_id);
static unsigned int get_latest_key_version_nolock(unsigned int key_id);
static int load_key(KEY_INFO *info);

/* Mutex to serialize access to caches */
static mysql_mutex_t mtx;

#ifdef HAVE_PSI_INTERFACE
static uint mtx_key;
static PSI_mutex_info mtx_info = {&mtx_key, "mtx", 0};
#endif

static Aws::KMS::KMSClient *client;

/* Redirect AWS trace to error log */
class  MySQLLogSystem : public Aws::Utils::Logging::FormattedLogSystem
{
public:

  using Base = FormattedLogSystem;
  MySQLLogSystem(LogLevel logLevel) :
    Base(logLevel) 
  {
  }
  virtual LogLevel GetLogLevel(void) const override
  {
    return (LogLevel)log_level;
  }
  virtual ~MySQLLogSystem() 
  {
  }

protected:
  virtual void ProcessFormattedStatement(Aws::String&& statement) override
  {
#ifdef _WIN32
    /*
      On Windows, we can't use C runtime functions to write to stdout,
      because we compile with static C runtime, so plugin has a stdout
      different from server. Thus we're using WriteFile().
    */
    DWORD nSize= (DWORD)statement.size();
    DWORD nWritten;
    const char *s= statement.c_str();
    HANDLE h= GetStdHandle(STD_OUTPUT_HANDLE);

    WriteFile(h, s, nSize, &nWritten, NULL);
#else
    printf("%s", statement.c_str());
#endif
  }
};


/* 
  Plugin initialization.

  Create KMS client and scan datadir to find out which keys and versions
  are present.
*/
static int plugin_init(void *p)
{
  DBUG_ENTER("plugin_init");
  client = new KMSClient();
  if (!client)
  {
    sql_print_error("Can not initialize KMS client");
    DBUG_RETURN(-1);
  }
  InitializeAWSLogging(Aws::MakeShared<MySQLLogSystem>("aws_key_management_plugin", (Aws::Utils::Logging::LogLevel) log_level));
#ifdef HAVE_PSI_INTERFACE
  mysql_mutex_register("aws_key_management", &mtx_info, 1);
#endif
  mysql_mutex_init(mtx_key, &mtx, NULL);

  MY_DIR *dirp = my_dir(".", MYF(0));
  if (!dirp)
  {
    sql_print_error("Can't scan current directory");
    DBUG_RETURN(-1);
  }
  for (unsigned int i=0; i < dirp->number_of_files; i++)
  {

    KEY_INFO info;
    if (extract_id_and_version(dirp->dir_entry[i].name, &info.key_id, &info.key_version) == 0)
    {
      key_info_cache[KEY_ID_AND_VERSION(info.key_id, info.key_version)]= info;
      latest_version_cache[info.key_id]= max(info.key_version, latest_version_cache[info.key_id]);
    }
  }
  my_dirend(dirp);
  DBUG_RETURN(0);
}


static int plugin_deinit(void *p)
{
  DBUG_ENTER("plugin_deinit");
  latest_version_cache.clear();
  key_info_cache.clear();
  mysql_mutex_destroy(&mtx);
  delete client;
  ShutdownAWSLogging();
  DBUG_RETURN(0);
}

/*  Generate filename to store the ciphered key */
static void format_keyfile_name(char *buf, size_t size, uint key_id, uint version)
{
  snprintf(buf, size, "aws-kms-key.%u.%u", key_id, version);
}

/* Extract key id and version from file name */
static int extract_id_and_version(const char *name, uint *id, uint *ver)
{
  int len;
  int n= sscanf(name, "aws-kms-key.%u.%u%n", id, ver, &len);
  if (n == 2 && *id > 0 && *ver > 0 && len == (int)strlen(name))
    return 0;
  return 1;
}

/*
  Decrypt key stored in aws-kms-key.<id>.<version>
  Cache the decrypted key.
*/
static int load_key(KEY_INFO *info)
{
  int ret;
  char path[256];
  DBUG_ENTER("load_key");
  DBUG_PRINT("enter", ("id=%u,ver=%u", info->key_id, info->key_version));
  format_keyfile_name(path, sizeof(path), info->key_id, info->key_version);
  ret= aws_decrypt_key(path, info);
  if (ret)
    info->load_failed= true;

  latest_version_cache[info->key_id]= max(latest_version_cache[info->key_id], info->key_version);
  key_info_cache[KEY_ID_AND_VERSION(info->key_id, info->key_version)]= *info;

  if (!ret)
  {
    sql_print_information("AWS KMS plugin: loaded key %u, version %u, key length %u bit",
      info->key_id, info->key_version,(uint)info->length*8);
  }
  else
  {
    sql_print_warning("AWS KMS plugin: key %u, version %u could not be decrypted",
      info->key_id, info->key_version);
  }
  DBUG_RETURN(ret);
}


/* 
  Get latest version for the key.

  If key is not decrypted yet, this function also decrypt the key
  and error will be returned if decryption fails.

  The reason for that is that Innodb crashes
   in case errors are returned by get_key(),

  A new key will be created if it does not exist, provided there is
  valid master_key_id.
*/
static unsigned int get_latest_key_version(unsigned int key_id)
{
  unsigned int ret;
  DBUG_ENTER("get_latest_key_version");
  mysql_mutex_lock(&mtx);
  ret= get_latest_key_version_nolock(key_id);
  mysql_mutex_unlock(&mtx);
  DBUG_PRINT("info", ("key=%u,ret=%u", key_id, ret));
  DBUG_RETURN(ret);
}

static unsigned int get_latest_key_version_nolock(unsigned int key_id)
{
  KEY_INFO info;
  uint ver;
  DBUG_ENTER("get_latest_key_version_nolock");
  ver= latest_version_cache[key_id];
  if (ver > 0)
  {
    info= key_info_cache[KEY_ID_AND_VERSION(key_id, ver)];
  }
  if (info.load_failed)
  {
    /* Decryption failed previously, don't retry */
    DBUG_RETURN(ENCRYPTION_KEY_VERSION_INVALID);
  }
  else if (ver > 0)
  {
    /* Key exists already, return it*/
    if (info.length > 0)
      DBUG_RETURN(ver);
  }
  else // (ver == 0)
  {
    /* Generate a new key, version 1 */
    if (!master_key_id[0])
    {
      my_printf_error(ER_UNKNOWN_ERROR,
        "Can't generate encryption key %u, because 'aws_key_management_master_key_id' parameter is not set",
        MYF(0), key_id);
      DBUG_RETURN(ENCRYPTION_KEY_VERSION_INVALID);
    }
    if (aws_generate_datakey(key_id, 1) != 0)
      DBUG_RETURN(ENCRYPTION_KEY_VERSION_INVALID);
    info.key_id= key_id;
    info.key_version= 1;
    info.length= 0;
  }

  if (load_key(&info))
    DBUG_RETURN(ENCRYPTION_KEY_VERSION_INVALID);
  DBUG_RETURN(info.key_version);
}


/* 
  Decrypt a file with KMS
*/
static  int aws_decrypt_key(const char *path, KEY_INFO *info)
{
  DBUG_ENTER("aws_decrypt_key");

  /* Read file content into memory */
  ifstream ifs(path, ios::binary | ios::ate);
  if (!ifs.good())
  {
    sql_print_error("can't open file %s", path);
    DBUG_RETURN(-1);
  }
  size_t pos = (size_t)ifs.tellg();
  if (!pos || pos == SIZE_T_MAX)
  {
    sql_print_error("invalid key file %s", path);
    DBUG_RETURN(-1);
  }
  std::vector<char>  contents(pos);
  ifs.seekg(0, ios::beg);
  ifs.read(&contents[0], pos);

  /* Decrypt data the with AWS */
  DecryptRequest request;
  Aws::Utils::ByteBuffer byteBuffer((unsigned char *)contents.data(), pos);
  request.SetCiphertextBlob(byteBuffer);
  DecryptOutcome outcome = client->Decrypt(request);
  if (!outcome.IsSuccess())
  {
    sql_print_error("AWS KMS plugin: Decrypt failed for %s : %s", path,
      outcome.GetError().GetMessage().c_str());
    DBUG_RETURN(-1);
  }
  Aws::Utils::ByteBuffer plaintext = outcome.GetResult().GetPlaintext();
  size_t len = plaintext.GetLength();

  if (len > (int)sizeof(info->data))
  {
    sql_print_error("AWS KMS plugin: encoding key too large for %s", path);
    DBUG_RETURN(ENCRYPTION_KEY_BUFFER_TOO_SMALL);
  }
  memcpy(info->data, plaintext.GetUnderlyingData(), len);
  info->length= len;
  DBUG_RETURN(0);
}


/* Generate a new datakey and store it a file */
static int aws_generate_datakey(uint keyid, uint version)
{

  DBUG_ENTER("aws_generate_datakey");
  GenerateDataKeyWithoutPlaintextRequest request;
  request.SetKeyId(master_key_id);
  request.SetKeySpec(DataKeySpecMapper::GetDataKeySpecForName(key_spec_names[key_spec]));

  GenerateDataKeyWithoutPlaintextOutcome outcome;
  outcome= client->GenerateDataKeyWithoutPlaintext(request);
  if (!outcome.IsSuccess())
  {
    sql_print_error("AWS KMS plugin : GenerateDataKeyWithoutPlaintext failed : %s - %s",
      outcome.GetError().GetExceptionName().c_str(),
      outcome.GetError().GetMessage().c_str());
    DBUG_RETURN(-1);
  }

  string out;
  char filename[20];
  Aws::Utils::ByteBuffer byteBuffer = outcome.GetResult().GetCiphertextBlob();

  format_keyfile_name(filename, sizeof(filename), keyid, version);
  int fd= my_open(filename, O_RDWR | O_CREAT, 0);
  if (fd < 0)
  {
    sql_print_error("AWS KMS plugin: Can't create file %s", filename);
    DBUG_RETURN(-1);
  }
  size_t len= byteBuffer.GetLength();
  if (my_write(fd, byteBuffer.GetUnderlyingData(), len, 0) != len)
  {
    sql_print_error("AWS KMS plugin: can't write to %s", filename);
    my_close(fd, 0);
    my_delete(filename, 0);
    DBUG_RETURN(-1);
  }
  my_close(fd, 0);
  sql_print_information("AWS KMS plugin: generated encrypted datakey for key id=%u, version=%u",
    keyid, version);
  DBUG_RETURN(0);
}

/* Key rotation  for a single key */
static int rotate_single_key(uint key_id)
{
  uint ver;
  ver= latest_version_cache[key_id];

  if (!ver)
  {
    my_printf_error(ER_UNKNOWN_ERROR, "key %u does not exist", MYF(ME_JUST_WARNING), key_id);
    return -1;
  }
  else if (aws_generate_datakey(key_id, ver + 1))
  {
    my_printf_error(ER_UNKNOWN_ERROR, "Could not generate datakey for key id= %u, ver= %u",
      MYF(ME_JUST_WARNING), key_id, ver);
    return -1;
  }
  else
  {
    KEY_INFO info;
    info.key_id= key_id;
    info.key_version = ver + 1;
    if (load_key(&info))
    {
      my_printf_error(ER_UNKNOWN_ERROR, "Could not load datakey for key id= %u, ver= %u",
        MYF(ME_JUST_WARNING), key_id, ver);
      return -1;
    }
  }
  return 0;
}

/* Key rotation for all key ids */
static int rotate_all_keys()
{
  int ret= 0;
  for (map<uint, uint>::iterator it= latest_version_cache.begin(); it != latest_version_cache.end(); it++)
  {
    ret= rotate_single_key(it->first);
    if (ret)
      break;
  }
  return ret;
}

static void update_rotate(MYSQL_THD, struct st_mysql_sys_var *, void *, const void *val)
{
  if (!master_key_id[0])
  {
    my_printf_error(ER_UNKNOWN_ERROR,
      "aws_key_management_master_key_id must be set to generate new data keys", MYF(ME_JUST_WARNING));
    return;
  }
  mysql_mutex_lock(&mtx);
  rotate_key= *(int *)val;
  switch (rotate_key)
  {
  case 0:
    break;
  case -1:
    rotate_all_keys();
    break;
  default:
    rotate_single_key(rotate_key);
    break;
  }
  rotate_key= 0;
  mysql_mutex_unlock(&mtx);
}

static unsigned int get_key(
  unsigned int key_id,
  unsigned int version,
  unsigned char* dstbuf,
  unsigned int* buflen)
{
  KEY_INFO info;

  DBUG_ENTER("get_key");
  mysql_mutex_lock(&mtx);
  info= key_info_cache[KEY_ID_AND_VERSION(key_id, version)];
  if (info.length == 0 && !info.load_failed)
  {
    info.key_id= key_id;
    info.key_version= version;
    load_key(&info);
  }
  mysql_mutex_unlock(&mtx);
  if (info.load_failed)
    DBUG_RETURN(ENCRYPTION_KEY_VERSION_INVALID);
  if (*buflen < info.length)
  {
    *buflen= info.length;
    DBUG_RETURN(ENCRYPTION_KEY_BUFFER_TOO_SMALL);
  }
  *buflen= info.length;
  memcpy(dstbuf, info.data, info.length);
  DBUG_RETURN(0);
}


/* Plugin defs */
struct st_mariadb_encryption aws_key_management_plugin= {
  MariaDB_ENCRYPTION_INTERFACE_VERSION,
  get_latest_key_version,
  get_key,
  // use default encrypt/decrypt functions
  0, 0, 0, 0, 0
};


static TYPELIB key_spec_typelib =
{
  array_elements(key_spec_names) - 1, "",
  key_spec_names, NULL
};

const char *log_level_names[] =
{
  "Off",
  "Fatal",
  "Error",
  "Warn",
  "Info",
  "Debug",
  "Trace",
  0
};

static TYPELIB log_level_typelib =
{
  array_elements(log_level_names) - 1, "",
  log_level_names, NULL
};

static MYSQL_SYSVAR_STR(master_key_id, master_key_id,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Key id for master encryption key. Used to create new datakeys. If not set, no new keys will be created",
  NULL, NULL, "");

static MYSQL_SYSVAR_ENUM(key_spec, key_spec,
  PLUGIN_VAR_RQCMDARG,
  "Encryption algorithm used to create new keys.",
  NULL, NULL, 0, &key_spec_typelib);


static MYSQL_SYSVAR_ENUM(log_level, log_level,
  PLUGIN_VAR_RQCMDARG,
  "Logging for AWS API",
  NULL, NULL, 0, &log_level_typelib);


static MYSQL_SYSVAR_INT(rotate_key, rotate_key,
  PLUGIN_VAR_RQCMDARG,
  "Set this variable to key id to perform rotation of the key. Specify -1 to rotate all keys",
  NULL, update_rotate, 0, -1, INT_MAX, 1);

static struct st_mysql_sys_var* settings[]= {
  MYSQL_SYSVAR(master_key_id),
  MYSQL_SYSVAR(key_spec),
  MYSQL_SYSVAR(rotate_key),
  MYSQL_SYSVAR(log_level),
  NULL
};

/*
  Plugin library descriptor
*/
maria_declare_plugin(aws_key_management)
{
  MariaDB_ENCRYPTION_PLUGIN,
    &aws_key_management_plugin,
    "aws_key_management",
    "MariaDB Corporation",
    "AWS key management plugin",
    PLUGIN_LICENSE_GPL,
    plugin_init,
    plugin_deinit,
    0x0100,
    NULL,
    settings,
    "1.0",
    MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;

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
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


#include <my_global.h>
#include <typelib.h>
#include <mysql/plugin_encryption.h>
#include <my_crypt.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <mysqld_error.h>
#include <my_sys.h>
#include <map>
#include <algorithm>
#include <string>
#include <vector>
#include <iterator>
#include <sstream>
#include <fstream>

#ifndef _WIN32
#include <dirent.h>
#endif

#include <aws/core/Aws.h>
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
static char* region;
static unsigned long key_spec;
static unsigned long log_level;
static int rotate_key;
static int request_timeout;
static char* endpoint_url;

#ifndef DBUG_OFF
#define WITH_AWS_MOCK 1
#else
#define WITH_AWS_MOCK 0
#endif

#if WITH_AWS_MOCK
static char mock;
#endif


/* AWS functionality*/
static int read_and_decrypt_key(const char *path, KEY_INFO *info);
static int generate_and_save_datakey(uint key_id, uint version);

static int extract_id_and_version(const char *name, uint *id, uint *ver);
static unsigned int get_latest_key_version(unsigned int key_id);
static unsigned int get_latest_key_version_nolock(unsigned int key_id);
static int load_key(KEY_INFO *info);
static std::mutex mtx;


static Aws::KMS::KMSClient *client;

static void print_kms_error(const char *func, const Aws::Client::AWSError<Aws::KMS::KMSErrors>& err)
{
  my_printf_error(ER_UNKNOWN_ERROR,
    "AWS KMS plugin : KMS Client API '%s' failed : %s - %s",
    ME_ERROR_LOG_ONLY,
    func, err.GetExceptionName().c_str(), err.GetMessage().c_str());
}

#if WITH_AWS_MOCK
/* 
  Mock routines to test plugin without actual AWS KMS interaction
  we only need to mock 2 functions - generating encrypted key, and decrypt

  This mock functions do no-op encryption, i.e encrypt and decrypt of
  a buffer return the buffer itself.
*/

/*
  Generate random "encrypted" key. We do not encrypt anything in mock mode.
*/
static int mock_generate_encrypted_key(Aws::Utils::ByteBuffer *result)
{
  size_t len = key_spec == 0?16 : 32;
  *result = Aws::Utils::ByteBuffer(len);
  my_random_bytes(result->GetUnderlyingData(), (int)len);
  return 0;
}


static int mock_decrypt(Aws::Utils::ByteBuffer input, Aws::Utils::ByteBuffer* output)
{
  /* We do not encrypt or decrypt in mock mode.*/
  *output = input;
  return 0;
}
#endif

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

  virtual void Flush(void) override
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

/*  Get list of files in current directory */
static vector<string> traverse_current_directory()
{
  vector<string> v;
#ifdef _WIN32
  WIN32_FIND_DATA find_data;
  HANDLE h= FindFirstFile("*.*", &find_data);
  if (h == INVALID_HANDLE_VALUE)
    return v;
  do
  {
    v.push_back(find_data.cFileName);
  }
  while (FindNextFile(h, &find_data));
  FindClose(h);
#else
  DIR *dir = opendir(".");
  if (!dir)
    return v;
  struct dirent *e;
  while ((e= readdir(dir)))
    v.push_back(e->d_name);
  closedir(dir);
#endif
  return v;
}

Aws::SDKOptions sdkOptions;

static int aws_init()
{

#ifdef HAVE_WOLFSSL
  sdkOptions.cryptoOptions.initAndCleanupOpenSSL = true;
#else
  /* Server initialized OpenSSL already, thus AWS must skip it */
  sdkOptions.cryptoOptions.initAndCleanupOpenSSL = false;
#endif

  Aws::InitAPI(sdkOptions);
  InitializeAWSLogging(Aws::MakeShared<MySQLLogSystem>("aws_key_management_plugin", (Aws::Utils::Logging::LogLevel) log_level));

  Aws::Client::ClientConfiguration clientConfiguration;
  if (region && region[0])
  {
    clientConfiguration.region = region;
  }
  if (endpoint_url && endpoint_url[0])
  {
    clientConfiguration.endpointOverride = endpoint_url;
  }
  if (request_timeout)
  {
     clientConfiguration.requestTimeoutMs= request_timeout;
     clientConfiguration.connectTimeoutMs= request_timeout;
  }
  client = new KMSClient(clientConfiguration);
  if (!client)
  {
    my_printf_error(ER_UNKNOWN_ERROR, "Can't initialize KMS client", ME_ERROR_LOG_ONLY | ME_WARNING);
    return -1;
  }
  return 0;
}

static int init()
{
#if WITH_AWS_MOCK
  if(mock)
    return 0;
#endif
  return aws_init();
}

/* 
  Plugin initialization.

  Create KMS client and scan datadir to find out which keys and versions
  are present.
*/
static int plugin_init(void *p)
{
  if (init())
    return -1;

  vector<string> files= traverse_current_directory();
  for (size_t i=0; i < files.size(); i++)
  {

    KEY_INFO info;
    if (extract_id_and_version(files[i].c_str(), &info.key_id, &info.key_version) == 0)
    {
      key_info_cache[KEY_ID_AND_VERSION(info.key_id, info.key_version)]= info;
      latest_version_cache[info.key_id]= max(info.key_version, latest_version_cache[info.key_id]);
    }
  }
 return 0;
}


static void aws_shutdown()
{
  delete client;
  ShutdownAWSLogging();
  Aws::ShutdownAPI(sdkOptions);
}


static void shutdown()
{
#if WITH_AWS_MOCK
  if(mock)
    return;
#endif
  aws_shutdown();
}


static int plugin_deinit(void *p)
{
  latest_version_cache.clear();
  key_info_cache.clear();
  shutdown();
  return 0;
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

  format_keyfile_name(path, sizeof(path), info->key_id, info->key_version);
  ret= read_and_decrypt_key(path, info);
  if (ret)
    info->load_failed= true;

  latest_version_cache[info->key_id]= max(latest_version_cache[info->key_id], info->key_version);
  key_info_cache[KEY_ID_AND_VERSION(info->key_id, info->key_version)]= *info;

  if (!ret)
  {
    my_printf_error(ER_UNKNOWN_ERROR, "AWS KMS plugin: loaded key %u, version %u, key length %u bit", ME_ERROR_LOG_ONLY | ME_NOTE,
      info->key_id, info->key_version,(uint)info->length*8);
  }
  else
  {
    my_printf_error(ER_UNKNOWN_ERROR, "AWS KMS plugin: key %u, version %u could not be decrypted", ME_ERROR_LOG_ONLY | ME_WARNING,
      info->key_id, info->key_version);
  }
  return ret;
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
  mtx.lock();
  ret= get_latest_key_version_nolock(key_id);
  mtx.unlock();
  return ret;
}

static unsigned int get_latest_key_version_nolock(unsigned int key_id)
{
  KEY_INFO info;
  uint ver;

  ver= latest_version_cache[key_id];
  if (ver > 0)
  {
    info= key_info_cache[KEY_ID_AND_VERSION(key_id, ver)];
  }
  if (info.load_failed)
  {
    /* Decryption failed previously, don't retry */
    return(ENCRYPTION_KEY_VERSION_INVALID);
  }
  else if (ver > 0)
  {
    /* Key exists already, return it*/
    if (info.length > 0)
      return(ver);
  }
  else // (ver == 0)
  {
    /* Generate a new key, version 1 */
    if (generate_and_save_datakey(key_id, 1) != 0)
      return(ENCRYPTION_KEY_VERSION_INVALID);
    info.key_id= key_id;
    info.key_version= 1;
    info.length= 0;
  }

  if (load_key(&info))
    return(ENCRYPTION_KEY_VERSION_INVALID);
  return(info.key_version);
}

/* Decrypt Byte buffer with AWS. */
static int aws_decrypt(Aws::Utils::ByteBuffer input, Aws::Utils::ByteBuffer* output)
{
  DecryptRequest request;
  request.SetCiphertextBlob(input);
  DecryptOutcome outcome = client->Decrypt(request);
  if (!outcome.IsSuccess())
  {
    print_kms_error("Decrypt", outcome.GetError());
    return -1;
  }
  *output= outcome.GetResult().GetPlaintext();
  return 0;
}


static int decrypt(Aws::Utils::ByteBuffer input, Aws::Utils::ByteBuffer* output)
{
#if WITH_AWS_MOCK
  if(mock)
    return mock_decrypt(input,output);
#endif
  return aws_decrypt(input, output);
}

/* 
  Decrypt a file with KMS
*/
static  int read_and_decrypt_key(const char *path, KEY_INFO *info)
{

  /* Read file content into memory */
  ifstream ifs(path, ios::binary | ios::ate);
  if (!ifs.good())
  {
    my_printf_error(ER_UNKNOWN_ERROR, "can't open file %s", ME_ERROR_LOG_ONLY, path);
    return(-1);
  }
  size_t pos = (size_t)ifs.tellg();
  if (!pos || pos == SIZE_T_MAX)
  {
    my_printf_error(ER_UNKNOWN_ERROR, "invalid key file %s", ME_ERROR_LOG_ONLY, path);
    return(-1);
  }
  std::vector<char>  contents(pos);
  ifs.seekg(0, ios::beg);
  ifs.read(&contents[0], pos);

  /* Decrypt data the with AWS */

  Aws::Utils::ByteBuffer input((unsigned char *)contents.data(), pos);
  Aws::Utils::ByteBuffer plaintext;

  if (decrypt(input, &plaintext))
  {
      return -1;
  }

  size_t len = plaintext.GetLength();

  if (len > sizeof(info->data))
  {
    my_printf_error(ER_UNKNOWN_ERROR, "AWS KMS plugin: encoding key too large for %s", ME_ERROR_LOG_ONLY, path);
    return(ENCRYPTION_KEY_BUFFER_TOO_SMALL);
  }
  memcpy(info->data, plaintext.GetUnderlyingData(), len);
  info->length= (unsigned int)len;
  return(0);
}


int aws_generate_encrypted_key(Aws::Utils::ByteBuffer *result)
{
  if (!master_key_id[0])
  {
    my_printf_error(ER_UNKNOWN_ERROR,
      "Can't generate encryption key, because 'aws_key_management_master_key_id' parameter is not set",
      MYF(0));
    return(-1);
  }
  GenerateDataKeyWithoutPlaintextRequest request;
  request.SetKeyId(master_key_id);
  request.SetKeySpec(DataKeySpecMapper::GetDataKeySpecForName(key_spec_names[key_spec]));

  GenerateDataKeyWithoutPlaintextOutcome outcome;
  outcome= client->GenerateDataKeyWithoutPlaintext(request);
  if (!outcome.IsSuccess())
  {
    print_kms_error("GenerateDataKeyWithoutPlaintext", outcome.GetError());
    return(-1);
  }
 *result =  outcome.GetResult().GetCiphertextBlob();
 return 0;
}


static int generate_encrypted_key(Aws::Utils::ByteBuffer *output)
{
#if WITH_AWS_MOCK
   if(mock)
     return mock_generate_encrypted_key(output);
#endif
   return aws_generate_encrypted_key(output);
}

/* Generate a new datakey and store it a file */
static int generate_and_save_datakey(uint keyid, uint version)
{
  Aws::Utils::ByteBuffer byteBuffer;

  if (generate_encrypted_key(&byteBuffer))
     return -1;

  string out;
  char filename[20];
  format_keyfile_name(filename, sizeof(filename), keyid, version);
  int fd= open(filename, O_WRONLY |O_CREAT|O_BINARY, IF_WIN(_S_IREAD, S_IRUSR| S_IRGRP| S_IROTH));
  if (fd < 0)
  {
    my_printf_error(ER_UNKNOWN_ERROR, "AWS KMS plugin: Can't create file %s", ME_ERROR_LOG_ONLY, filename);
    return(-1);
  }
  unsigned int len= (unsigned int)byteBuffer.GetLength();
  if (write(fd, byteBuffer.GetUnderlyingData(), len) != len)
  {
    my_printf_error(ER_UNKNOWN_ERROR, "AWS KMS plugin: can't write to %s", ME_ERROR_LOG_ONLY, filename);
    close(fd);
    unlink(filename);
    return(-1);
  }
  close(fd);
  my_printf_error(ER_UNKNOWN_ERROR, "AWS KMS plugin: generated encrypted datakey for key id=%u, version=%u", ME_ERROR_LOG_ONLY | ME_NOTE,
    keyid, version);
  return(0);
}

/* Key rotation  for a single key */
static int rotate_single_key(uint key_id)
{
  uint ver;
  ver= latest_version_cache[key_id];

  if (!ver)
  {
    my_printf_error(ER_UNKNOWN_ERROR, "key %u does not exist", MYF(ME_WARNING), key_id);
    return -1;
  }
  else if (generate_and_save_datakey(key_id, ver + 1))
  {
    my_printf_error(ER_UNKNOWN_ERROR, "Could not generate datakey for key id= %u, ver= %u",
      MYF(ME_WARNING), key_id, ver);
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
        MYF(ME_WARNING), key_id, ver);
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
      "aws_key_management_master_key_id must be set to generate new data keys", MYF(ME_WARNING));
    return;
  }
  mtx.lock();
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
  mtx.unlock();
}

static unsigned int get_key(
  unsigned int key_id,
  unsigned int version,
  unsigned char* dstbuf,
  unsigned int* buflen)
{
  KEY_INFO info;

  mtx.lock();
  info= key_info_cache[KEY_ID_AND_VERSION(key_id, version)];
  if (info.length == 0 && !info.load_failed)
  {
    info.key_id= key_id;
    info.key_version= version;
    load_key(&info);
  }
  mtx.unlock();
  if (info.load_failed)
    return(ENCRYPTION_KEY_VERSION_INVALID);
  if (*buflen < info.length)
  {
    *buflen= info.length;
    return(ENCRYPTION_KEY_BUFFER_TOO_SMALL);
  }
  *buflen= info.length;
  memcpy(dstbuf, info.data, info.length);
  return(0);
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


static MYSQL_SYSVAR_INT(request_timeout, request_timeout,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Timeout in milliseconds for create HTTPS connection or execute AWS request. Specify 0 to use SDK default.",
  NULL, NULL, 0, 0, INT_MAX, 1);

static MYSQL_SYSVAR_STR(region, region,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "AWS region. For example us-east-1, or eu-central-1. If no value provided, SDK default is used.",
  NULL, NULL, "");

static MYSQL_SYSVAR_STR(endpoint_url, endpoint_url,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Used to override the default AWS API endpoint. If not set, the default will be used",
  NULL, NULL, "");

#if WITH_AWS_MOCK
static MYSQL_SYSVAR_BOOL(mock, mock,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
 "Mock AWS KMS calls (for testing).",
  NULL,  NULL, 0);
#endif

static struct st_mysql_sys_var* settings[]= {
  MYSQL_SYSVAR(master_key_id),
  MYSQL_SYSVAR(key_spec),
  MYSQL_SYSVAR(rotate_key),
  MYSQL_SYSVAR(log_level),
  MYSQL_SYSVAR(request_timeout),
  MYSQL_SYSVAR(region),
  MYSQL_SYSVAR(endpoint_url),
#if WITH_AWS_MOCK
  MYSQL_SYSVAR(mock),
#endif
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
    MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;

/* Copyright (C) 2019-2022 MariaDB Corporation

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

#include <mysql/plugin_encryption.h>
#include <mysqld_error.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <string>
#include <sstream>
#include <curl/curl.h>
#ifdef _WIN32
#include <malloc.h>
#define alloca _alloca
#elif !defined(__FreeBSD__)
#include <alloca.h>
#endif
#include <algorithm>
#include <unordered_map>
#include <mutex>

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define HASHICORP_HAVE_EXCEPTIONS 1
#else
#define HASHICORP_HAVE_EXCEPTIONS 0
#endif

#define HASHICORP_DEBUG_LOGGING 0

#define PLUGIN_ERROR_HEADER "hashicorp: "

/* Key version information structure: */
typedef struct VER_INFO
{
  unsigned int key_version;
  clock_t timestamp;
  VER_INFO() : key_version(0), timestamp(0) {};
  VER_INFO(unsigned int key_version_, clock_t timestamp_) :
    key_version(key_version_), timestamp(timestamp_) {};
} VER_INFO;

/* Key information structure: */
typedef struct KEY_INFO
{
  unsigned int key_id;
  unsigned int key_version;
  clock_t timestamp;
  unsigned int length;
  unsigned char data [MY_AES_MAX_KEY_LENGTH];
  KEY_INFO() : key_id(0), key_version(0), timestamp(0), length(0) {};
  KEY_INFO(unsigned int key_id_,
           unsigned int key_version_,
           clock_t timestamp_,
           unsigned int length_) :
    key_id(key_id_), key_version(key_version_),
    timestamp(timestamp_), length(length_) {};
} KEY_INFO;

/* Cache for the latest version, per key id: */
typedef std::unordered_map<unsigned int, VER_INFO> VER_MAP;

/* Cache for key information: */
typedef std::unordered_map<unsigned long long, KEY_INFO> KEY_MAP;

#define KEY_ID_AND_VERSION(key_id, version) \
  ((unsigned long long)key_id << 32 | version)

class HCData
{
private:
  struct curl_slist *slist;
  char *vault_url_data;
  size_t vault_url_len;
  char *local_token;
  char *token_header;
  bool curl_inited;
public:
  HCData()
   :slist(NULL),
    vault_url_data(NULL),
    vault_url_len(0),
    local_token(NULL),
    token_header(NULL),
    curl_inited(false)
  {}
  unsigned int get_latest_version (unsigned int key_id);
  unsigned int get_key_from_vault (unsigned int key_id,
                                   unsigned int key_version,
                                   unsigned char *dstbuf,
                                   unsigned int *buflen);
  int init ();
  void deinit ()
  {
    if (slist)
    {
      curl_slist_free_all(slist);
      slist = NULL;
    }
    if (curl_inited)
    {
      curl_global_cleanup();
      curl_inited = false;
    }
    vault_url_len = 0;
    if (vault_url_data)
    {
      free(vault_url_data);
      vault_url_data = NULL;
    }
    if (token_header)
    {
      free(token_header);
      token_header = NULL;
    }
    if (local_token)
    {
      free(local_token);
      local_token = NULL;
    }
  }
  void cache_clean ()
  {
    latest_version_cache.clear();
    key_info_cache.clear();
  }
private:
  std::mutex mtx;
  VER_MAP latest_version_cache;
  KEY_MAP key_info_cache;
private:
  void cache_add (const KEY_INFO& info, bool update_version);
  unsigned int cache_get (unsigned int key_id, unsigned int key_version,
                          unsigned char* data, unsigned int* buflen,
                          bool with_timeouts);
  unsigned int cache_check_version (unsigned int key_id);
  unsigned int cache_get_version (unsigned int key_id);
  int curl_run (const char *url, std::string *response,
                bool soft_timeout) const;
  int check_version (const char *mount_url) const;
  void *alloc (size_t nbytes) const
  {
    void *res = (char *) malloc(nbytes);
    if (!res)
    {
      my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                      "Memory allocation error", 0);
    }
    return res;
  }
};

static HCData data;

static clock_t cache_max_time;
static clock_t cache_max_ver_time;

/*
  Convert milliseconds to timer ticks with rounding
  to nearest integer:
*/
static clock_t ms_to_ticks (long ms)
{
  long long ticks_1000 = ms * (long long) CLOCKS_PER_SEC;
  clock_t ticks = (clock_t) (ticks_1000 / 1000);
  return ticks + ((clock_t) (ticks_1000 % 1000) >= 500);
}

void HCData::cache_add (const KEY_INFO& info, bool update_version)
{
  unsigned int key_id = info.key_id;
  unsigned int key_version = info.key_version;
  mtx.lock();
  VER_INFO &ver_info = latest_version_cache[key_id];
  if (update_version || ver_info.key_version < key_version)
  {
    ver_info.key_version = key_version;
    ver_info.timestamp = info.timestamp;
  }
  key_info_cache[KEY_ID_AND_VERSION(key_id, key_version)] = info;
#if HASHICORP_DEBUG_LOGGING
  my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                  "cache_add: key_id = %u, key_version = %u, "
                  "timestamp = %u, update_version = %u, new version = %u",
                  ME_ERROR_LOG_ONLY | ME_NOTE, key_id, key_version,
                  ver_info.timestamp, (int) update_version,
                  ver_info.key_version);
#endif
  mtx.unlock();
}

unsigned int
  HCData::cache_get (unsigned int key_id, unsigned int key_version,
                     unsigned char* data, unsigned int* buflen,
                     bool with_timeouts)
{
  unsigned int version = key_version;
  clock_t current_time = clock();
  mtx.lock();
  if (key_version == ENCRYPTION_KEY_VERSION_INVALID)
  {
    clock_t timestamp;
#if HASHICORP_HAVE_EXCEPTIONS
    try
    {
      VER_INFO &ver_info = latest_version_cache.at(key_id);
      version = ver_info.key_version;
      timestamp = ver_info.timestamp;
    }
    catch (const std::out_of_range &e)
#else
    VER_MAP::const_iterator ver_iter = latest_version_cache.find(key_id);
    if (ver_iter != latest_version_cache.end())
    {
      version = ver_iter->second.key_version;
      timestamp = ver_iter->second.timestamp;
    }
    else
#endif
    {
      mtx.unlock();
      return ENCRYPTION_KEY_VERSION_INVALID;
    }
#if HASHICORP_DEBUG_LOGGING
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "cache_get: key_id = %u, key_version = %u, "
                    "last version = %u, version timestamp = %u, "
                    "current time = %u, diff = %u",
                    ME_ERROR_LOG_ONLY | ME_NOTE, key_id, key_version,
                    version, timestamp, current_time,
                    current_time - timestamp);
#endif
    if (with_timeouts && current_time - timestamp > cache_max_ver_time)
    {
      mtx.unlock();
      return ENCRYPTION_KEY_VERSION_INVALID;
    }
  }
  KEY_INFO info;
#if HASHICORP_HAVE_EXCEPTIONS
  try
  {
    info = key_info_cache.at(KEY_ID_AND_VERSION(key_id, version));
  }
  catch (const std::out_of_range &e)
#else
  KEY_MAP::const_iterator key_iter =
    key_info_cache.find(KEY_ID_AND_VERSION(key_id, version));
  if (key_iter != key_info_cache.end())
  {
    info = key_iter->second;
  }
  else
#endif
  {
    mtx.unlock();
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  mtx.unlock();
#if HASHICORP_DEBUG_LOGGING
  my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                  "cache_get: key_id = %u, key_version = %u, "
                  "effective version = %u, key data timestamp = %u, "
                  "current time = %u, diff = %u",
                  ME_ERROR_LOG_ONLY | ME_NOTE, key_id, key_version,
                  version, info.timestamp, current_time,
                  current_time - info.timestamp);
#endif
  unsigned int length= info.length;
  if (with_timeouts && current_time - info.timestamp > cache_max_time)
  {
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  unsigned int max_length = *buflen;
  *buflen = length;
  if (max_length >= length)
  {
    memcpy(data, info.data, length);
  }
  else
  {
#ifndef NDEBUG
    if (max_length)
    {
      my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                      "Encryption key buffer is too small",
                      ME_ERROR_LOG_ONLY | ME_NOTE);
    }
#endif
    return ENCRYPTION_KEY_BUFFER_TOO_SMALL;
  }
  return 0;
}

unsigned int HCData::cache_get_version (unsigned int key_id)
{
  unsigned int version;
  mtx.lock();
#if HASHICORP_HAVE_EXCEPTIONS
  try
  {
    version = latest_version_cache.at(key_id).key_version;
  }
  catch (const std::out_of_range &e)
#else
  VER_MAP::const_iterator ver_iter = latest_version_cache.find(key_id);
  if (ver_iter != latest_version_cache.end())
  {
    version = ver_iter->second.key_version;
  }
  else
#endif
  {
    version = ENCRYPTION_KEY_VERSION_INVALID;
  }
  mtx.unlock();
  return version;
}

unsigned int HCData::cache_check_version (unsigned int key_id)
{
  unsigned int version;
  clock_t timestamp;
  mtx.lock();
#if HASHICORP_HAVE_EXCEPTIONS
  try
  {
    VER_INFO &ver_info = latest_version_cache.at(key_id);
    version = ver_info.key_version;
    timestamp = ver_info.timestamp;
  }
  catch (const std::out_of_range &e)
#else
  VER_MAP::const_iterator ver_iter = latest_version_cache.find(key_id);
  if (ver_iter != latest_version_cache.end())
  {
    version = ver_iter->second.key_version;
    timestamp = ver_iter->second.timestamp;
  }
  else
#endif
  {
    mtx.unlock();
#if HASHICORP_DEBUG_LOGGING
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "cache_check_version: key_id = %u (not in the cache)",
                    ME_ERROR_LOG_ONLY | ME_NOTE,
                    key_id);
#endif
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  mtx.unlock();
  clock_t current_time = clock();
#if HASHICORP_DEBUG_LOGGING
  my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                  "cache_check_version: key_id = %u, "
                  "last version = %u, version timestamp = %u, "
                  "current time = %u, diff = %u",
                  ME_ERROR_LOG_ONLY | ME_NOTE, key_id, version,
                  version, timestamp, current_time,
                  current_time - timestamp);
#endif
  if (current_time - timestamp <= cache_max_ver_time)
  {
    return version;
  }
  else
  {
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
}

static char* vault_url;
static char* token;
static char* vault_ca;
static int timeout;
static int max_retries;
static char caching_enabled;
static char check_kv_version;
static long cache_timeout;
static long cache_version_timeout;
static char use_cache_on_timeout;

static MYSQL_SYSVAR_STR(vault_ca, vault_ca,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path to the Certificate Authority (CA) bundle (is a file "
  "that contains root and intermediate certificates)",
  NULL, NULL, "");

static MYSQL_SYSVAR_STR(vault_url, vault_url,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "HTTP[s] URL that is used to connect to the Hashicorp Vault server",
  NULL, NULL, "");

static MYSQL_SYSVAR_STR(token, token,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY | PLUGIN_VAR_NOSYSVAR,
  "Authentication token that passed to the Hashicorp Vault "
  "in the request header",
  NULL, NULL, "");

static MYSQL_SYSVAR_INT(timeout, timeout,
  PLUGIN_VAR_RQCMDARG,
  "Duration (in seconds) for the Hashicorp Vault server "
  "connection timeout",
  NULL, NULL, 15, 0, 86400, 1);

static MYSQL_SYSVAR_INT(max_retries, max_retries,
  PLUGIN_VAR_RQCMDARG,
  "Number of server request retries in case of timeout",
  NULL, NULL, 3, 0, INT_MAX, 1);

static MYSQL_SYSVAR_BOOL(caching_enabled, caching_enabled,
  PLUGIN_VAR_RQCMDARG,
  "Enable key caching (storing key values received from "
  "the Hashicorp Vault server in the local memory)",
  NULL, NULL, 1);

static MYSQL_SYSVAR_BOOL(check_kv_version, check_kv_version,
  PLUGIN_VAR_RQCMDARG,
  "Enable kv storage version check during plugin initialization",
  NULL, NULL, 1);

static void cache_timeout_update (MYSQL_THD thd,
                                  struct st_mysql_sys_var *var,
                                  void *var_ptr,
                                  const void *save)
{
  long timeout = * (long *) save;
  * (long *) var_ptr = timeout;
  cache_max_time = ms_to_ticks(timeout);
}

static MYSQL_SYSVAR_LONG(cache_timeout, cache_timeout,
  PLUGIN_VAR_RQCMDARG,
  "Cache timeout for key data (in milliseconds)",
  NULL, cache_timeout_update, 60000, 0, LONG_MAX, 1);

static void
  cache_version_timeout_update (MYSQL_THD thd,
                                struct st_mysql_sys_var *var,
                                void *var_ptr,
                                const void *save)
{
  long timeout = * (long *) save;
  * (long *) var_ptr = timeout;
  cache_max_ver_time = ms_to_ticks(timeout);
}

static MYSQL_SYSVAR_LONG(cache_version_timeout, cache_version_timeout,
  PLUGIN_VAR_RQCMDARG,
  "Cache timeout for key version (in milliseconds)",
  NULL, cache_version_timeout_update, 0, 0, LONG_MAX, 1);

static MYSQL_SYSVAR_BOOL(use_cache_on_timeout, use_cache_on_timeout,
  PLUGIN_VAR_RQCMDARG,
  "In case of timeout (when accessing the vault server) "
  "use the value taken from the cache",
  NULL, NULL, 0);

static struct st_mysql_sys_var *settings[] = {
  MYSQL_SYSVAR(vault_url),
  MYSQL_SYSVAR(token),
  MYSQL_SYSVAR(vault_ca),
  MYSQL_SYSVAR(timeout),
  MYSQL_SYSVAR(max_retries),
  MYSQL_SYSVAR(caching_enabled),
  MYSQL_SYSVAR(cache_timeout),
  MYSQL_SYSVAR(cache_version_timeout),
  MYSQL_SYSVAR(use_cache_on_timeout),
  MYSQL_SYSVAR(check_kv_version),
  NULL
};

/*
  Reasonable length limit to protect against accidentally reading
  the wrong key or from trying to overload the server with unnecessary
  work to receive too long responses to requests:
*/
#define MAX_RESPONSE_SIZE 131072

static size_t write_response_memory (void *contents, size_t size, size_t nmemb,
                                     void *userp)
{
  size_t realsize = size * nmemb;
  std::ostringstream *read_data = static_cast<std::ostringstream *>(userp);
  size_t current_length = read_data->tellp();
  if (current_length + realsize > MAX_RESPONSE_SIZE)
    return 0; // response size limit exceeded
  read_data->write(static_cast<char *>(contents), realsize);
  if (!read_data->good())
    return 0;
  return realsize;
}

enum {
   OPERATION_OK,
   OPERATION_TIMEOUT,
   OPERATION_ERROR
};

static CURLcode
  perform_with_retries (CURL *curl, std::ostringstream *read_data_stream)
{
  int retries= max_retries;
  CURLcode curl_res;
  do {
    curl_res= curl_easy_perform(curl);
    if (curl_res != CURLE_OPERATION_TIMEDOUT)
    {
      break;
    }
    read_data_stream->clear();
    read_data_stream->str("");
  } while (retries--);
  return curl_res;
}

int HCData::curl_run (const char *url, std::string *response,
                      bool soft_timeout) const
{
  char curl_errbuf[CURL_ERROR_SIZE];
  std::ostringstream read_data_stream;
  long http_code = 0;
  CURLcode curl_res = CURLE_OK;
  CURL *curl = curl_easy_init();
  if (curl == NULL)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Cannot initialize curl session",
                    ME_ERROR_LOG_ONLY);
    return OPERATION_ERROR;
  }
  curl_errbuf[0] = '\0';
  if ((curl_res= curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_errbuf)) !=
          CURLE_OK ||
      (curl_res= curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                                  write_response_memory)) != CURLE_OK ||
      (curl_res= curl_easy_setopt(curl, CURLOPT_WRITEDATA,
                                  &read_data_stream)) !=
          CURLE_OK ||
      (curl_res= curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist)) !=
          CURLE_OK ||
      /*
        The options CURLOPT_SSL_VERIFYPEER and CURLOPT_SSL_VERIFYHOST are
        set explicitly to withstand possible future changes in curl defaults:
      */
      (curl_res= curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1)) !=
          CURLE_OK ||
      (curl_res= curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L)) !=
          CURLE_OK ||
      (strlen(vault_ca) != 0 &&
       (curl_res= curl_easy_setopt(curl, CURLOPT_CAINFO, vault_ca)) !=
           CURLE_OK) ||
      (curl_res= curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL)) !=
          CURLE_OK ||
      (curl_res= curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L)) !=
          CURLE_OK ||
      (timeout &&
       ((curl_res= curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout)) !=
            CURLE_OK ||
        (curl_res= curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout)) !=
            CURLE_OK)) ||
      (curl_res = curl_easy_setopt(curl, CURLOPT_URL, url)) != CURLE_OK ||
      (curl_res = perform_with_retries(curl, &read_data_stream)) != CURLE_OK ||
      (curl_res = curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE,
                                     &http_code)) != CURLE_OK)
  {
    curl_easy_cleanup(curl);
    if (soft_timeout && curl_res == CURLE_OPERATION_TIMEDOUT)
    {
      return OPERATION_TIMEOUT;
    }
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "curl returned this error code: %u "
                    "with the following error message: %s", 0, curl_res,
                    curl_errbuf[0] ? curl_errbuf :
                                     curl_easy_strerror(curl_res));
    return OPERATION_ERROR;
  }
  curl_easy_cleanup(curl);
  *response = read_data_stream.str();
  bool is_error = http_code < 200 || http_code >= 300;
  if (is_error)
  {
    const char *res = response->c_str();
    /*
      Error 404 requires special handling - in case the server
      returned an empty array of error strings (the value of the
      "error" object in JSON is equal to an empty array), we should
      ignore this error at this level, since this means the missing
      key (this problem is handled at a higher level), but if the
      error object contains anything other than empty array, then
      we need to print the error message to the log:
    */
    if (http_code == 404)
    {
      const char *err;
      int err_len;
      if (json_get_object_key(res, res + response->size(),
                              "errors", &err, &err_len) == JSV_ARRAY)
      {
        const char *ev;
        int ev_len;
        if (json_get_array_item(err, err + err_len, 0, &ev, &ev_len) ==
            JSV_NOTHING)
        {
          *response = std::string("");
          is_error = false;
        }
      }
    }
    if (is_error)
    {
      my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                      "Hashicorp server error: %d, response: %s",
                      ME_ERROR_LOG_ONLY | ME_WARNING, http_code, res);
    }
  }
  return is_error ? OPERATION_ERROR : OPERATION_OK;
}

static inline int c2xdigit (int c)
{
  if (c > 9)
  {
    c -= 'A' - '0' - 10;
    if (c > 15)
    {
      c -= 'a' - 'A';
    }
  }
  return c;
}

static int hex2buf (unsigned int max_length, unsigned char *dstbuf,
                    int key_len, const char *key)
{
  int length = 0;
  while (key_len >= 2)
  {
    int c1 = key[0];
    int c2 = key[1];
    if (! isxdigit(c1) || ! isxdigit(c2))
    {
      break;
    }
    if (max_length)
    {
      c1 = c2xdigit(c1 - '0');
      c2 = c2xdigit(c2 - '0');
      dstbuf[length++] = (c1 << 4) + c2;
    }
    key += 2;
    key_len -= 2;
  }
  if (key_len)
  {
    if (key_len != 1)
    {
      my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                      "Syntax error - the key data should contain only "
                      "hexadecimal digits",
                      0);
    }
    else
    {
      my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                      "Syntax error - extra character in the key data",
                      0);
    }
    return -1;
  }
  return 0;
}

static int get_data (const std::string &response_str,
                     const char **js, int *js_len,
                     unsigned int key_id,
                     unsigned int key_version)
{
  const char *response = response_str.c_str();
  size_t response_len = response_str.size();
  /*
    If the key is not found, this is not considered a fatal error,
    but we need to add an informational message to the log:
  */
  if (response_len == 0)
  {
    if (key_version == ENCRYPTION_KEY_VERSION_INVALID)
    {
      my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                      "Key not found (key id: %u)",
                      ME_ERROR_LOG_ONLY | ME_NOTE, key_id);
    }
    else
    {
      my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                      "Key not found (key id: %u, key version: %u)",
                      ME_ERROR_LOG_ONLY | ME_NOTE, key_id, key_version);
    }
    return 1;
  }
  if (json_get_object_key(response, response + response_len, "data",
                          js, js_len) != JSV_OBJECT)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get data object (http response is: %s)",
                    0, response);
    return 2;
  }
  return 0;
}

static unsigned int get_version (const char *js, int js_len,
                                 const std::string &response_str,
                                 int *rc)
{
  const char *ver;
  int ver_len;
  *rc = 1;
  if (json_get_object_key(js, js + js_len, "metadata",
                          &ver, &ver_len) != JSV_OBJECT)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get metadata object (http response is: %s)",
                    0, response_str.c_str());
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  if (json_get_object_key(ver, ver + ver_len, "version",
                          &ver, &ver_len) != JSV_NUMBER)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get version number (http response is: %s)",
                    0, response_str.c_str());
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  errno = 0;
  unsigned long version = strtoul(ver, NULL, 10);
  if (version > UINT_MAX || (version == ULONG_MAX && errno))
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Integer conversion error (for version number) "
                    "(http response is: %s)",
                    0, response_str.c_str());
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  *rc = 0;
  return (unsigned int) version;
}

static int get_key_data (const char *js, int js_len,
                         const char **key, int *key_len,
                         const std::string &response_str)
{
  if (json_get_object_key(js, js + js_len, "data",
                          &js, &js_len) != JSV_OBJECT)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get second-level data object "
                    "(http response is: %s)",
                    0, response_str.c_str());
    return 1;
  }
  if (json_get_object_key(js, js + js_len, "data",
                          key, key_len) != JSV_STRING)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get data string (http response is: %s)",
                    0, response_str.c_str());
    return 1;
  }
  return 0;
}

unsigned int HCData::get_latest_version (unsigned int key_id)
{
  unsigned int version;
#if HASHICORP_DEBUG_LOGGING
  my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                  "get_latest_version: key_id = %u",
                  ME_ERROR_LOG_ONLY | ME_NOTE, key_id);
#endif
  if (caching_enabled)
  {
    version = cache_check_version(key_id);
    if (version != ENCRYPTION_KEY_VERSION_INVALID)
    {
      return version;
    }
  }
  std::string response_str;
  /*
    Maximum buffer length = URL length plus 20 characters of
    a 64-bit unsigned integer, plus a slash character, plus
    a length of the "/data/" string and plus a zero byte:
  */
  size_t buf_len = vault_url_len + (20 + 6 + 1);
  char *url = (char *) alloca(buf_len);
  snprintf(url, buf_len, "%s%u", vault_url_data, key_id);
  bool use_cache= caching_enabled && use_cache_on_timeout;
  int rc;
  if ((rc= curl_run(url, &response_str, use_cache)) != OPERATION_OK)
  {
    if (rc == OPERATION_TIMEOUT)
    {
      version = cache_get_version(key_id);
      if (version != ENCRYPTION_KEY_VERSION_INVALID)
      {
        return version;
      }
    }
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get key data", 0);
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  const char *js;
  int js_len;
  if (get_data(response_str, &js, &js_len, key_id,
               ENCRYPTION_KEY_VERSION_INVALID))
  {
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  version = get_version(js, js_len, response_str, &rc);
  if (!caching_enabled || rc)
  {
    return version;
  }
  const char* key;
  int key_len;
  if (get_key_data(js, js_len, &key, &key_len, response_str))
  {
     return ENCRYPTION_KEY_VERSION_INVALID;
  }
  unsigned int length = (unsigned int) key_len >> 1;
  KEY_INFO info(key_id, version, clock(), length);
  if (length > sizeof(info.data))
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Encryption key data is too long",
                    ME_ERROR_LOG_ONLY | ME_NOTE);
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  int ret = hex2buf(sizeof(info.data), info.data, key_len, key);
  if (ret)
  {
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  cache_add(info, true);
  return version;
}

unsigned int HCData::get_key_from_vault (unsigned int key_id,
                                         unsigned int key_version,
                                         unsigned char *dstbuf,
                                         unsigned int *buflen)
{
#if HASHICORP_DEBUG_LOGGING
  my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                  "get_latest_version: key_id = %u, key_version = %u",
                  ME_ERROR_LOG_ONLY | ME_NOTE, key_id, key_version);
#endif
  if (caching_enabled &&
      cache_get(key_id, key_version, dstbuf, buflen, true) !=
      ENCRYPTION_KEY_VERSION_INVALID)
  {
    return 0;
  }
  std::string response_str;
  /*
    Maximum buffer length = URL length plus 40 characters of the
    two 64-bit unsigned integers, plus a slash character, plus a
    question mark, plus length of the "/data/" and the "?version="
    strings and plus a zero byte:
  */
  size_t buf_len = vault_url_len + (40 + 6 + 9 + 1);
  char *url = (char *) alloca(buf_len);
  if (key_version != ENCRYPTION_KEY_VERSION_INVALID)
    snprintf(url, buf_len, "%s%u?version=%u",
             vault_url_data, key_id, key_version);
  else
    snprintf(url, buf_len, "%s%u", vault_url_data, key_id);
  bool use_cache= caching_enabled && use_cache_on_timeout;
  int rc;
  if ((rc= curl_run(url, &response_str, use_cache)) != OPERATION_OK)
  {
    if (rc == OPERATION_TIMEOUT)
    {
      if (cache_get(key_id, key_version, dstbuf, buflen, false) !=
          ENCRYPTION_KEY_VERSION_INVALID)
      {
        return 0;
      }
    }
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get key data", 0);
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  const char *js;
  int js_len;
  if (get_data(response_str, &js, &js_len, key_id, key_version))
  {
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
#ifndef NDEBUG
  unsigned long version;
#else
  unsigned long version= key_version;
  if (caching_enabled &&
      key_version == ENCRYPTION_KEY_VERSION_INVALID)
#endif
  {
    int rc;
    version = get_version(js, js_len, response_str, &rc);
    if (rc)
    {
      return version;
    }
  }
#ifndef NDEBUG
  /*
    An internal check that is needed only for debugging the plugin
    operation - in order to ensure that we get from the Hashicorp Vault
    server exactly the version of the key that is needed:
  */
  if (key_version != ENCRYPTION_KEY_VERSION_INVALID &&
      key_version != version)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Key version mismatch", 0);
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
#endif
  const char* key;
  int key_len;
  if (get_key_data(js, js_len, &key, &key_len, response_str))
  {
     return ENCRYPTION_KEY_VERSION_INVALID;
  }
  unsigned int max_length = dstbuf ? *buflen : 0;
  unsigned int length = (unsigned int) key_len >> 1;
  *buflen = length;
  if (length > max_length)
  {
#ifndef NDEBUG
    if (max_length)
    {
      my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                      "Encryption key buffer is too small",
                      ME_ERROR_LOG_ONLY | ME_NOTE);
    }
#endif
    return ENCRYPTION_KEY_BUFFER_TOO_SMALL;
  }
  int ret = hex2buf(max_length, dstbuf, key_len, key);
  if (ret)
  {
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  if (caching_enabled)
  {
    KEY_INFO info(key_id, (unsigned int) version, clock(), length);
    memcpy(info.data, dstbuf, length);
    cache_add(info, key_version == ENCRYPTION_KEY_VERSION_INVALID);
  }
  return 0;
}

static unsigned int get_latest_version (unsigned int key_id)
{
  return data.get_latest_version(key_id);
}

static unsigned int get_key_from_vault (unsigned int key_id,
                                        unsigned int key_version,
                                        unsigned char *dstbuf,
                                        unsigned int *buflen)
{
  return data.get_key_from_vault(key_id, key_version, dstbuf, buflen);
}

struct st_mariadb_encryption hashicorp_key_management_plugin= {
  MariaDB_ENCRYPTION_INTERFACE_VERSION,
  get_latest_version,
  get_key_from_vault,
  0, 0, 0, 0, 0
};

#ifdef _MSC_VER

static int setenv (const char *name, const char *value, int overwrite)
{
  if (!overwrite)
  {
    size_t len= 0;
    int rc= getenv_s(&len, NULL, 0, name);
    if (rc)
    {
      return rc;
    }
    if (len)
    {
      errno = EINVAL;
      return EINVAL;
    }
  }
  return _putenv_s(name, value);
}

#endif

#define MAX_URL_SIZE 32768

int HCData::init ()
{
  const static char *x_vault_token = "X-Vault-Token:";
  const static size_t x_vault_token_len = strlen(x_vault_token);
  char *token_env= getenv("VAULT_TOKEN");
  size_t token_len = strlen(token);
  if (token_len == 0)
  {
    if (token_env)
    {
      token_len = strlen(token_env);
      if (token_len != 0)
      {
        /*
          The value of the token parameter obtained using the getenv()
          system call, which does not guarantee that the memory pointed
          to by the returned pointer can be read in the long term (for
          example, after changing the values of the environment variables
          of the current process). Therefore, we need to copy the token
          value to the working buffer:
        */
        if (!(token = (char *) alloc(token_len + 1)))
        {
          return 1;
        }
        memcpy(token, token_env, token_len + 1);
        local_token = token;
      }
    }
    if (token_len == 0) {
      my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                      "The --hashicorp-key-management-token option value "
                      "or the value of the corresponding parameter in the "
                      "configuration file must be specified, otherwise the "
                      "VAULT_TOKEN environment variable must be set",
                      0);
      return 1;
    }
  }
  else
  {
    /*
      If the VAULT_TOKEN environment variable is not set or
      is not equal to the value of the token parameter, then
      we must set (overwrite) it for correct operation of
      the mariabackup:
    */
    bool not_equal= token_env != NULL && strcmp(token_env, token) != 0;
    if (token_env == NULL || not_equal)
    {
      setenv("VAULT_TOKEN", token, 1);
      if (not_equal)
      {
        my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                        "The --hashicorp-key-management-token option value "
                        "or the value of the corresponding parameter is not "
                        "equal to the value of the VAULT_TOKEN environment "
                        "variable",
                        ME_ERROR_LOG_ONLY | ME_WARNING);
      }
    }
  }
#if HASHICORP_DEBUG_LOGGING
  my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                  "plugin_init: token = %s, token_len = %d",
                  ME_ERROR_LOG_ONLY | ME_NOTE, token, (int) token_len);
#endif
  size_t buf_len = x_vault_token_len + token_len + 1;
  if (!(token_header = (char *) alloc(buf_len)))
  {
    return 1;
  }
  snprintf(token_header, buf_len, "%s%s", x_vault_token, token);
  /* We need to check that the path inside the URL starts with "/v1/": */
  const char *suffix = strchr(vault_url, '/');
  if (suffix == NULL)
  {
Bad_url:
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "According to the Hashicorp Vault API rules, "
                    "the path inside the URL must start with "
                    "the \"/v1/\" prefix, while the supplied "
                    "URL value is: \"%s\"", 0, vault_url);
    return 1;
  }
  size_t prefix_len = (size_t) (suffix - vault_url);
  if (prefix_len == 0)
  {
No_Host:
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Supplied URL does not contain a hostname: \"%s\"",
                    0, vault_url);
    return 1;
  }
  /* Check if the suffix consists only of the slash: */
  size_t suffix_len = strlen(suffix + 1) + 1;
  if (suffix_len == 1)
  {
    goto Bad_url;
  }
  vault_url_len = prefix_len + suffix_len;
  /*
    The scheme always ends with "://", while the "suffix"
    points to the first of the slashes:
  */
  if (*(suffix - 1) == ':' && suffix[1] == '/')
  {
    /* Let's check that only the schema is present: */
    if (suffix_len == 2)
    {
      goto No_Host;
    }
    /* Save the current position: */
    const char *start = suffix + 2;
    /* We need to find next slash: */
    suffix = strchr(start, '/');
    if (suffix == NULL)
    {
      goto Bad_url;
    }
    /* Update the prefix and suffix lengths: */
    prefix_len = (size_t) (suffix - vault_url);
    suffix_len = vault_url_len - prefix_len;
    /*
      The slash right after the scheme is the absence of a hostname,
      this is invalid for all schemes, except for the "file://"
      (this allowed for debugging purposes only):
    */
    if (suffix == start &&
        (prefix_len != 7 || memcmp(vault_url, "file", 4) != 0))
    {
      goto No_Host;
    }
    /* Check if the suffix consists only of the slash: */
    if (suffix_len == 1)
    {
      goto Bad_url;
    }
  }
  /* Let's skip all leading slashes: */
  while (suffix[1] == '/')
  {
    suffix++;
    suffix_len--;
    if (suffix_len == 1)
    {
      goto Bad_url;
    }
  }
  /*
    Checking for "/v1" sequence (the leading slash has
    already been checked):
  */
  if (suffix_len < 3 || suffix[1] != 'v' || suffix[2] != '1')
  {
    goto Bad_url;
  }
  /* Let's skip the "/v1" sequence: */
  suffix_len -= 3;
  if (suffix_len == 0)
  {
No_Secret:
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Supplied URL does not contain a secret name: \"%s\"",
                    0, vault_url);
    return 1;
  }
  suffix += 3;
  /* Checking for a slash at the end of the "/v1/" sequence: */
  if (suffix[0] != '/')
  {
    goto Bad_url;
  }
  /* Skip slashes after the "/v1" sequence: */
  do
  {
    suffix++;
    suffix_len--;
    if (suffix_len == 0)
    {
      goto No_Secret;
    }
  } while (suffix[0] == '/');
  /* Remove trailing slashes at the end of the url: */
  while (vault_url[vault_url_len - 1] == '/')
  {
    vault_url_len--;
    suffix_len--;
  }
  /*
    Checking the maximum allowable length to protect
    against allocating too much memory on the stack:
  */
  if (vault_url_len > MAX_URL_SIZE)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Maximum allowed vault URL length exceeded", 0);
    return 1;
  }
  /*
    In advance, we create a buffer containing the URL for vault
    + the "/data/" suffix (7 characters):
  */
  if (!(vault_url_data = (char *) alloc(vault_url_len + 7)))
  {
    return 1;
  }
  memcpy(vault_url_data, vault_url, vault_url_len);
  memcpy(vault_url_data + vault_url_len, "/data/", 7);
  cache_max_time = ms_to_ticks(cache_timeout);
  cache_max_ver_time = ms_to_ticks(cache_version_timeout);
  /* Initialize curl: */
  CURLcode curl_res = curl_global_init(CURL_GLOBAL_ALL);
  if (curl_res != CURLE_OK)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "unable to initialize curl library, "
                    "curl returned this error code: %u "
                    "with the following error message: %s",
                    0, curl_res, curl_easy_strerror(curl_res));
    return 1;
  }
  curl_inited = true;
  slist = curl_slist_append(slist, token_header);
  if (slist == NULL)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "curl: unable to construct slist", 0);
    return 1;
  }
  /*
    If we do not need to check the key-value storage version,
    then we immediately return from this function:
  */
  if (check_kv_version == 0) {
    return 0;
  }
  /*
    Let's construct a URL to check the version of the key-value storage:
  */
  char *mount_url = (char *) alloc(vault_url_len + 11 + 6);
  if (mount_url == NULL)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Memory allocation error", 0);
    return 1;
  }
  /*
    The prefix length must be recalculated, as it may have
    changed in the process of discarding trailing slashes:
  */
  prefix_len = vault_url_len - suffix_len;
  memcpy(mount_url, vault_url_data, prefix_len);
  memcpy(mount_url + prefix_len, "sys/mounts/", 11);
  memcpy(mount_url + prefix_len + 11, vault_url_data + prefix_len, suffix_len);
  memcpy(mount_url + prefix_len + 11 + suffix_len, "/tune", 6);
#if HASHICORP_DEBUG_LOGGING
  my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                  "storage mount url: [%s]",
                  ME_ERROR_LOG_ONLY | ME_NOTE, mount_url);
#endif
  int rc = check_version(mount_url);
  free(mount_url);
  return rc;
}

int HCData::check_version (const char *mount_url) const
{
  std::string response_str;
  int rc = curl_run(mount_url, &response_str, false);
  if (rc != OPERATION_OK)
  {
storage_error:
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get storage options for \"%s\"",
                    0, mount_url);
    return 1;
  }
  const char *response = response_str.c_str();
  size_t response_len = response_str.size();
  /*
    If the key is not found, this is not considered a fatal error,
    but we need to add an informational message to the log:
  */
  if (response_len == 0)
  {
    goto storage_error;
  }
  const char *js;
  int js_len;
  if (json_get_object_key(response, response + response_len, "options",
                          &js, &js_len) != JSV_OBJECT)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get storage options (http response is: %s)",
                    0, response);
    return 1;
  }
  const char *ver;
  int ver_len;
  enum json_types jst =
    json_get_object_key(js, js + js_len, "version", &ver, &ver_len);
  if (jst != JSV_STRING && jst != JSV_NUMBER)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get storage version (http response is: %s)",
                    0, response);
    return 1;
  }
  unsigned long version = strtoul(ver, NULL, 10);
  if (version > UINT_MAX || (version == ULONG_MAX && errno))
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Integer conversion error (for version number) "
                    "(http response is: %s)", 0, response);
    return 1;
  }
  if (version < 2)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Key-value storage must be version "
                    "number 2 or later", 0);
    return 1;
  }
  return 0;
}

static int hashicorp_key_management_plugin_init(void *p)
{
  int rc = data.init();
  if (rc)
  {
    data.deinit();
  }
  return rc;
}

static int hashicorp_key_management_plugin_deinit(void *p)
{
  data.cache_clean();
  data.deinit();
  return 0;
}

/*
  Plugin library descriptor
*/
maria_declare_plugin(hashicorp_key_management)
{
  MariaDB_ENCRYPTION_PLUGIN,
  &hashicorp_key_management_plugin,
  "hashicorp_key_management",
  "MariaDB Corporation",
  "HashiCorp Vault key management plugin",
  PLUGIN_LICENSE_GPL,
  hashicorp_key_management_plugin_init,
  hashicorp_key_management_plugin_deinit,
  0x0200 /* 2.0 */,
  NULL, /* status variables */
  settings,
  "2.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;

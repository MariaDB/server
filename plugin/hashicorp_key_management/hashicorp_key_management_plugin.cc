/* Copyright (C) 2019, 2020 MariaDB Corporation

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
#include <errno.h>
#include <string>
#include <sstream>
#include <curl/curl.h>
#ifdef _WIN32
#include <malloc.h>
#define alloca _alloca
#else
#include <alloca.h>
#endif
#include <algorithm>
#include <unordered_map>
#include <mutex>

#define PLUGIN_ERROR_HEADER "hashicorp: "

static std::mutex mtx;

/* Key information structure: */
typedef struct KEY_INFO
{
  unsigned int key_id;
  unsigned int key_version;
  unsigned int length;
  unsigned char data [MY_AES_MAX_KEY_LENGTH];
  KEY_INFO() : key_id(0), key_version(0), length(0) {};
} KEY_INFO;

/* Cache for the latest version, per key id: */
static std::unordered_map<unsigned int, unsigned int> latest_version_cache;

/* Cache for key information: */
static std::unordered_map<unsigned long long, KEY_INFO> key_info_cache;

#define KEY_ID_AND_VERSION(key_id, version) \
  ((unsigned long long)key_id << 32 | version)

static void cache_add (unsigned int key_id, unsigned int key_version,
                       unsigned int length, KEY_INFO* info)
{
  info->key_id = key_id;
  info->key_version = key_version;
  info->length = length;
  mtx.lock();
  latest_version_cache[key_id]=
    std::max(latest_version_cache[key_id], key_version);
  key_info_cache[KEY_ID_AND_VERSION(key_id, key_version)]= *info;
  mtx.unlock();
}

static unsigned int
  cache_get (unsigned int key_id, unsigned int key_version,
             unsigned char* data, unsigned int* buflen)
{
  unsigned int ver= key_version;
  mtx.lock();
  if (key_version == ENCRYPTION_KEY_VERSION_INVALID)
  {
    ver= latest_version_cache[key_id];
    if (ver == 0)
    {
      mtx.unlock();
      return ENCRYPTION_KEY_VERSION_INVALID;
    }
  }
  KEY_INFO info= key_info_cache[KEY_ID_AND_VERSION(key_id, ver)];
  mtx.unlock();
  unsigned int length= info.length;
  if (length == 0)
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

static unsigned int cache_get_version (unsigned int key_id)
{
  unsigned int ver;
  mtx.lock();
  ver= latest_version_cache[key_id];
  mtx.unlock();
  if (ver)
  {
    return ver;
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
  MYSQL_SYSVAR(use_cache_on_timeout),
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

static struct curl_slist *list;

static int curl_run (char *url, std::string *response, bool soft_timeout)
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
      (curl_res= curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list)) !=
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
                    "CURL returned this error code: %u "
                    " with error message: %s", 0, curl_res,
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
      if (json_get_object_key(res, res + strlen(res),
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
    c -= 'A' - '0';
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

static const char * get_data (const std::string response_str,
                              const char **js, int *js_len)
{
  const char *response = response_str.c_str();
  size_t response_len = response_str.size();
  /*
    If the key is not found, this is not considered a fatal error,
    but we need to add an informational message to the log:
  */
  if (response_len == 0)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Key not found",
                    ME_ERROR_LOG_ONLY | ME_NOTE);
    return NULL;
  }
  if (json_get_object_key(response, response + response_len, "data",
                          js, js_len) != JSV_OBJECT)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get data object (http response is: %s)",
                    0, response);
    return NULL;
  }
  return response;
}

static unsigned int get_version (const char *js, int js_len,
                                 const char *response, int *rc)
{
  const char *ver;
  int ver_len;
  *rc = 1;
  if (json_get_object_key(js, js + js_len, "metadata",
                          &ver, &ver_len) != JSV_OBJECT)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get metadata object (http response is: %s)",
                    0, response);
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  if (json_get_object_key(ver, ver + ver_len, "version",
                          &ver, &ver_len) != JSV_NUMBER)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get version number (http response is: %s)",
                    0, response);
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  errno = 0;
  unsigned long version = strtoul(ver, NULL, 10);
  if (version > UINT_MAX || (version == ULONG_MAX && errno))
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Integer conversion error (for version number)"
                    "(http response is: %s)",
                    0, response);
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  if (version > UINT_MAX || (version == ULONG_MAX && errno))
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Integer conversion error (for version number)"
                    "(http response is: %s)",
                    0, response);
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  *rc = 0;
  return (unsigned int) version;
}

static int get_key_data (const char *js, int js_len,
                         const char **key, int *key_len,
                         const char *response)
{
  if (json_get_object_key(js, js + js_len, "data",
                          &js, &js_len) != JSV_OBJECT)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get second-level data object "
                    "(http response is: %s)",
                    0, response);
    return 1;
  }
  if (json_get_object_key(js, js + js_len, "data",
                          key, key_len) != JSV_STRING)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get data string (http response is: %s)",
                    0, response);
    return 1;
  }
  return 0;
}

static char* vault_url_data;
static size_t vault_url_len;

static unsigned int get_latest_version (unsigned int key_id)
{
  std::string response_str;
  const char *response;
  const char *js;
  int js_len;
  /*
    Maximum buffer length = url length plus 20 characters of
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
      unsigned int ver = cache_get_version(key_id);
      if (ver != ENCRYPTION_KEY_VERSION_INVALID)
      {
        return ver;
      }
    }
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get key data", 0);
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  response = get_data(response_str, &js, &js_len);
  if (response == NULL)
  {
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  unsigned int version = get_version(js, js_len, response, &rc);
  if (!caching_enabled || rc)
  {
    return version;
  }
  const char* key;
  int key_len;
  if (get_key_data(js, js_len, &key, &key_len, response))
  {
     return ENCRYPTION_KEY_VERSION_INVALID;
  }
  KEY_INFO info;
  unsigned int length = (unsigned int) key_len >> 1;
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
  cache_add(key_id, version, length, &info);
  return version;
}

static unsigned int get_key_from_vault (unsigned int key_id,
                                        unsigned int key_version,
                                        unsigned char *dstbuf,
                                        unsigned int *buflen)
{
  if (caching_enabled &&
      cache_get(key_id, key_version, dstbuf, buflen) !=
      ENCRYPTION_KEY_VERSION_INVALID)
  {
    return 0;
  }
  std::string response_str;
  const char *response;
  const char *js;
  int js_len;
  /*
    Maximum buffer length = url length plus 40 characters of the
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
  if (curl_run(url, &response_str, false) != OPERATION_OK)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Unable to get key data", 0);
    return ENCRYPTION_KEY_VERSION_INVALID;
  }
  response = get_data(response_str, &js, &js_len);
  if (response == NULL)
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
    version = get_version(js, js_len, response, &rc);
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
  if (get_key_data(js, js_len, &key, &key_len, response))
  {
     return ENCRYPTION_KEY_VERSION_INVALID;
  }
  unsigned int max_length = *buflen;
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
    KEY_INFO info;
    memcpy(info.data, dstbuf, length);
    cache_add(key_id, (unsigned int) version, length, &info);
  }
  return 0;
}

struct st_mariadb_encryption hashicorp_key_management_plugin= {
  MariaDB_ENCRYPTION_INTERFACE_VERSION,
  get_latest_version,
  get_key_from_vault,
  0, 0, 0, 0, 0
};

#ifdef _MSC_VER

static int setenv(const char *name, const char *value, int overwrite)
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

static char *local_token= NULL;
static char *token_header= NULL;

static int hashicorp_key_management_plugin_init(void *p)
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
        token = (char *) malloc(token_len + 1);
        if (token == NULL)
        {
          my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                          "Memory allocation error", 0);
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
  size_t buf_len = x_vault_token_len + token_len + 1;
  token_header = (char *) malloc(buf_len);
  if (token_header == NULL)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Memory allocation error", 0);
    return 1;
  }
  snprintf(token_header, buf_len, "%s%s", x_vault_token, token);
  curl_global_init(CURL_GLOBAL_ALL);
  list = curl_slist_append(list, token_header);
  if (list == NULL)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "curl: unable to construct slist", 0);
  }
  vault_url_len = strlen(vault_url);
  /*
    Checking the maximum allowable length to protect
    against allocating too much memory on the stack:
  */
  if (vault_url_len > MAX_URL_SIZE)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Maximum allowed vault URL length exceeded",
                    0);
    return 1;
  }
  if (vault_url_len && vault_url[vault_url_len - 1] == '/')
  {
    vault_url_len--;
  }
  /*
    In advance, we create a buffer containing the URL for vault
    + the "/data/" suffix (7 characters):
  */
  vault_url_data = (char *) malloc(vault_url_len + 7);
  if (vault_url_data == NULL)
  {
    my_printf_error(ER_UNKNOWN_ERROR, PLUGIN_ERROR_HEADER
                    "Memory allocation error", 0);
    return 1;
  }
  memcpy(vault_url_data, vault_url, vault_url_len);
  memcpy(vault_url_data + vault_url_len, "/data/", 7);
  return 0;
}

static int hashicorp_key_management_plugin_deinit(void *p)
{
  if (list)
  {
    curl_slist_free_all(list);
  }
  latest_version_cache.clear();
  key_info_cache.clear();
  curl_global_cleanup();
  if (vault_url_data)
  {
    free(vault_url_data);
  }
  if (token_header)
  {
    free(token_header);
  }
  if (local_token)
  {
    free(local_token);
  }
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
  0x0103 /* 1.03 */,
  NULL, /* status variables */
  settings,
  "1.03",
  MariaDB_PLUGIN_MATURITY_BETA
}
maria_declare_plugin_end;

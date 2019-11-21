/* Copyright (C) 2014 eperi GmbH.
   Copyright (C) 2015 MariaDB Corporation

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

/******************************************************************//**
 @file Parser.cc
 A class to parse the key file

How it works...
The location and usage can be configured via the configuration file.
Example

[mysqld]
...
file_key_management_filename = /home/mdb/keys.enc
file_key_management_filekey = secret
...

The keys are read from a file.
The filename is set up via the file_key_management_filename
configuration value.
file_key_management_filename is used to configure the absolute
path to this file.

Examples:
file_key_management_filename = \\\\unc\\keys.enc        (windows share)
file_key_management_filename = e:/tmp/keys.enc          (windows path)
file_key_management_filename = /tmp/keys.enc            (linux path)

The key file contains AES keys as hex-encoded strings.
Supported are keys of size 128, 192 or 256 bits.
Example:
1;F5502320F8429037B8DAEF761B189D12
2;770A8A65DA156D24EE2A093277530142770A8A65DA156D24EE2A093277530142

1 is the key identifier which can be used for table creation,
it is followed by a AES key

The key file could be encrypted and the key to decrypt the file can
be given with the optional file_key_management_filekey
parameter.

The file key can also be located if FILE: is prepended to the
key. Then the following part is interpreted as absolute path to the
file containing the file key (which must be a text - not binary - string).

Example:

file_key_management_filekey = FILE:y:/secret256.enc

If the key file can not be read at server startup, for example if the
file key is not present, the plugin will not start
access to encrypted tables will not be possible.

Open SSL command line utility can be used to create an encrypted key file.
Example:
openssl enc -aes-256-cbc -md sha1 -k "secret" -in keys.txt -out keys.enc
***********************************************************************/

#include "parser.h"
#include <m_string.h>
#include <mysys_err.h>

#define FILE_PREFIX "FILE:"
#define MAX_KEY_FILE_SIZE 1024*1024
#define MAX_SECRET_SIZE 256

/*
  The values below are what one gets after
  openssl enc -aes-256-cbc -md sha1 -k "secret" -in keys.txt -out keys.enc
*/
#define OpenSSL_prefix     "Salted__"
#define OpenSSL_prefix_len (sizeof(OpenSSL_prefix) - 1)
#define OpenSSL_salt_len   8
#define OpenSSL_key_len    32
#define OpenSSL_iv_len     16

/**
   Calculate key and iv from a given salt and secret as in the
   openssl command-line tool

   @param salt   [in]  the given salt as extracted from the encrypted file
   @param secret [in]  the given secret as String, provided by the user
   @param key    [out] 32 Bytes of key are written to this pointer
   @param iv     [out] 16 Bytes of iv are written to this pointer

   Note, that in openssl this whole function can be reduced to

    #include <openssl/evp.h>
    EVP_BytesToKey(EVP_aes_256_cbc(), EVP_sha1(), salt,
                   secret, strlen(secret), 1, key, iv);

   but alas! we want to support yassl too
*/

void Parser::bytes_to_key(const unsigned char *salt, const char *input,
                          unsigned char *key, unsigned char *iv)
{
  unsigned char digest[MY_SHA1_HASH_SIZE];
  int key_left   = OpenSSL_key_len;
  int iv_left    = OpenSSL_iv_len;
  const size_t ilen= strlen(input);
  const size_t slen= OpenSSL_salt_len; // either this or explicit (size_t) casts below

  my_sha1_multi(digest, input, ilen, salt, slen, NullS);

  while (iv_left)
  {
    int left= MY_SHA1_HASH_SIZE;
    if (key_left)
    {
      int store = MY_MIN(key_left, MY_SHA1_HASH_SIZE);
      memcpy(&key[OpenSSL_key_len - key_left], digest, store);

      key_left -= store;
      left     -= store;
    }

    if (iv_left && left)
    {
      int store= MY_MIN(iv_left, left);
      memcpy(&iv[OpenSSL_iv_len - iv_left], &digest[MY_SHA1_HASH_SIZE - left], store);

      iv_left    -= store;
    }

    if (iv_left)
      my_sha1_multi(digest, digest, MY_SHA1_HASH_SIZE,
                            input, ilen, salt, slen, NullS);
  }
}


bool Parser::parse(std::map<uint,keyentry> *keys)
{
  const char *secret= filekey;
  char buf[MAX_SECRET_SIZE + 1];

  //If secret starts with FILE: interpret the secret as a filename.
  if (strncmp(filekey, FILE_PREFIX,sizeof(FILE_PREFIX) -1) == 0)
  {
    if (read_filekey(filekey + sizeof(FILE_PREFIX) - 1, buf))
      return 1;
    secret= buf;
  }

  return parse_file(keys, secret);
}


/*
  secret is limited to MAX_SECRET_SIZE characters
*/

bool Parser::read_filekey(const char *filekey, char *secret)
{
  int f= open(filekey, O_RDONLY|O_BINARY);
  if (f == -1)
  {
    my_error(EE_FILENOTFOUND,ME_ERROR_LOG, filekey, errno);
    return 1;
  }

  int len= read(f, secret, MAX_SECRET_SIZE);
  if (len <= 0)
  {
    my_error(EE_READ,ME_ERROR_LOG, filekey, errno);
    close(f);
    return 1;
  }
  close(f);
  while (secret[len - 1] == '\r' || secret[len - 1] == '\n') len--;
  secret[len]= '\0';
  return 0;
}


/**
   Get the keys from the key file <filename> and decrypt it with the
   key <secret>.  Store the keys with id smaller then <maxKeyId> in an
   array of structs keyentry.
   
   @return 0 when ok, 1 for an error
 */

bool Parser::parse_file(std::map<uint,keyentry> *keys, const char *secret)
{
  char *buffer= read_and_decrypt_file(secret);

  if (!buffer)
    return 1;

  keyentry key;
  char *line=buffer;

  while (*line)
  {
    line_number++;
    switch (parse_line(&line, &key)) {
    case 1: // comment
      break;
    case -1: // error
      free(buffer);
      return 1;
    case 0:
      (*keys)[key.id] = key;
      break;
    }
  }

  free(buffer);
  if (keys->size() == 0 || (*keys)[1].id == 0)
  {
    report_error("System key id 1 is missing", 0);
    return 1;
  }

  return 0;
}

void Parser::report_error(const char *reason, size_t position)
{
  my_printf_error(EE_READ, "%s at %s line %u, column %zu",
    ME_ERROR_LOG, reason, filename, line_number, position + 1);
}

/*
  return 0 - new key
         1 - comment
        -1 - error
*/
int Parser::parse_line(char **line_ptr, keyentry *key)
{
  int res= 1;
  char *p= *line_ptr;
  while (isspace(*p) && *p != '\n') p++;
  if (*p != '#' && *p != '\n')
  {
    if (!isdigit(*p))
    {
      report_error("Syntax error", p - *line_ptr);
      return -1;
    }

    longlong id = 0;
    while (isdigit(*p))
    {
      id = id * 10 + *p - '0';
      if (id > UINT_MAX32)
      {
        report_error("Invalid key id", p - *line_ptr);
        return -1;
      }
      p++;
    }

    if (id < 1)
    {
      report_error("Invalid key id", p - *line_ptr);
      return -1;
    }

    if (*p != ';')
    {
      report_error("Syntax error", p - *line_ptr);
      return -1;
    }

    p++;
    key->id= (unsigned int)id;
    key->length=0;
    while (isxdigit(p[0]) && isxdigit(p[1]) && key->length < sizeof(key->key))
    {
      key->key[key->length++] = from_hex(p[0]) * 16 + from_hex(p[1]);
      p+=2;
    }
    if (isxdigit(*p) ||
        (key->length != 16 && key->length != 24 && key->length != 32))
    {
      report_error("Invalid key", p - *line_ptr);
      return -1;
    }

    res= 0;
  }
  while (*p && *p != '\n') p++;
  *line_ptr= *p == '\n' ? p + 1 : p;
  return res;
}

/**
   Decrypt the key file 'filename' if it is encrypted with the key
   'secret'.  Store the content of the decrypted file in 'buffer'. The
   buffer has to be freed in the calling function.
 */
#ifdef _WIN32
#define lseek _lseeki64
#endif

char* Parser::read_and_decrypt_file(const char *secret)
{
  int f;
  if (!filename || !filename[0])
  {
    my_printf_error(EE_CANT_OPEN_STREAM, "file-key-management-filename is not set",
                    ME_ERROR_LOG);
    goto err0;
  }

  f= open(filename, O_RDONLY|O_BINARY, 0);
  if (f < 0)
  {
    my_error(EE_FILENOTFOUND, ME_ERROR_LOG, filename, errno);
    goto err0;
  }

  my_off_t file_size;
  file_size= lseek(f, 0, SEEK_END);

  if (file_size == MY_FILEPOS_ERROR || (my_off_t)lseek(f, 0, SEEK_SET) == MY_FILEPOS_ERROR)
  {
    my_error(EE_CANT_SEEK, MYF(0), filename, errno);
    goto err1;
  }

  if (file_size > MAX_KEY_FILE_SIZE)
  {
    my_error(EE_READ, MYF(0), filename, EFBIG);
    goto err1;
  }

  //Read file into buffer
  uchar *buffer;
  buffer= (uchar*)malloc((size_t)file_size + 1);
  if (!buffer)
  {
    my_error(EE_OUTOFMEMORY, ME_ERROR_LOG| ME_FATAL, file_size);
    goto err1;
  }

  if (read(f, buffer, (int)file_size) != (int)file_size)
  {
    my_printf_error(EE_READ,
      "read from %s failed, errno %d",
      MYF(ME_ERROR_LOG|ME_FATAL), filename, errno);
    goto err2;
  }

// Check for file encryption
  uchar *decrypted;
  if (file_size > OpenSSL_prefix_len && strncmp((char*)buffer, OpenSSL_prefix, OpenSSL_prefix_len) == 0)
  {
    uchar key[OpenSSL_key_len];
    uchar iv[OpenSSL_iv_len];

    decrypted= (uchar*)malloc((size_t)file_size);
    if (!decrypted)
    {
      my_error(EE_OUTOFMEMORY, ME_ERROR_LOG | ME_FATAL, file_size);
      goto err2;
    }
    bytes_to_key(buffer + OpenSSL_prefix_len, secret, key, iv);
    uint32 d_size;
    if (my_aes_crypt(MY_AES_CBC, ENCRYPTION_FLAG_DECRYPT,
                     buffer + OpenSSL_prefix_len + OpenSSL_salt_len,
                     (unsigned int)file_size - OpenSSL_prefix_len - OpenSSL_salt_len,
                     decrypted, &d_size, key, OpenSSL_key_len,
                     iv, OpenSSL_iv_len))

    {
      my_printf_error(EE_READ, "Cannot decrypt %s. Wrong key?", ME_ERROR_LOG, filename);
      goto err3;
    }

    free(buffer);
    buffer= decrypted;
    file_size= d_size;
  }
  else if (*secret)
  {
    my_printf_error(EE_READ, "Cannot decrypt %s. Not encrypted", ME_ERROR_LOG, filename);
    goto err2;
  }

  buffer[file_size]= '\0';
  close(f);
  return (char*) buffer;

err3:
  free(decrypted);
err2:
  free(buffer);
err1:
  close(f);
err0:
  return NULL;
}

/* Copyright (C) 2014 eperi GmbH.

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

/******************************************************************//**
 @file EncKeys.cc
 A class to keep keys for encryption/decryption.

How it works...
The location and usage can be configured via the configuration file.
Example

[mysqld]
...
file_key_management_plugin_filename = /home/mdb/keys.enc
file_key_management_plugin_filekey = secret
file_key_management_plugin_encryption_method = aes_cbc

...

Optional configuration value
file_key_management_plugin_encryption_method determines the method
used for encryption.
Supported are aes_cbc, aes_ecb or aes_ctr. aes_cbc is default.
The plug-in sets the default aes encryption/decryption method to the given method.

The keys are read from a file.
The filename is set up via the file_key_management_plugin_filename
configuration value.
file_key_management_plugin_filename is used to configure the absolute
path to this file.

Examples:
file_key_management_plugin_filename = \\\\unc\\keys.enc 	(windows share)
file_key_management_plugin_filename = e:/tmp/keys.enc 		(windows path)
file_key_management_plugin_filename = /tmp/keys.enc    		(linux path)

The key file contains AES keys and initialization vectors as
hex-encoded Strings.
Supported are keys of size 128, 192 or 256 bits. IV consists of 16 bytes.
Example:
1;F5502320F8429037B8DAEF761B189D12;770A8A65DA156D24EE2A093277530142

1 is the key identifier which can be used for table creation, a 16
byte IV follows, and finally a 16 byte AES key.
255 entries are supported.

The key file should be encrypted and the key to decrypt the file can
be given with the optional file_key_management_plugin_filekey
parameter.

The file key can also be located if FILE: is prepended to the
key. Then the following part is interpreted as absolute path to the
file containing the file key. This file can optionally be encrypted,
currently with a fix key.

Example:

file_key_management_plugin_filekey = FILE:y:/secret256.enc

If the key file can not be read at server startup, for example if the
file key is not present, page_encryption feature is not availabe and
access to page_encryption tables is not possible.

Open SSL command line utility can be used to create an encrypted key file.
Examples:
openssl enc –aes-256-cbc –md sha1 –k secret –in keys.txt –out keys.enc
openssl enc –aes-256-cbc –md sha1 –k <initialPwd> –in secret –out secret.enc

 Created 09/15/2014
***********************************************************************/

#ifdef __WIN__
#define PCRE_STATIC 1
#endif

#include <my_global.h>
#include <sql_class.h>  /* For sql_print_error */
#include "EncKeys.h"
#include <my_aes.h>
#include <memory.h>
#include <my_sys.h>
#include <pcre.h>
#include <string.h>
#include <my_sys.h>

const char* EncKeys::strMAGIC= "Salted__";
const int EncKeys::magicSize= 8;//strlen(strMAGIC); // 8 byte
const char* EncKeys::newLine= "\n";

const char* EncKeys::errorNoKeyId= "KeyID %u not found or with error. Check the key and the log file.\n";
const char* EncKeys::errorInMatches= "Wrong match of the keyID in line %u, see the template.\n";
const char* EncKeys::errorExceedKeyFileSize= "The size of the key file %s exceeds "
				"the maximum allowed of %u bytes.\n";
const char* EncKeys::errorExceedKeySize= "The key size exceeds the maximum allowed size of %u in line %u.\n";
const char* EncKeys::errorEqualDoubleKey= "More than one identical key with keyID %u found"
				" in lines %u and %u.\nDelete one of them in the key file.\n";
const char* EncKeys::errorUnequalDoubleKey= "More than one not identical key with keyID %u found"
				" in lines %u and %u.\nChoose the right one and delete the other in the key file.\n"
				"I'll take the key from line %u\n";
#define errorNoInitializedKey "The key could not be initialized.\n"
const char* EncKeys::errorNotImplemented= "Initializing keys through key server is not"
				" yet implemented.\nYou can not read encrypted tables or columns\n\n";
const char* EncKeys::errorOpenFile= "Could not open %s for reading. You can not read encrypted tables or columns.\n\n";
const char* EncKeys::errorReadingFile= "Could not read from %s. You can not read encrypted tables or columns\n\n";
const char* EncKeys::errorFileSize= "Could not get the file size from %s. You can not read encrypted tables or columns\n\n";
const char* EncKeys::errorFalseFileKey= "Wrong encryption / decryption key for keyfile '%s'.\n";

/* read this from a secret source in some later version */
const char* EncKeys::initialPwd= "lg28s9ac5ffa537fd8798875c98e190df289da7e047c05";

EncKeys::EncKeys()
{
  countKeys= keyLineInKeyFile= 0;
  for (int ii= 0; ii < MAX_KEYS; ii++) {
    keys[ii].id= 0;
    keys[ii].iv= keys[ii].key= NULL;
  }
  oneKey= NULL;
}


EncKeys::~EncKeys()
{
  for (int ii= MAX_KEYS - 1; ii >= 0 ; ii--) {
    delete[] keys[ii].iv;
    keys[ii].iv= NULL;
    delete[] keys[ii].key;
    keys[ii].key= NULL;
  }
}


bool EncKeys::initKeys(const char *filename, const char *filekey)
{
  if (filename==NULL)
    return false;

  const char *MAGIC= "FILE:";
  const short MAGIC_LEN= 5;

  char *secret= (char*) malloc(MAX_SECRET_SIZE +1 * sizeof(char));

  if (filekey != NULL)
  {
    //If secret starts with FILE: interpret the secret as filename.
    if(memcmp(MAGIC, filekey, MAGIC_LEN) == 0)
    {
      int fk_len= strlen(filekey);
      char *secretfile= (char*)malloc((1 + fk_len - MAGIC_LEN)* sizeof(char));
      memcpy(secretfile, filekey+MAGIC_LEN, fk_len - MAGIC_LEN);
      secretfile[fk_len-MAGIC_LEN]= '\0';
      parseSecret(secretfile, secret);
      free(secretfile);
    } else
    {
      sprintf(secret, "%s", filekey);
    }
  }

  int ret= parseFile((const char *)filename, 254, secret);
  free(secret);
  return (ret==NO_ERROR_KEY_FILE_PARSE_OK);
}


/*
  secret is limited to MAX_SECRET_SIZE characters
*/

void EncKeys::parseSecret(const char *secretfile, char *secret)
{
  size_t maxSize= (MAX_SECRET_SIZE +16 + magicSize*2) ;
  char* buf= (char*)malloc((maxSize) * sizeof(char));
  char* _initPwd= (char*)malloc((strlen(initialPwd)+1) * sizeof(char));
  FILE *fp= fopen(secretfile, "rb");
  fseek(fp, 0L, SEEK_END);
  long file_size= ftell(fp);
  rewind(fp);
  size_t bytes_to_read= ((maxSize >= (size_t) file_size) ? (size_t) file_size :
                         maxSize);
  bytes_to_read= fread(buf, 1, bytes_to_read, fp);
  if (memcmp(buf, strMAGIC, magicSize))
  {
    bytes_to_read= (bytes_to_read>MAX_SECRET_SIZE) ? MAX_SECRET_SIZE :
      bytes_to_read;
    memcpy(secret, buf, bytes_to_read);
    secret[bytes_to_read]= '\0';
  }
  else
  {
    unsigned char salt[magicSize];
    unsigned char *key= new unsigned char[keySize32];
    unsigned char *iv= new unsigned char[ivSize16];
    memcpy(&salt, buf + magicSize, magicSize);
    memcpy(_initPwd, initialPwd, strlen(initialPwd));
    _initPwd[strlen(initialPwd)]= '\0';
    my_bytes_to_key((unsigned char *) salt, _initPwd, key, iv);
    uint32 d_size= 0;
    my_aes_decrypt_dynamic_type func= get_aes_decrypt_func(MY_AES_ALGORITHM_CBC);
    int re= (* func)((const uchar*)buf + 2 * magicSize,
                      bytes_to_read - 2 * magicSize,
                      (uchar*)secret, &d_size, (const uchar*)key, keySize32,
                      iv, ivSize16, 0);
    if (re)
      d_size= 0;
    if (d_size>EncKeys::MAX_SECRET_SIZE)
    {
      d_size= EncKeys::MAX_SECRET_SIZE;
    }
    secret[d_size]= '\0';
    delete[] key;
    delete[] iv;
  }
  free(buf);
  free(_initPwd);
  fclose(fp);
}


/**
 * Returns a struct keyentry with the asked 'id' or NULL.
 */
keyentry *EncKeys::getKeys(int id)
{
  if (KEY_MIN <= id && KEY_MAX >= id && (&keys[id - 1])->iv)
  {
    return &keys[id - 1];
  }
#ifndef DBUG_OFF
  else
  {
    sql_print_error(errorNoKeyId, id);
  }
#endif
  return NULL;
}

/**
   Get the keys from the key file <filename> and decrypt it with the
   key <secret>.  Store the keys with id smaller then <maxKeyId> in an
   array of structs keyentry.  Returns NO_ERROR_PARSE_OK or an
   appropriate error code.
 */

int EncKeys::parseFile(const char* filename, const uint32 maxKeyId,
                       const char *secret)
{
  int errorCode= 0;
  char *buffer= decryptFile(filename, secret, &errorCode);
  uint32 id= 0;

  if (errorCode != NO_ERROR_PARSE_OK)
    return errorCode;
  errorCode= NO_ERROR_KEY_FILE_PARSE_OK;

  char *line= strtok(buffer, newLine);
  while (NULL != line)
  {
    keyLineInKeyFile++;
    switch (parseLine(line, maxKeyId)) {
    case NO_ERROR_PARSE_OK:
      id= oneKey->id;
      keys[oneKey->id - 1]= *oneKey;
      delete(oneKey);
      countKeys++;
      break;
    case ERROR_ID_TOO_BIG:
      sql_print_error(errorExceedKeySize, KEY_MAX,
              keyLineInKeyFile);
      sql_print_error(" ---> %s\n", line);
      errorCode= ERROR_KEY_FILE_EXCEEDS_MAX_NUMBERS_OF_KEYS;
      break;
    case ERROR_NOINITIALIZEDKEY:
      sql_print_error(errorNoInitializedKey);
      sql_print_error(" ----> %s\n", line);
      errorCode= ERROR_KEY_FILE_PARSE_NULL;
      break;
    case ERROR_WRONG_NUMBER_OF_MATCHES:
      sql_print_error(errorInMatches, keyLineInKeyFile);
      sql_print_error(" -----> %s\n", line);
      errorCode= ERROR_KEY_FILE_PARSE_NULL;
      break;
    case NO_ERROR_KEY_GREATER_THAN_ASKED:
      sql_print_error("No asked key in line %u: %s\n",
              keyLineInKeyFile, line);
      break;
    case NO_ERROR_ISCOMMENT:
      sql_print_error("Is comment in line %u: %s\n",
              keyLineInKeyFile, line);
    default:
      break;
    }
    line= strtok(NULL, newLine);
  }

  free(line);
  line= NULL;
  delete[] buffer;
  buffer= NULL;
  return errorCode;
}


int EncKeys::parseLine(const char *line, const uint32 maxKeyId)
{
  int ret= NO_ERROR_PARSE_OK;
  if (isComment(line))
    ret= NO_ERROR_ISCOMMENT;
  else
  {
    const char *error_p= NULL;
    int offset;
    pcre *pattern= pcre_compile(
                                 "([0-9]+);([0-9,a-f,A-F]{32});([0-9,a-f,A-F]{64}|[0-9,a-f,A-F]{48}|[0-9,a-f,A-F]{32})",
                                 0, &error_p, &offset, NULL);
    if (NULL != error_p)
      sql_print_error("Error: %s\nOffset: %d\n", error_p, offset);

    int m_len= (int) strlen(line), ovector[MAX_OFFSETS_IN_PCRE_PATTERNS];
    int rc= pcre_exec(pattern, NULL, line, m_len, 0, 0, ovector,
                       MAX_OFFSETS_IN_PCRE_PATTERNS);
    pcre_free(pattern);
    if (4 == rc)
    {
      char lin[MAX_KEY_LINE_SIZE + 1];
      strncpy(lin, line, MAX_KEY_LINE_SIZE);
      lin[MAX_KEY_LINE_SIZE]= '\0';
      char *substring_start= lin + ovector[2];
      int substr_length= ovector[3] - ovector[2];
      if (3 < substr_length)
        ret= ERROR_ID_TOO_BIG;
      else
      {
        char buffer[4];
        sprintf(buffer, "%.*s", substr_length, substring_start);
        uint32 id= atoi(buffer);
        if (0 == id)		ret= ERROR_NOINITIALIZEDKEY;
        else if (KEY_MAX < id)	ret= ERROR_ID_TOO_BIG;
        else if (maxKeyId < id)	ret= NO_ERROR_KEY_GREATER_THAN_ASKED;
        else
        {
          oneKey= new keyentry;
          oneKey->id= id;
          substring_start= lin + ovector[4];
          substr_length= ovector[5] - ovector[4];
          oneKey->iv= new char[substr_length + 1];
          sprintf(oneKey->iv, "%.*s", substr_length, substring_start);
          substring_start= lin + ovector[6];
          substr_length= ovector[7] - ovector[6];
          oneKey->key= new char[substr_length + 1];
          sprintf(oneKey->key, "%.*s", substr_length, substring_start);
        }
      }
    }
    else
      ret= ERROR_WRONG_NUMBER_OF_MATCHES;
  }
  return ret;
}

/**
   Decrypt the key file 'filename' if it is encrypted with the key
   'secret'.  Store the content of the decrypted file in 'buffer'. The
   buffer has to be freed in the calling function.
 */

char* EncKeys::decryptFile(const char* filename, const char *secret,
                           int *errorCode)
{
  *errorCode= NO_ERROR_PARSE_OK;
  FILE *fp= fopen(filename, "rb");
  if (NULL == fp)
  {
    sql_print_error(errorOpenFile, filename);
    *errorCode= ERROR_OPEN_FILE;
    return NULL;
  }

  if (fseek(fp, 0L, SEEK_END))
  {
    *errorCode= ERROR_READING_FILE;
    return NULL;
  }
  long file_size= ftell(fp);   // get the file size
  if (MAX_KEY_FILE_SIZE < file_size)
  {
    sql_print_error(errorExceedKeyFileSize, filename, MAX_KEY_FILE_SIZE);
    *errorCode=  ERROR_KEY_FILE_TOO_BIG;
    fclose(fp);
    return NULL;
  }
  else if (-1L == file_size)
  {
    sql_print_error(errorFileSize, filename);
    *errorCode= ERROR_READING_FILE;
    return NULL;
  }

  rewind(fp);
  //Read file into buffer
  uchar *buffer= new uchar[file_size + 1];
  file_size= fread(buffer, 1, file_size, fp);
  buffer[file_size]= '\0';
  fclose(fp);
  //Check for file encryption
  if (0 == memcmp(buffer, strMAGIC, magicSize))
  {
    //If file is encrypted, decrypt it first.
    unsigned char salt[magicSize];
    unsigned char *key= new unsigned char[keySize32];
    unsigned char *iv= new unsigned char[ivSize16];
    uchar *decrypted= new uchar[file_size];
    memcpy(&salt, buffer + magicSize, magicSize);
    my_bytes_to_key((unsigned char *) salt, secret, key, iv);
    uint32 d_size= 0;
    my_aes_decrypt_dynamic_type func= get_aes_decrypt_func(MY_AES_ALGORITHM_CBC);
    int res= (* func)((const uchar*)buffer + 2 * magicSize,
                      file_size - 2 * magicSize,
                      decrypted, &d_size, (const uchar*) key, keySize32,
                      iv, ivSize16, 0);
    if(0 != res)
    {
      *errorCode= ERROR_FALSE_FILE_KEY;
      delete[] buffer;	buffer= NULL;
      sql_print_error(errorFalseFileKey, filename);
    }
    else
    {
      memcpy(buffer, decrypted, d_size);
      buffer[d_size]= '\0';
    }

    delete[] decrypted;		decrypted= NULL;
    delete[] key;			key= NULL;
    delete[] iv;			iv= NULL;
  }
  return (char*) buffer;
}

bool EncKeys::isComment(const char *line)
{
  const char *error_p;
  int offset, m_len= (int) strlen(line),
    ovector[MAX_OFFSETS_IN_PCRE_PATTERNS];
  pcre *pattern= pcre_compile("\\s*#.*", 0, &error_p, &offset, NULL);
  int rc= pcre_exec(pattern, NULL, line, m_len, 0, 0, ovector,
                     MAX_OFFSETS_IN_PCRE_PATTERNS);
  pcre_free(pattern);
  return (rc >= 0);
}


void EncKeys::printKeyEntry(uint32 id)
{
#ifndef DBUG_OFF
  keyentry *entry= getKeys(id);
  if (NULL == entry)
  {
    sql_print_error("No such keyID: %u\n",id);
  }
  else
  {
    sql_print_error("Key: id: %3u\tiv:%lu bytes\tkey:%lu bytes\n",
                    entry->id, strlen(entry->iv)/2, strlen(entry->key)/2);
  }
#endif /* DBUG_OFF */
}

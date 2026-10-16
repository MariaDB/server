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
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/******************************************************************//**
@file Parser.h
A structure and class to keep keys for encryption/decryption.

Created 09/15/2014
***********************************************************************/

#include <my_crypt.h>
#include <my_sys.h>
#include <ctype.h>
#include <map>
#include <stdlib.h> /* size_t */

struct keyentry {
  unsigned int id;
  unsigned int version;
  unsigned char key[MY_AES_MAX_KEY_LENGTH];
  unsigned int length;
};

/* version -> key, ordered so that rbegin() is the latest version */
typedef std::map<unsigned int, keyentry> key_versions;
/* key id -> all versions of that key */
typedef std::map<unsigned int, key_versions> keymap;

/* Overwrite secrets before their memory is reused; volatile stops the
   compiler from dropping the stores as dead. */
static inline void secure_zero(void *p, size_t n)
{
  volatile unsigned char *v= (volatile unsigned char *) p;
  while (n--)
    *v++= 0;
}

class Parser
{
  const char *filename;
  const char *filekey;
  const enum my_digest digest;
  const uint use_pbkdf2;
  unsigned int line_number;
  /* ME_ERROR_LOG reports to the client too; a reload uses ME_ERROR_LOG_ONLY */
  const myf log_flags;

  unsigned int from_hex(char c)
  { return c <= '9' ? c - '0' : tolower(c) - 'a' + 10; }

  bool read_filekey(const char *filekey, char *secret);
  bool parse_file(keymap *keys, const char *secret);
  void report_error(const char *reason, size_t position);
  int parse_line(char **line_ptr, keyentry *key);
  char* read_and_decrypt_file(const char *secret);

public:
  Parser(const char* fn, const char *fk, enum my_digest digest,
         uint use_pbkdf2, myf log_flags= ME_ERROR_LOG) :
  filename(fn), filekey(fk), digest(digest), use_pbkdf2(use_pbkdf2),
  line_number(0), log_flags(log_flags) { }
  bool parse(keymap *keys);
};

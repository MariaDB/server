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
#include <ctype.h>
#include <map>
#include <stdlib.h> /* size_t */

struct keyentry {
  unsigned int id;
  unsigned char key[MY_AES_MAX_KEY_LENGTH];
  unsigned int length;
};

class Parser
{
  const char *filename;
  const char *filekey;
  unsigned int line_number;

  unsigned int from_hex(char c)
  { return c <= '9' ? c - '0' : tolower(c) - 'a' + 10; }

  void bytes_to_key(const unsigned char *salt, const char *secret,
                    unsigned char *key, unsigned char *iv);
  bool read_filekey(const char *filekey, char *secret);
  bool parse_file(std::map<unsigned int ,keyentry> *keys, const char *secret);
  void report_error(const char *reason, size_t position);
  int parse_line(char **line_ptr, keyentry *key);
  char* read_and_decrypt_file(const char *secret);

public:
  Parser(const char* fn, const char *fk) :
    filename(fn), filekey(fk), line_number(0) { }
  bool parse(std::map<unsigned int ,keyentry> *keys);
};

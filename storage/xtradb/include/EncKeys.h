/* Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.

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
@file EncKeys.h
A structure and class to keep keys for encryption/decryption.

Created 09/15/2014 Florin Fugaciu
***********************************************************************/

#ifndef ENCKEYS_H_
#define ENCKEYS_H_

#include <sys/types.h>
#include <stdio.h>

#define MAX_KEYS 255
#define KEY_MIN  1
#define KEY_MAX  MAX_KEYS

#define MAX_KEYLEN	512
#define MAX_IVLEN	256

#define KEYINITTYPE_FILE	1
#define KEYINITTYPE_SERVER	2

#define ERROR_NOINITIALIZEDKEYS	1

#define E_WRONG_NUMBER_OF_MATCHES 10


struct keyentry {
    uint id;
    char *iv;
    char *key;
};


class EncKeys {
	uint lenKey;
	keyentry keys[MAX_KEYS];
	keyentry oneKey;

	bool initKeysThroughFile( const char *name, const char *path);
	bool isComment( const char *line);
	bool parseFile( const char* filename, const uint k_len);
	bool parseLine( const char *line);

public:
	EncKeys();
	virtual ~EncKeys();
	bool initKeys( const char *name, const char *url, const int initType);
	keyentry *getKeys( int id);
};

#endif /* ENCKEYS_H_ */

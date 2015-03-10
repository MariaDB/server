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
@file EncKeys.h
A structure and class to keep keys for encryption/decryption.

Created 09/15/2014
***********************************************************************/

#ifndef ENCKEYS_H_
#define ENCKEYS_H_

#include <my_global.h>
#include <sys/types.h>
#include <stdio.h>





struct keyentry {
	uint32 id;
	char *iv;
	char *key;
};


class EncKeys
{
private:
	static const char *strMAGIC, *newLine;
	static const int magicSize;

	enum constants { MAX_OFFSETS_IN_PCRE_PATTERNS = 30};
	enum keyAttributes { KEY_MIN = 1, KEY_MAX = 255, MAX_KEYS = 255,
		MAX_IVLEN = 256, MAX_KEYLEN = 512, ivSize16 = 16, keySize32 = 32 };
	enum keyInitType { KEYINITTYPE_FILE = 1, KEYINITTYPE_SERVER = 2 };
	enum errorAttributes { MAX_KEY_LINE_SIZE = 3 * MAX_KEYLEN, MAX_KEY_FILE_SIZE = 1048576 };
	enum errorCodesLine { NO_ERROR_PARSE_OK = 0, NO_ERROR_ISCOMMENT = 10, NO_ERROR_KEY_GREATER_THAN_ASKED = 20,
		ERROR_NOINITIALIZEDKEY = 30, ERROR_ID_TOO_BIG = 40, ERROR_WRONG_NUMBER_OF_MATCHES = 50,
		ERROR_EQUAL_DOUBLE_KEY = 60, ERROR_UNEQUAL_DOUBLE_KEY = 70 };

	static const char *errorNoKeyId, *errorInMatches, *errorExceedKeyFileSize,
		*errorExceedKeySize, *errorEqualDoubleKey, *errorUnequalDoubleKey,
		*errorNoInitializedKey, *errorFalseFileKey,
		*errorNotImplemented, *errorOpenFile, *errorReadingFile, *errorFileSize;

	static const char* initialPwd;
	uint32 countKeys, keyLineInKeyFile;
	keyentry keys[MAX_KEYS], *oneKey;

	void printKeyEntry( uint32 id);
	bool isComment( const char *line);
	char * decryptFile( const char* filename, const char *secret, int *errorCode);
	int parseFile( const char* filename, const uint32 maxKeyId, const char *secret);
	int parseLine( const char *line, const uint32 maxKeyId);

public:
	static const size_t MAX_SECRET_SIZE = 256;

	enum errorCodesFile { NO_ERROR_KEY_FILE_PARSE_OK = 0, ERROR_KEY_FILE_PARSE_NULL = 110,
		ERROR_KEY_FILE_TOO_BIG = 120, ERROR_KEY_FILE_EXCEEDS_MAX_NUMBERS_OF_KEYS = 130,
		ERROR_OPEN_FILE = 140, ERROR_READING_FILE = 150, ERROR_FALSE_FILE_KEY = 160,
		ERROR_KEYINITTYPE_SERVER_NOT_IMPLEMENTED = 170, ERROR_ENCRYPTION_SECRET_NULL = 180 };
	EncKeys();
	virtual ~EncKeys();
	bool initKeys( const char *filename, const char *filekey);
	keyentry *getKeys( int id);
	/* made public for unit testing */
	static void parseSecret( const char *filename, char *secret );

};

#endif /* ENCKEYS_H_ */

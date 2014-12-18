/* Copyright (C) 2014 eperi GmbH. All Rights Reserved.

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
innodb_data_encryption_providertype = 1
innodb_data_encryption_providername = keys.enc
innodb_data_encryption_providerurl = /home/mdb/
innodb_data_encryption_filekey = secret
...

As provider type currently only value 1 is supported, which means, the keys are read from a file.
The filename is set up via the innodb_data_encryption_providername configuration value.
innodb_data_encryption_providerurl is used to configure the path to this file. This is usually
a folder name.
Examples:
innodb_data_encryption_providerurl = \\\\unc 	(windows share)
innodb_data_encryption_providerurl = e:/tmp/ 	(windows path)
innodb_data_encryption_providerurl = /tmp    	(linux path)

The key file contains AES keys and initialization vectors as hex-encoded Strings.
Supported are keys of size 128, 192 or 256 bits. IV consists of 16 bytes.

The key file should be encrypted and the key to decrypt the file can be given with the
innodb_data_encryption_filekey parameter.

The file key can also be located if FILE: is prepended to the key. Then the following part is interpreted
as absolut to the file containing the file key. This file can optionally be encrypted, currently with a fix key.
Example:
innodb_data_encryption_filekey = FILE:y:/secret256.enc

If the key file can not be read at server startup, for example if the file key is not present,
page_encryption feature is not availabe and access to page_encryption tables is not possible.

Example files can be found inside the unittest/eperi folder.

Open SSL command line utility can be used to create an encrypted key file.
Examples:
openssl enc –aes-256-cbc –md sha1 –k secret –in keys.txt –out keys.enc
openssl enc –aes-256-cbc –md sha1 –k <initialPwd> –in secret –out secret.enc

 Created 09/15/2014
 ***********************************************************************/
#ifdef __WIN__
#define PCRE_STATIC 1
#endif

#include "EncKeys.h"
#include <my_global.h>
#include <my_aes.h>
#include <memory.h>
#include <my_sys.h>
#include <pcre.h>
#include <string.h>
#include <my_sys.h>




const char* EncKeys::strMAGIC = "Salted__";
const int EncKeys::magicSize = 8;//strlen(strMAGIC); // 8 byte
const char* EncKeys::newLine = "\n";

const char* EncKeys::errorNoKeyId = "KeyID = %u not found or with error. Check the key and the log file.\n";
const char* EncKeys::errorInMatches = "Wrong match of the keyID in line %u, see the template.\n";
const char* EncKeys::errorExceedKeyFileSize = "The size of the key file %s exceeds "
				"the maximum allowed of %u bytes.\n";
const char* EncKeys::errorExceedKeySize = "The key size exceeds the maximum allowed size of %u in line %u.\n";
const char* EncKeys::errorEqualDoubleKey = "More than one identical key with keyID = %u found"
				" in lines %u and %u.\nDelete one of them in the key file.\n";
const char* EncKeys::errorUnequalDoubleKey = "More than one not identical key with keyID = %u found"
				" in lines %u and %u.\nChoose the right one and delete the other in the key file.\n"
				"I'll take the key from line %u\n";
const char* EncKeys::errorNoInitializedKey = "The key could not be initialized.\n";
const char* EncKeys::errorNotImplemented = "Initializing keys through key server is not"
				" yet implemented.\nYou can not read encrypted tables or columns\n\n";
const char* EncKeys::errorOpenFile = "Could not open %s for reading. You can not read encrypted tables or columns.\n\n";
const char* EncKeys::errorReadingFile = "Could not read from %s. You can not read encrypted tables or columns\n\n";
const char* EncKeys::errorFileSize = "Could not get the file size from %s. You can not read encrypted tables or columns\n\n";
const char* EncKeys::errorFalseFileKey = "Wrong encryption / decryption key for keyfile '%s'.\n";

/* read this from a secret source in some later version */
const char* EncKeys::initialPwd = "lg28s9ac5ffa537fd8798875c98e190df289da7e047c05";

EncKeys::EncKeys() {
	countKeys = keyLineInKeyFile = 0;
	for (int ii = 0; ii < MAX_KEYS; ii++) {
		keys[ii].id = 0;
		keys[ii].iv = keys[ii].key = NULL;
	}
	oneKey = NULL;
}

EncKeys::~EncKeys() {
	for (int ii = MAX_KEYS - 1; ii >= 0 ; ii--) {
		delete[] keys[ii].iv;	keys[ii].iv = NULL;
		delete[] keys[ii].key;	keys[ii].key = NULL;

	}
}

bool EncKeys::initKeys(const char *name, const char *url, const int initType, const char *filekey) {
	if (KEYINITTYPE_FILE == initType)
	{
		int result = initKeysThroughFile(name, url, filekey);
		return ERROR_FALSE_FILE_KEY != result && ERROR_OPEN_FILE != result && ERROR_READING_FILE != result;
	}
	else if (KEYINITTYPE_SERVER == initType)
	{
		return NO_ERROR_KEY_FILE_PARSE_OK == initKeysThroughServer(name, url, filekey);
	}
	return false;
}

int EncKeys::initKeysThroughFile(const char *name, const char *path, const char *filekey) {
	if (path==NULL || name==NULL) return ERROR_OPEN_FILE;
	size_t len1 = strlen(path);
	size_t len2 = strlen(name);
	const char *MAGIC = "FILE:";
	const short MAGIC_LEN = 5;
	int ret = NO_ERROR_KEY_FILE_PARSE_OK;
	bool isUncPath= (len1>2) ? ((strncmp("\\\\", path, 2)==0) ? TRUE : FALSE) : FALSE;
		bool isSlash = ((isUncPath? '\\':'/') == path[len1 - 1]);
		char *secret = (char*) malloc(MAX_SECRET_SIZE +1 * sizeof(char));
		char *filename = (char*) malloc((len1 + len2 + (isSlash ? 1 : 2)) * sizeof(char));
		if(filekey != NULL)
		{
			//If secret starts with FILE: interpret the secret as filename.
			if(memcmp(MAGIC, filekey, MAGIC_LEN) == 0) {
				int fk_len = strlen(filekey);
				char *secretfile = (char*)malloc( (1 + fk_len - MAGIC_LEN)* sizeof(char));
				memcpy(secretfile, filekey+MAGIC_LEN, fk_len - MAGIC_LEN);
				secretfile[fk_len-MAGIC_LEN] = '\0';
				parseSecret(secretfile, secret);
				free(secretfile);
			} else
			{
				sprintf(secret, "%s", filekey);
			}
		}
		sprintf(filename, "%s%s%s", path, isSlash ? "" : (isUncPath ? "\\":"/"), name);
		ret = parseFile((const char *)filename, 254, secret);
		free(filename);
		free(secret);
	return ret;
}

int EncKeys::initKeysThroughServer( const char *name, const char *path, const char *filekey)
{
	//TODO
#ifdef UNIV_DEBUG
	fprintf(stderr, errorNotImplemented);
#endif //UNIV_DEBUG
	return ERROR_KEYINITTYPE_SERVER_NOT_IMPLEMENTED;
}

/*
 * secret is limited to MAX_SECRET_SIZE characters
 */
void EncKeys::parseSecret( const char *secretfile, char *secret ) {
	int maxSize = (MAX_SECRET_SIZE +16 + magicSize*2) ;
	char* buf = (char*)malloc((maxSize) * sizeof(char));
	char* _initPwd = (char*)malloc((strlen(initialPwd)+1) * sizeof(char));

	FILE *fp = fopen(secretfile, "rb");
	fseek(fp, 0L, SEEK_END);
	long file_size = ftell(fp);
	rewind(fp);
	int bytes_to_read = (maxSize >= file_size)? file_size:(maxSize);
	fread(buf, 1, bytes_to_read, fp);
	if (memcmp(buf, strMAGIC, magicSize)) {
		bytes_to_read = (bytes_to_read>MAX_SECRET_SIZE) ? MAX_SECRET_SIZE : bytes_to_read;
		memcpy(secret, buf, bytes_to_read);
		secret[bytes_to_read] = '\0';
	} else {
		unsigned char salt[magicSize];
		unsigned char *key = new unsigned char[keySize32];
		unsigned char *iv = new unsigned char[ivSize16];
		memcpy(&salt, buf + magicSize, magicSize);
		memcpy(_initPwd, initialPwd, strlen(initialPwd));
		_initPwd[strlen(initialPwd)]= '\0';
		my_bytes_to_key((unsigned char *) salt, _initPwd, key, iv);
		uint32 d_size = 0;
		int res = my_aes_decrypt_cbc((const char*)buf + 2 * magicSize, bytes_to_read - 2 * magicSize,
				secret, &d_size, key, keySize32, iv, ivSize16, 0);
		if (d_size>EncKeys::MAX_SECRET_SIZE) {
			d_size = EncKeys::MAX_SECRET_SIZE;
		}
		secret[d_size] = '\0';
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
keyentry *EncKeys::getKeys(int id) {
	if (KEY_MIN <= id && KEY_MAX >= id && (&keys[id - 1])->iv)
	{
		return &keys[id - 1];
	}
#ifdef UNIV_DEBUG
	else {

		fprintf(stderr, errorNoKeyId, id);
		return NULL;
	}
#endif //UNIV_DEBUG
}

/**
 * Get the keys from the key file <filename> and decrypt it with the key <secret>.
 * Store the keys with id smaller then <maxKeyId> in an array of structs keyentry.
 * Returns NO_ERROR_PARSE_OK or an appropriate error code.
 */
int EncKeys::parseFile(const char* filename, const ulint maxKeyId, const char *secret) {
	int errorCode = 0;
	char *buffer = decryptFile(filename, secret, &errorCode);
	ulint id = 0;

	if (NO_ERROR_PARSE_OK != errorCode)	return errorCode;
	else								errorCode = NO_ERROR_KEY_FILE_PARSE_OK;

	char *line = strtok(buffer, newLine);
	while ( NULL != line) {
		keyLineInKeyFile++;
		switch (parseLine(line, maxKeyId)) {
		case NO_ERROR_PARSE_OK:
			id = oneKey->id;
			keys[oneKey->id - 1] = *oneKey;
			delete(oneKey);
			countKeys++;
			fprintf(stderr, "Line: %u --> ", keyLineInKeyFile); printKeyEntry(id);
			break;
		case ERROR_ID_TOO_BIG:
			fprintf(stderr, errorExceedKeySize, KEY_MAX, keyLineInKeyFile);
			fprintf(stderr, " --> %s\n", line);
			errorCode = ERROR_KEY_FILE_EXCEEDS_MAX_NUMBERS_OF_KEYS;
			break;
		case ERROR_NOINITIALIZEDKEY:
			fprintf(stderr, errorNoInitializedKey);
			fprintf(stderr, " --> %s\n", line);
			errorCode = ERROR_KEY_FILE_PARSE_NULL;
			break;
		case ERROR_WRONG_NUMBER_OF_MATCHES:
			fprintf(stderr, errorInMatches, keyLineInKeyFile);
			fprintf(stderr, " --> %s\n", line);
			errorCode = ERROR_KEY_FILE_PARSE_NULL;
			break;
		case NO_ERROR_KEY_GREATER_THAN_ASKED:
			fprintf(stderr, "No asked key in line %lu: %s\n", keyLineInKeyFile, line);
			break;
		case NO_ERROR_ISCOMMENT:
			fprintf(stderr, "Is comment in line %lu: %s\n", keyLineInKeyFile, line);
		default:
			break;
		}
		line = strtok(NULL, newLine);
	}

	free(line);			line = NULL;
	delete[] buffer;	buffer = NULL;
	return errorCode;
}

int EncKeys::parseLine(const char *line, const ulint maxKeyId) {
	int ret = NO_ERROR_PARSE_OK;
	if (isComment(line))
		ret = NO_ERROR_ISCOMMENT;
	else {
		const char *error_p = NULL;
		int offset;
		pcre *pattern = pcre_compile(
				"([0-9]+);([0-9,a-f,A-F]{32});([0-9,a-f,A-F]{64}|[0-9,a-f,A-F]{48}|[0-9,a-f,A-F]{32})",
				0, &error_p, &offset, NULL);
		if ( NULL != error_p)
			fprintf(stderr, "Error: %s\nOffset: %d\n", error_p, offset);

		int m_len = (int) strlen(line), ovector[MAX_OFFSETS_IN_PCRE_PATTERNS];
		int rc = pcre_exec(pattern, NULL, line, m_len, 0, 0, ovector, MAX_OFFSETS_IN_PCRE_PATTERNS);
		pcre_free(pattern);
		if (4 == rc) {
			char lin[MAX_KEY_LINE_SIZE + 1];
			strncpy( lin, line, MAX_KEY_LINE_SIZE);
			lin[MAX_KEY_LINE_SIZE] = '\0';
			char *substring_start = lin + ovector[2];
			int substr_length = ovector[3] - ovector[2];
			if (3 < substr_length)
				ret = ERROR_ID_TOO_BIG;
			else {
				char buffer[4];
				sprintf(buffer, "%.*s", substr_length, substring_start);
				ulint id = atoi(buffer);
				if (0 == id)			ret = ERROR_NOINITIALIZEDKEY;
				else if (KEY_MAX < id)	ret = ERROR_ID_TOO_BIG;
				else if (maxKeyId < id)	ret = NO_ERROR_KEY_GREATER_THAN_ASKED;
				else {
					oneKey = new keyentry;
					oneKey->id = id;
					substring_start = lin + ovector[4];
					substr_length = ovector[5] - ovector[4];
					oneKey->iv = new char[substr_length + 1];
					sprintf(oneKey->iv, "%.*s", substr_length, substring_start);
					substring_start = lin + ovector[6];
					substr_length = ovector[7] - ovector[6];
					oneKey->key = new char[substr_length + 1];
					sprintf(oneKey->key, "%.*s", substr_length, substring_start);
				}
			}
		}
		else
			ret = ERROR_WRONG_NUMBER_OF_MATCHES;
	}
	return ret;
}

/**
 * Decrypt the key file 'filename' if it is encrypted with the key 'secret'.
 * Store the content of the decrypted file in 'buffer'. The buffer has to be freed
 * in the calling function.
 */
char* EncKeys::decryptFile(const char* filename, const char *secret, int *errorCode) {
	*errorCode = NO_ERROR_PARSE_OK;
	fprintf(stderr, "Reading %s\n\n", filename);
	FILE *fp = fopen(filename, "rb");
	if (NULL == fp) {
		fprintf(stderr, errorOpenFile, filename);
		*errorCode = ERROR_OPEN_FILE;
		return NULL;
	}

	if (fseek(fp, 0L, SEEK_END)) {
		*errorCode = ERROR_READING_FILE;
		return NULL;
	}
	long file_size = ftell(fp);   // get the file size
	if (MAX_KEY_FILE_SIZE < file_size) {
		fprintf(stderr, errorExceedKeyFileSize, filename, MAX_KEY_FILE_SIZE);
		*errorCode =  ERROR_KEY_FILE_TOO_BIG;
		fclose(fp);
		return NULL;
	}
	else if (-1L == file_size) {
		fprintf(stderr, errorFileSize, filename);
		*errorCode = ERROR_READING_FILE;
		return NULL;
	}

	rewind(fp);
	//Read file into buffer
	uchar *buffer = new uchar[file_size + 1];
	size_t read_bytes = fread(buffer, 1, file_size, fp);
	buffer[file_size] = '\0';
	fclose(fp);
	//Check for file encryption
	if (0 == memcmp(buffer, strMAGIC, magicSize)) { //If file is encrypted, decrypt it first.
		unsigned char salt[magicSize];
		unsigned char *key = new unsigned char[keySize32];
		unsigned char *iv = new unsigned char[ivSize16];
		char *decrypted = new char[file_size];
		memcpy(&salt, buffer + magicSize, magicSize);
		my_bytes_to_key((unsigned char *) salt, secret, key, iv);
		uint32 d_size = 0;
		int res = my_aes_decrypt_cbc((const char*)buffer + 2 * magicSize, file_size - 2 * magicSize,
				decrypted, &d_size, key, keySize32, iv, ivSize16, 0);
		if(0 != res) {
			*errorCode = ERROR_FALSE_FILE_KEY;
			delete[] buffer;	buffer = NULL;
			fprintf(stderr, errorFalseFileKey, filename);
		}
		else {
			memcpy(buffer, decrypted, d_size);
			buffer[d_size] = '\0';
		}

		delete[] decrypted;		decrypted = NULL;
		delete[] key;			key = NULL;
		delete[] iv;			iv = NULL;
	}
	return (char*) buffer;
}

bool EncKeys::isComment(const char *line) {
	const char *error_p;
	int offset, m_len = (int) strlen(line), ovector[MAX_OFFSETS_IN_PCRE_PATTERNS];
	pcre *pattern = pcre_compile("\\s*#.*", 0, &error_p, &offset, NULL);
	int rc = pcre_exec( pattern, NULL, line, m_len, 0, 0, ovector, MAX_OFFSETS_IN_PCRE_PATTERNS);
	pcre_free(pattern);
	if (0 > rc)	return false;
	else		return true;
}


void EncKeys::printKeyEntry( ulint id)
{
#ifdef UNIV_DEBUG
	keyentry *entry = getKeys(id);
	if( NULL == entry)	{
		fprintf(stderr, "No such keyID=%lu\n",id);
	}
	else {
		fprintf(stderr, "Key: id:%3lu \tiv:%lu bytes\tkey:%lu bytes\n", entry->id, strlen(entry->iv)/2, strlen(entry->key)/2);
	}
#endif //UNIV_DEBUG
}

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
@file EncKeys.cc
A class to keep keys for encryption/decryption.

Created 09/15/2014 Florin Fugaciu
***********************************************************************/


#include "EncKeys.h"
#include <memory.h>
#include <pcre.h>
#include <string.h>



EncKeys::EncKeys() {
	lenKey = sizeof(keyentry);
	for( int ii=0; ii < MAX_KEYS; ii++) {
		keys[ii].id = ii+1;
		keys[ii].iv = keys[ii].key = NULL;
	}
	oneKey.iv = new char[MAX_IVLEN + 1];
	oneKey.key = new char[MAX_KEYLEN + 1];
}


EncKeys::~EncKeys() {
	for( int ii=0; ii < MAX_KEYS; ii++) {
		delete[] keys[ii].iv;  keys[ii].iv  = NULL;
		delete[] keys[ii].key; keys[ii].key = NULL;
	}
	delete[] oneKey.iv;  oneKey.iv  = NULL;
	delete[] oneKey.key; oneKey.key = NULL;
}

bool EncKeys::initKeysThroughFile( const char *name, const char *path) {
	size_t len1 = strlen(path);
	size_t len2 = strlen(name);
	bool ret = true, isSlash = ('/' == path[len1-1]);
	char *filename = (char *) malloc( len1 + len2 + isSlash ? 1 : 2);

	sprintf( filename, "%s%s%s", path, isSlash ? "" : "/", name);
	ret = parseFile( filename, 256);
	free(filename);
	return ret;
}


bool EncKeys::initKeys( const char *name, const char *url, const int initType) {
	if( KEYINITTYPE_FILE == initType)  // url == path && name == filename
		return initKeysThroughFile( name, url);
	else if( KEYINITTYPE_SERVER == initType) {
		printf("Not yet implemented. I'll exit now.\n\n");
		exit(ERROR_NOINITIALIZEDKEYS);
	}
	return false;
}


keyentry *EncKeys::getKeys( int id) {
	if( KEY_MIN <= id && KEY_MAX >= id) {
		memcpy(&oneKey, &keys[id-1], lenKey);
		return &oneKey;
	}
	else
		return NULL;
}


bool EncKeys::parseFile( const char* filename, const uint k_len)
{
	char *line, *buf = line = NULL;
	size_t len = 0;
	ssize_t read;
	bool ret = true;

	printf("Reading %s\n\n", filename);
	FILE *fp = fopen( filename, "r");
	if(NULL == fp) {
		printf("Could not open %s for reading. I'll exit.\n\n", filename);
		return false;
	}
	while( -1 != (read = getline( &line, &len, fp)) ) {
		line[read - 1] = '\0';
		if( true == parseLine(line) && oneKey.id > 0 && oneKey.id < k_len) {
			keys[oneKey.id - 1].id = oneKey.id;
			buf = (char *) calloc( len = strlen(oneKey.iv) + 1, 1);
			memcpy( buf, oneKey.iv, len);
			keys[oneKey.id - 1].iv = buf;
			buf = (char *) calloc( len = strlen(oneKey.key) + 1, 1);
			memcpy( buf, oneKey.key, len);
			keys[oneKey.id - 1].key = buf;
		}
		else if( 0 == oneKey.id)
			oneKey.id = -1;
		else {
			printf("Could not read key with ID = %d. Exit now!\n\n", oneKey.id);
			ret = false;
		}
	}
	free(line);
	fclose(fp);
	return ret;
}

bool EncKeys::parseLine( const char *line)
{
    if( isComment(line)) {
    	oneKey.id = 0;
    	return false;
    }

    const char *error_p;
    int offset;

    pcre *pattern = pcre_compile(
            "([0-9]+);([0-9,a-f,A-F]+);([0-9,a-f,A-F]+)",
            0,
            &error_p,
            &offset,
            NULL);
    if( NULL != error_p ) {
        fprintf(stderr, "Error: %s\n", error_p);
        fprintf(stderr, "Offset: %d\n", offset);
    }

    char *buf = (char*) malloc(400*sizeof(char));
    sprintf( buf, "%s", line);
    int rc, m_len = (int) strlen(buf);
    int ovector[30];
    rc = pcre_exec(
            pattern,
            NULL,
            buf,
            m_len,
            0,
            0,
            ovector,
            30
              );
    if( 4 == rc) {
    	char *substring_start = buf + ovector[2];
        int substr_length = ovector[3] - ovector[2];
        char buffer[4];
        sprintf( buffer, "%.*s", substr_length, substring_start );
        oneKey.id = atoi(buffer);

        substring_start = buf + ovector[4];
        substr_length = ovector[5] - ovector[4];
        sprintf( oneKey.iv, "%.*s", substr_length, substring_start );

        substring_start = buf + ovector[6];
        substr_length = ovector[7] - ovector[6];
        sprintf( oneKey.key, "%.*s", substr_length, substring_start );
    }
    else
        return false; //E_WRONG_NUMBER_OF_MATCHES;

    return true;
}

bool EncKeys::isComment( const char *line)
{
    const char *error_p;
    int offset;
    int m_len = (int) strlen(line);

    pcre *pattern = pcre_compile(
            "\\s*#.*",
            0,
            &error_p,
            &offset,
            NULL);
    int rc, ovector[30];
    rc = pcre_exec(
            pattern,
            NULL,
            line,
            m_len,
            0,
            0,
            ovector,
            30
              );
    if( 0 > rc) return false;
    else        return true;
}

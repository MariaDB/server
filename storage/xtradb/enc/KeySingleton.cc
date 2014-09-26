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
@file KeySingleton.cc
Implementation of single pattern to keep keys for encrypting/decrypting pages.

Created 09/13/2014 Florin Fugaciu
***********************************************************************/


#include "KeySingleton.h"
#include <stdlib.h>


bool KeySingleton::instanceInited = false;
KeySingleton KeySingleton::theInstance;
EncKeys KeySingleton::encKeys;



KeySingleton & KeySingleton::getInstance() {
	if( !instanceInited) {
		printf("Encryption / decryption keys were not initialized. I'll exit.\n\n");
		exit(ERROR_NOINITIALIZEDKEYS);
	}
	return theInstance;
}

KeySingleton & KeySingleton::getInstance(const char *name, const char *url, const int initType) {
	if(instanceInited)	return theInstance;

	instanceInited = encKeys.initKeys(name, url, initType);
	if( !instanceInited) {
		printf("Could not initialize the encryption / decryption keys. I'll exit.\n\n");
		exit(ERROR_NOINITIALIZEDKEYS);
	}

	return theInstance;
}

keyentry *KeySingleton::getKeys(int id) {
	return encKeys.getKeys(id);
}


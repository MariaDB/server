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
@file KeySingletonPattern.h
Implementation of single pattern to keep keys for encrypting/decrypting pages.

Created 09/13/2014
***********************************************************************/


#ifndef KEYSINGLETON_H_
#define KEYSINGLETON_H_

#include "EncKeys.h"


class KeySingleton
{
private:
	static bool instanceInited;
	static KeySingleton theInstance;
	static EncKeys encKeys;

	// No new instance or object possible
	KeySingleton() {}

	// No new instance possible through copy constructor
	KeySingleton( const KeySingleton&) {}

	// No new instance possible through copy
	KeySingleton & operator = (const KeySingleton&);

public:
	virtual ~KeySingleton() {encKeys.~EncKeys();}
	static KeySingleton& getInstance();
	// Init the instance for only one time
	static KeySingleton& getInstance(const char *filename, const char *filekey);
	keyentry *getKeys(int id);
	bool hasKey(int id);
	static bool isAvailable() {
		return instanceInited;
	}
};

#endif /* KEYSINGLETON_H_ */

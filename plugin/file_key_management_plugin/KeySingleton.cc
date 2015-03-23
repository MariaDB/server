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
@file KeySingleton.cc
Implementation of single pattern to keep keys for encrypting/decrypting pages.

Created 09/13/2014
***********************************************************************/


#include <my_global.h>
#include <sql_class.h>
#include "KeySingleton.h"

bool KeySingleton::instanceInited = false;
KeySingleton KeySingleton::theInstance;
EncKeys KeySingleton::encKeys;

KeySingleton & KeySingleton::getInstance()
{
#ifndef DBUG_OFF
  if( !instanceInited)
  {
    sql_print_error("Encryption / decryption keys were not initialized. "
                    "You can not read encrypted tables or columns\n");
  }
#endif /* DBUG_OFF */
  return theInstance;
}

KeySingleton & KeySingleton::getInstance(const char *filename,
                                         const char *filekey)
{
  if (!instanceInited)
  {
    if (!(instanceInited = encKeys.initKeys(filename, filekey)))
      sql_print_error("Could not initialize any of the encryption / "
                      "decryption keys. You can not read encrypted tables");
  }    
  return theInstance;
}

keyentry *KeySingleton::getKeys(int id)
{
  return encKeys.getKeys(id);
}

bool KeySingleton::hasKey(int id)
{
  return encKeys.getKeys(id) != NULL;
}

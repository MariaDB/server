/******************************************************************//**
@file EperiKeySingleton-t.cc
Implementation of single pattern to keep keys for encrypting/decrypting pages.

Created 09/15/2014
***********************************************************************/

#include "EperiKeySingleton-t.h"
#include <KeySingleton.h>
#include <stdio.h>
#include <stdlib.h>
#include <tap.h>
#include <my_sys.h>
#include <string.h>


EperiKeySingleton::EperiKeySingleton() {
}

EperiKeySingleton::~EperiKeySingleton() {
}




void printEntry(struct keyentry *entry, uint id)
{
	if( NULL == entry)
		printf("No such keyID = %d\n", id);
	else
		printf("%3u. id:%3u \tiv:%s \tkey:%s\n", id, entry->id, entry->iv, entry->key);
}


int main()
{
	plan(1);
	#ifdef SINGLETON_TEST_DATA

	printf("%s\n", "main() EperiKeySingleton.cc");
	printf("%s\n", SINGLETON_TEST_DATA);
	KeySingleton& ksp = KeySingleton::getInstance( "keys.txt", SINGLETON_TEST_DATA, 1, "secret");
	printEntry(ksp.getKeys(0), 0);

	return EXIT_SUCCESS;
	#else

	#endif

}

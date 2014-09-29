/******************************************************************//**
@file EperiKeySingleton-t.cc
Implementation of single pattern to keep keys for encrypting/decrypting pages.

Created 09/15/2014 Florin Fugaciu
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

void printAll(KeySingleton& ksp, uint length)
{
	int len = (MAX_KEYS <= length ? MAX_KEYS : length);
    for( int ii=1; ii<=len; ii++)
    	printEntry(ksp.getKeys(ii), ii);
}


int main()
{
//	plan(9);
	printf("%s\n", "main() EperiKeySingleton.cc");

	KeySingleton& ksp = KeySingleton::getInstance( "keys.txt", "/home/florin/w/cxx/build-mariadb/unittest/eperi/", KEYINITTYPE_FILE);
	printEntry(ksp.getKeys(0), 0);

/*
	EncKeys encKeys;
	encKeys.initKeys("keys.txt", "/home/florin/w/cxx/build-mariadb/unittest/eperi/", KEYINITTYPE_FILE);
	printEntry(encKeys.getKeys(0), 0);
*/


	printAll(ksp, 256);
	ok(ksp.getKeys(1)->id == 1, "Key id 1 is present");
	ok(!strcmp(ksp.getKeys(2)->iv,"35B2FF0795FB84BBD666DB8430CA214E"), "Testing IV value of key 2");
	ok(!strcmp(ksp.getKeys(15)->key, "B374A26A71490437AA024E4FADD5B497FDFF1A8EA6FF12F6FB65AF2720B59CCF"),"Testing key value of key 15");
	ok((NULL == ksp.getKeys(47)->key), "Key id 47 should be null.");
	ok(ksp.getKeys(255)->id == 255, "Last possible key to insert");
	ok((NULL == ksp.getKeys(256)), "Cannot insert more keys than defined.");

	KeySingleton& ksp1 = KeySingleton::getInstance("keys.txt", "/home/florin/w/cxx/build-mariadb/unittest/eperi", KEYINITTYPE_FILE);
	printEntry(ksp1.getKeys(1), 1);

	KeySingleton& ksp2 = KeySingleton::getInstance();
	printEntry(ksp2.getKeys(2), 2);

	return EXIT_SUCCESS;
}

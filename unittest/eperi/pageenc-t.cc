/*
 * pageenc.cc
 *
 *  Created on: 23.08.2014
 *      Author: florin
 */
#define EP_UNIT_TEST 1
#define UNIV_INLINE
typedef unsigned char byte;
typedef unsigned long int ulint;
typedef unsigned long int ibool;

#include "pageenc-t.h"
#include <stdio.h>
#include <string.h>

#include <tap.h>

extern int summef(int a, int b);
extern int summef2(int a, int b);
extern int multiplikation(int a, int b);


extern byte*
fil_encrypt_page(
/*==============*/
    ulint		space_id,      /*!< in: tablespace id of the
                       table. */
    byte*           buf,           /*!< in: buffer from which to write; in aio
                       this must be appropriately aligned */
    byte*           out_buf,       /*!< out: compressed buffer */
    ulint           len,           /*!< in: length of input buffer.*/
    ulint           compression_level, /*!< in: compression level */
    ulint*          out_len   /*!< out: actual length of compressed page */
    );

/****************************************************************//**
For page encrypted pages decrypt the page after actual read
operation.
@return decrypted page */
extern ulint
fil_decrypt_page(
/*================*/
		byte*		page_buf,      /*!< in: preallocated buffer or NULL */
		byte*		buf,           /*!< out: buffer from which to read; in aio
		                       this must be appropriately aligned */
		ulint		len,           /*!< in: length of output buffer.*/
	    ulint*		write_size,    /*!< in/out: Actual payload size of the decrypted data. */
	    ibool*      page_compressed
	    );









byte* readFile(char* fileName) {
FILE *fileptr;
byte *buffer;
long filelen;

fileptr = fopen(fileName, "rb");  // Open the file in binary mode
fseek(fileptr, 0, SEEK_END);          // Jump to the end of the file
filelen = ftell(fileptr);             // Get the current byte offset in the file
rewind(fileptr);                      // Jump back to the beginning of the file

buffer = (byte *)malloc((filelen+1)*sizeof(byte)); // Enough memory for file + \0
fread(buffer, filelen, 1, fileptr); // Read in the entire file
fclose(fileptr); // Close the file
return buffer;
}


void testIt(char* filename, ulint cmp_checksum) {
	byte* buf = readFile(filename);
	byte* dest = (byte *) malloc(16384*sizeof(byte));
	ulint out_len;
	fil_encrypt_page(0,buf,dest,0,255,&out_len);

	fil_decrypt_page(NULL, dest, 16384 ,NULL,NULL);
	ulint a = 0;
	ulint b = 0;
	if (cmp_checksum) {
		a = 4;
		b = 8;
	}
	ulint i = memcmp(buf + a,dest +a, 16384 - (a+b));

	char str[80];
	strcpy (str,"File ");
	strcat (str,filename );

	ok  (i==0, str);
}
void test_page_enc_dec() {
testIt("xaa",0);
	testIt("xab",0);
	testIt("xac",0);
	testIt("xad",0);


	testIt("xae",1);
	testIt("xaf",1);
}


int main()
{


	test_page_enc_dec();

	return 0;
}

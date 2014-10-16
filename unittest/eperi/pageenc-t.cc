/*
 * pageenc.cc
 *
 *  Created on: 23.08.2014
 */
//#define UNIV_INLINE
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
extern "C" {
extern int my_aes_decrypt_cbc(const char* source, unsigned long int source_length,
		char* dest, unsigned long int *dest_length,
		const unsigned char* key, uint8 key_length,
		const unsigned char* iv, uint8 iv_length);
}
ulint
mach_read_from_2(
/*=============*/
	const byte*	b)	/*!< in: pointer to 2 bytes */
{
	return(((ulint)(b[0]) << 8) | (ulint)(b[1]));
}
ulint
mach_read_from_1(
/*=============*/
	const byte*	b)	/*!< in: pointer to 1 bytes */
{
	return((ulint)(b[0]));
}

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
    ulint*          out_len,   /*!< out: actual length of compressed page */
    ulint			mode       /*!< in: calling mode */
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
	    ibool*      page_compressed,
	    ulint 		mode /*!<in: calling mode, useful for unit test, etc. */
	    );









byte* readFile(char* fileName, int* fileLen) {
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
if (fileLen!=NULL)
	*fileLen = filelen;
return buffer;
}

void testEncryptionChecksum(char* filename) {
	byte* buf = readFile(filename,NULL);
	byte* dest = (byte *) malloc(16384*sizeof(byte));
	ulint out_len;
	fil_encrypt_page(0,buf,dest,0,255, &out_len, 1);
	dest[2000]=0xFF;
	dest[2001]=0xFF;
	dest[2002]=0xFF;
	dest[2003]=0xFF;

	ulint result = fil_decrypt_page(NULL, dest, 16384 ,NULL,NULL, 1);

	char str[80];
	strcpy (str,"Detect decryption error in ");
	strcat (str,filename );


	ok  (result == 1, "%s encryption result %lu", (char*) str, result);

}

void testIt(char* filename, ulint do_not_cmp_checksum) {
	byte* buf = readFile(filename, NULL);
	byte* dest = (byte *) malloc(16384*sizeof(byte));
	ulint out_len;
	ulint cc1 = 0;

	ulint orig_page_type = mach_read_from_2(buf + 24);
	byte* snd = fil_encrypt_page(0,buf,dest,0,255, &out_len, 1);
	cc1 = (buf!=dest);
	cc1 = cc1 && (snd==dest);
	if (!do_not_cmp_checksum) {
		/* verify page type and enryption key*/
		cc1 = cc1 && (mach_read_from_2(dest+1) == orig_page_type);
		/* 255 is the key used for unit test */
		cc1 = cc1 && (mach_read_from_1(dest) == 255);
	}
	ulint result = fil_decrypt_page(NULL, dest, 16384 ,NULL,NULL, 1);
	cc1 = result == 0;
	ulint a = 0;
	ulint b = 0;
	if (do_not_cmp_checksum) {
		a = 4;
		b = 8;
	}
	ulint i = memcmp((buf + a),(dest +a), (size_t)(16384 - (a+b)));

	char str[80];
	strcpy (str,"File ");
	strcat (str,filename );

	ok  (i==0 && cc1, "%s", (char*) str);
}
void test_page_enc_dec() {
	char compressed[] = "compressed";
	char compressed_full[] = "compressed_full";
	char xaa[] = "xaa";
	char xab[] = "xab";
	char xac[] = "xac";
	char xad[] = "xad";
	char xae[] = "xae";
	char xaf[] = "xaf";

	testIt(compressed,0);
	testIt(compressed_full,0);

	testIt(xaa,0);
	testIt(xab,0);
	testIt(xac,0);
	testIt(xad,0);


	testIt(xae,1);
	testIt(xaf,1);

}

void
test_cbc_secret_txt()
{
	printf("%d", strlen("Salted__"));
	int len = 0;
	byte* buf = readFile((char*)"secret.txt",&len);
	byte* dest = (byte *) malloc(16384*sizeof(byte));



	ulint dest_len = 0;
    unsigned char key[32] = {0xFF,0xFF,0xFF,0xEE,0xEE,0xEE,0xFF,0xFF,0xFF,0xEE,0xEE,0xEE,0xAA,0xAA,0xAA,0xAA,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0};
	uint8 k_len = 32;
	unsigned char iv[16] = {0xFF,0xFF,0xFF,0xEE,0xEE,0xEE,0xFF,0xFF,0xFF,0xEE,0xEE,0xEE,0xAA,0xAA,0xAA,0xAA};
	uint8 i_len = 16;

	my_aes_decrypt_cbc((char*)buf, len, (char*)dest , &dest_len, (unsigned char*) &key, k_len, (unsigned char*) &iv, i_len);
	ulint cc1 = (strncmp((char*)dest, "secret\n", dest_len)==0);
	char result[10] = "123451234";
	memcpy(result,dest,10);
	result[dest_len]='\0';
	ok(cc1 ,"Result is secret -> %s", result);
	ulint cc2 = dest_len==7;
	ok(cc2 ,"Result length is %lu.",dest_len);

}


int main()
{

	test_cbc_secret_txt();
//	test_page_enc_dec();
//	testEncryptionChecksum((char* )"xaa");

	return 0;
}

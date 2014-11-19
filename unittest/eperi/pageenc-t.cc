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
#include "EncKeys.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <tap.h>
#define FIL_PAGE_TYPE_FSP_HDR	8	/*!< File space header */
#define FIL_PAGE_TYPE_XDES	9	/*!< Extent descriptor page */
#define PAGE_ENCRYPTION_WILL_NOT_ENCRYPT  5


extern int summef(int a, int b);
extern int summef2(int a, int b);
extern int multiplikation(int a, int b);
extern ulint fil_page_encryption_calc_checksum(unsigned char* buf, ulint len);
extern "C" {
extern int my_aes_decrypt_cbc(const char* source, unsigned long int source_length,
		char* dest, unsigned long int *dest_length,
		const unsigned char* key, uint8 key_length,
		const unsigned char* iv, uint8 iv_length);
}
void
mach_write_to_4(
/*============*/
	byte*	b,	/*!< in: pointer to four bytes where to store */
	ulint	n)	/*!< in: ulint integer to be stored */
{

	b[0] = (byte)(n >> 24);
	b[1] = (byte)(n >> 16);
	b[2] = (byte)(n >> 8);
	b[3] = (byte) n;
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
    ulint* 		    errorCode,   	/*!< out: an error code. set, if page is intentionally not encrypted */
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
	int fl  = 0;
	byte* buf = readFile(filename,&fl);
	byte* dest = (byte *) malloc(16384*sizeof(byte));
	ulint out_len;
	ulint ec = 0;
	fil_encrypt_page(0,buf,dest,fl,255, &out_len, &ec, 1);
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

void testIt(char* filename, ulint do_not_cmp_checksum, ulint page_compressed, ulint input_size) {
	int fl = 0;
	byte* buf = readFile(filename, &fl);
	char str[80];
		strcpy (str,"File ");
		strcat (str,filename );


	byte* dest = (byte *) malloc(16384*sizeof(byte));

	ulint out_len;
	ulint cc1 = 0;
	ulint ec = 0;

	ulint orig_page_type = mach_read_from_2(buf + 24);
	ulint compressed_page =0;
	if (orig_page_type == 0x8632) {
		compressed_page = 1;
	}

	byte* snd = fil_encrypt_page(0,buf,dest,page_compressed? input_size : fl,255, &out_len,  &ec, fl==8192 ? fl : 1);
	if ((orig_page_type ==8) || (orig_page_type==9)) {
		ulint cc2 = memcmp(buf,snd,fl) == 0;
		cc1 = (ec == 5) && cc2;
		ok(cc1, "page type 8 or 9 will not be encrypted! file %s", (char*) str);
		return;
	}

	cc1 = (buf!=dest);
	if (compressed_page) {
		ulint write_size = mach_read_from_1(dest+3);
		cc1 = cc1 && (pow(2,write_size) ==fl);
	}
	cc1 = cc1 && (snd==dest);
	if (!do_not_cmp_checksum) {
		/* verify page type and enryption key*/
		cc1 = cc1 && (mach_read_from_2(dest+1) == orig_page_type);
		/* 255 is the key used for unit test */
		cc1 = cc1 && (mach_read_from_1(dest) == 255);
	}
	if (page_compressed) {
		memcpy (dest+out_len, buf+out_len, fl-out_len);
	}
	ulint write_size = 0;
	ulint result = fil_decrypt_page(NULL, dest, page_compressed? fl: out_len,&write_size,NULL,  out_len==8192 ? out_len : 1);
	cc1 = (result == 0) && (page_compressed? (write_size = out_len):(write_size==fl));
	ulint a = 0;
	ulint b = 0;
	if (do_not_cmp_checksum) {
		a = 4;
		b = 0;
	}
	ulint i = memcmp((buf + a),(dest +a), (size_t)(write_size - (a+b)));

	cc1 = (i==0) && (cc1);
	if (!cc1) {
		//dump_buffer(fl, buf);

		//dump_buffer(write_size, dest);
	}
	if (page_compressed) {
		ok(cc1, "%s %s write size: %lu", str, "page_compressed", out_len );
	}
	ok  (cc1, "%s", (char*) str);
}
void testIt(char* filename, ulint do_not_cmp_checksum) {
	testIt(filename, do_not_cmp_checksum, 0, 0);
}
void test_page_enc_dec() {
	char compressed[] = "compressed";
	char compressed_full[] = "compressed_full";
	char compressed_6bytes_av[] = "compressed_6bytes_av";

	char xaa[] = "xaa";
	char xab[] = "xab";
	char xac[] = "xac";
	char xad[] = "xad";
	char xae[] = "xae";
	char xaf[] = "xaf";



	testIt("row_format_compressedaa", 0);
	testIt("row_format_compressedab", 0);
	testIt("row_format_compressedac", 0);
	testIt("row_format_compressedad", 0);

	testIt("row_format_dynamicaa", 0);
	testIt("row_format_dynamicab", 0);
	testIt("row_format_dynamicac", 0);
	testIt("row_format_dynamicad", 0);

	testIt("row_format_redundantaa", 0);
	testIt("row_format_redundantab", 0);
	testIt("row_format_redundantac", 0);
	testIt("row_format_redundantad", 0);

	testIt("row_format_compactaa", 0);
	testIt("row_format_compactab", 0);
	testIt("row_format_compactac", 0);
	testIt("row_format_compactad", 0);


	testIt(compressed,0, 1, 16384);
	testIt(compressed_full, 0, 1, 16384);
	testIt(compressed_6bytes_av, 0, 1, 16384);

	testIt(compressed,0, 1, 4096);


	testIt(xaa,0);
	testIt(xab,0);
	testIt(xac,0);
	testIt(xad,0);

// empty pages
	testIt(xae,1);
	testIt(xaf,1);

}

char* __sub(size_t length, char* in) {
	in[length-1] = '\0';
	return in;
}
void testSecret(char* filename, char* cmp) {
	char* s = (char*) malloc (1000);
	EncKeys::parseSecret(filename,s);
	int c = strcmp (s, cmp);
	//printf("\n%s\n%s\n",s,cmp);
	ok(c==0,"secret can be decrypted");
	free (s);
}
void testShortSecret_EncryptedFile() {
	testSecret("secret.enc", "secret");
}
void testShortSecret_PlainFile() {
	testSecret("secret", "secret");
}
void testLongSecret_PlainFile() {
	char * s = (char*) "2304832408230498 3094823084092384093824908234 480 32480923840981309548sdmflösdkmflkjmfokjmk4rlkwemflkjrl23409098dsk39i980938098098234098098sdkfölklök1230980sd2304983209483209489fklödkfölk3209483209480932482309480923480923480923480923840932840923840932843399";
	char * x = (char* )malloc(EncKeys::MAX_SECRET_SIZE+1);
	memcpy(x,s,EncKeys::MAX_SECRET_SIZE);
	x[EncKeys::MAX_SECRET_SIZE] = '\0';

	testSecret((char*)"long_secret", x);

}
void testLongSecret_EncryptedFile() {
	char * s = (char*) "2304832408230498 3094823084092384093824908234 480 32480923840981309548sdmflösdkmflkjmfokjmk4rlkwemflkjrl23409098dsk39i980938098098234098098sdkfölklök1230980sd2304983209483209489fklödkfölk3209483209480932482309480923480923480923480923840932840923840932843399";
	char * x = (char* )malloc(EncKeys::MAX_SECRET_SIZE+1);
	memcpy(x,s,EncKeys::MAX_SECRET_SIZE);
	x[EncKeys::MAX_SECRET_SIZE] = '\0';

	testSecret("long_secret.enc", x);

}
void testSecret256_EncryptedFile() {
	char * s = (char*) "423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423";
	char * x = (char* )malloc(EncKeys::MAX_SECRET_SIZE+1);
	memcpy(x,s,EncKeys::MAX_SECRET_SIZE);
	x[EncKeys::MAX_SECRET_SIZE] = '\0';

	testSecret("secret256.enc", x);

}
void testSecret256_PlainFile() {
	char * s = (char*) "423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423480980928309482423";
	char * x = (char* )malloc(EncKeys::MAX_SECRET_SIZE+1);
	memcpy(x,s,EncKeys::MAX_SECRET_SIZE);
	x[EncKeys::MAX_SECRET_SIZE] = '\0';

	testSecret("secret256", x);

}
void testSecrets() {


	testShortSecret_EncryptedFile();
	testShortSecret_PlainFile();
	testLongSecret_PlainFile();
	testLongSecret_EncryptedFile();
	testSecret256_PlainFile();
	testSecret256_EncryptedFile();

}

int main()
{


	testSecrets();
	test_page_enc_dec();
	testEncryptionChecksum((char* )"xab");

	return 0;
}

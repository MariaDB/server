#define EP_UNIT_TEST 1
#define UNIV_INLINE
typedef unsigned char byte;
typedef unsigned long int ulint;
typedef unsigned long int ibool;

#include <tap.h>
#include <my_aes.h>
#include <string.h>
#include <my_dbug.h>
#include <openssl/aes.h>
#include "../../storage/xtradb/include/fil0pageencryption.h"


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
    ulint           unit_test);

/****************************************************************//**
For page encrypted pages decrypt the page after actual read
operation.
@return decrypted page */
extern void
fil_decrypt_page(
/*================*/
		byte*		page_buf,      /*!< in: preallocated buffer or NULL */
		byte*		buf,           /*!< out: buffer from which to read; in aio
		                       this must be appropriately aligned */
		ulint		len,           /*!< in: length of output buffer.*/
	    ulint*		write_size,    /*!< in/out: Actual payload size of the decrypted data. */
	    ulint       unit_test);





#define MY_AES_TEST_TEXTBLOCK "abcdefghijklmnopqrstuvwxyz\
	ABCDEFGHIJKLMNOPQRSTUVW\
	1234567890ß^!\"§$%&/()=?`\
	öäüÖÄÜ+*#',.-;:_~’µ<>|³²¹¼\
	½¬{[]}æ“¢ð€đŋħłµ”øþ@¶ſŧ↓„ł«»←\
	abcdefghijklmnopqrstuvwxyz\
	ABCDEFGHIJKLMNOPQRSTUVW\
	1234567890ß^!\"§$%&/()=?`\
	öäüÖÄÜ+*#',.-;:_~’µ<>|³²¹¼\
	½¬{[]}æ“¢ð€đŋħłµ”øþ@¶ſŧ↓„ł«»←\
	abcdefghijklmnopqrstuvwxyz\
	ABCDEFGHIJKLMNOPQRSTUVW\
	1234567890ß^!\"§$%&/()=?`\
	öäüÖÄÜ+*#',.-;:_~’µ<>|³²¹¼\
	½¬{[]}æ“¢ð€đŋħłµ”øþ@¶ſŧ↓„ł«»←\
	abcdefghijklmnopqrstuvwxyz\
	ABCDEFGHIJKLMNOPQRSTUVW\
	1234567890ß^!\"§$%&/()=?`\
	öäüÖÄÜ+*#',.-;:_~’µ<>|³²¹¼\
	½¬{[]}æ“¢ð€đŋħłµ”øþ@¶ſŧ↓„ł«»←\
	abcdefghijklmnopqrstuvwxyz\
	ABCDEFGHIJKLMNOPQRSTUVW\
	1234567890ß^!\"§$%&/()=?`\
	öäüÖÄÜ+*#',.-;:_~’µ<>|³²¹¼\
	½¬{[]}æ“¢ð€đŋħłµ”øþ@¶ſŧ↓„ł«»←\
	"

#define MY_AES_TEST_JOSHUA " David Lightman: [typing] What is the primary goal?\
Joshua: You should know, Professor. You programmed me.\
David Lightman: Oh, come on.\
David Lightman: [typing] What is the primary goal?\
Joshua: To win the game.\
"


byte* readFile(char* fileName) {
FILE *fileptr;
byte *buffer;
long filelen;

fileptr = fopen(fileName, "rb");  // Open the file in binary mode
fseek(fileptr, 0, SEEK_END);          // Jump to the end of the file
filelen = ftell(fileptr);             // Get the current byte offset in the file
rewind(fileptr);                      // Jump back to the beginning of the file

buffer = (char *)malloc((filelen+1)*sizeof(char)); // Enough memory for file + \0
fread(buffer, filelen, 1, fileptr); // Read in the entire file
fclose(fileptr); // Close the file
return buffer;
}

void
test_cbc_wrong_keylength()
{
	plan(2);
	char* source = "Joshua: Shall we play a game";
	ulint s_len = (ulint)strlen(source);
	char* key="899C0ECB592B2CEE46E64191B6E6DE9B97D8A8EEA43BEF78";
	uint8 k_len = 6;
	char* iv = "F0974007D619466B9EBF8D4F6E302AA3";
	uint8 i_len = 16;
	char* dest = (char *) malloc(2*s_len*sizeof(char));
	unsigned long int dest_len = 0;

	int rc = my_aes_encrypt_cbc(source, s_len, dest, &dest_len, key, k_len, iv, i_len);
	ok(rc == -5, "Encryption - wrong keylength was detected.");
	rc = my_aes_decrypt_cbc(source, s_len, dest, &dest_len, key, k_len, iv, i_len);
	ok(rc == -5, "Decryption - wrong keylength was detected.");
}

void
test_cbc_keysizes()
{
	plan(2);
	char* source = MY_AES_TEST_JOSHUA;
	ulint s_len = (ulint)strlen(source);
	char* key="899C0ECB592B2CEE46E64191B6E6DE9B97D8A8EEA43BEF78";
	uint8 k_len = 24;
	char* iv = "F0974007D619466B9EBF8D4F6E302AA3";
	uint8 i_len = 16;
	char* dest = (char *) malloc(2*s_len*sizeof(char));
	ulint dest_len = 0;
	my_aes_encrypt_cbc(source, s_len, dest, &dest_len, key, k_len, iv, i_len);
	source = (char *) malloc(strlen(MY_AES_TEST_TEXTBLOCK) * sizeof(char));
	my_aes_decrypt_cbc(dest , strlen(dest), source, &dest_len, key, k_len, iv, i_len);
	ok(strcmp(source, MY_AES_TEST_JOSHUA),"Decrypted text is identical to original text.");

	key="7B3B8DA94B77F91A6E05037B21AD5F6E86BD4657C45D97BC7FF14313A781B5A3";
	k_len = 32;
	dest = (char *) malloc(2*s_len*sizeof(char));
	my_aes_encrypt_cbc(source, s_len, dest, &dest_len, key, k_len, iv, i_len);
	source = (char *) malloc(strlen(MY_AES_TEST_TEXTBLOCK) * sizeof(char));
	my_aes_decrypt_cbc(dest , strlen(dest), source, &dest_len, key, k_len, iv, i_len);
	ok(strcmp(source, MY_AES_TEST_JOSHUA),"Decrypted text is identical to original text.");
	free(source);
	free(dest);
}

void
test_cbc_large()
{
	plan(1);
	char* source = MY_AES_TEST_TEXTBLOCK;
	ulint s_len = (ulint)strlen(source);

	char* key = "3C5DC9153A6FE5F22516E217C1603BF7";
	uint8 k_len = 16;
	char* iv = "F0974007D619466B9EBF8D4F6E302AA3";
	uint8 i_len = 16;
	char* dest = (char *) malloc( 2* s_len * sizeof(char));
	ulint dest_len = 0;
	dump_buffer(10,source);
	dump_buffer(10,dest);
	my_aes_encrypt_cbc(source, s_len, dest, &dest_len, key, k_len, iv, i_len);
	source = (char *) malloc(strlen(MY_AES_TEST_TEXTBLOCK) * sizeof(char));
	my_aes_decrypt_cbc(dest , strlen(dest), source, &dest_len, key, k_len, iv, i_len);
	ok(strcmp(source, MY_AES_TEST_TEXTBLOCK),"Decrypted text is identical to original text.");
	free(source);
	free(dest);
}

void
test_wrong_key()
{
	plan(1);
		char* source = MY_AES_TEST_TEXTBLOCK;
		ulint s_len = (ulint)strlen(source);

		char* key = "3C5DC9153A6FE5F22516E217C1603BF7";
		uint8 k_len = 16;
		char* iv = "F0974007D619466B9EBF8D4F6E302AA3";
		uint8 i_len = 16;
		char* dest = (char *) malloc( 2* s_len * sizeof(char));
		ulint dest_len = 0;
		dump_buffer(10,source);
		dump_buffer(10,dest);
		my_aes_encrypt_cbc(source, s_len, dest, &dest_len, key, k_len, iv, i_len);

		iv = "F1A74007D619455B9EBF8D4F6E302AA3";
		source = (char *) malloc(strlen(MY_AES_TEST_TEXTBLOCK) * sizeof(char));
		my_aes_decrypt_cbc(dest , strlen(dest), source, &dest_len, key, k_len, iv, i_len);
		ok(strcmp(source, MY_AES_TEST_TEXTBLOCK) != 0,"Using wrong iv results in wrong decryption.");
		free(source);
		free(dest);
}

void
test_cbc()
{
	plan(1);
	int i;
	char source[20];
	for(i=0; i<20; i++) {
		source[i] = 5;
	}
	ulint s_len = 20;
	char dest[32];
	for(i=0; i<32; i++){
		dest[i]=0;
	}
	ulint dest_len = 0;
	char* key = "583BE7F334F85E7D9DDB362E9AC38151";
	uint8 k_len = 16;
	char* iv = "3325CC3F02203FB6B849990042E58BCB";
	uint8 i_len = 16;
	int ec = my_aes_encrypt_cbc(source, s_len, &dest, &dest_len, key, k_len, iv, i_len);
	ok(ec == AES_OK, "Checking return code.");
	for(i=0; i<20; i++) {
		source[i] = 0;
	}
	my_aes_decrypt_cbc(dest , dest_len, &source, &dest_len, key, k_len, iv, i_len);
	ok(strcmp(source, "Beam me up, Scotty."),"Decrypted text is identical to original text.");

}

void
test_cbc_resultsize()
{
	plan(2);
	char *source = (char*) malloc(5000*sizeof(char));
	source = "abcdefghijklmnopqrstfjdklfkjdsljsdlkfjsaklföjsfölkdsjfölsdkjklösjsdklfjdsklöfjsdalökfjdsklöjfölksdjfklösdajfklösdaj";
	ulint s_len = (ulint) strlen(source);
	char* dest = (char *) malloc(2 * s_len * sizeof(char));
	ulint d_len = 0;
	char* key = "583BE7F334F85E7D9DDB362E9AC38151";
	uint8 k_len = 16;
	char* iv = "3325CC3F02203FB6B849990042E58BCB";
	uint8 i_len = 16;
	my_aes_encrypt_cbc(source, s_len, dest, &d_len, key, k_len, iv, i_len);
	ok(d_len==128, "Destination length ok.");
}

void test_cbc_enc_dec() {
    unsigned char inbuf[1024]="Hello,world!";
unsigned char encbuf[1024];

unsigned char key32[] = {0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa};
unsigned char deckey32[] = {0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa}
;
unsigned char iv[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
unsigned char deciv[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

AES_KEY aeskey;
AES_KEY aesdeckey;

//Now enrypt
memset(encbuf, 0, sizeof(encbuf));
AES_set_encrypt_key(key32, 32*8, &aeskey);
AES_cbc_encrypt(inbuf, encbuf, 16, &aeskey, iv, AES_ENCRYPT);

//Now decrypt
unsigned char decbuf[1024];
memset(decbuf, 0, sizeof(decbuf));

AES_set_decrypt_key(deckey32, 32*8, &aesdeckey);
AES_cbc_encrypt(encbuf, decbuf, 16, &aesdeckey, deciv, AES_DECRYPT);


int i = memcmp(decbuf,inbuf,16);
ok (i==0, "in==out");

}

void test_cbc_enc_dec2() {
    unsigned char inbuf[1024]="Hello,world!";
unsigned char encbuf[1024];

unsigned char key32[] = {0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa};
unsigned char deckey32[] = {0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa}
;
unsigned char iv[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
unsigned char deciv[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

AES_KEY aeskey;
AES_KEY aesdeckey;

//Now enrypt
memset(encbuf, 0, sizeof(encbuf));
AES_set_encrypt_key(key32, 32*8, &aeskey);
AES_cbc_encrypt(inbuf, encbuf, 16, &aeskey, iv, AES_ENCRYPT);

//Now decrypt
unsigned char decbuf[1024];
memset(decbuf, 0, sizeof(decbuf));

AES_set_decrypt_key(deckey32, 32*8, &aesdeckey);
AES_cbc_encrypt(encbuf, decbuf, 16, &aesdeckey, deciv, AES_DECRYPT);
dump_buffer(16, decbuf);
dump_buffer(16, encbuf);

int i = memcmp(decbuf,inbuf,16);
ok (i==0, "in==out");

}



void test_cbc_enc_() {
    unsigned char inbuf[1024]="Hello,world!";
unsigned char encbuf[1024];

unsigned char key32[] = {0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa};
unsigned char deckey32[] = {0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa}
;
unsigned char iv[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
unsigned char deciv[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

AES_KEY aeskey;
AES_KEY aesdeckey;

//Now enrypt
memset(encbuf, 0, sizeof(encbuf));
AES_set_encrypt_key(key32, 32*8, &aeskey);
AES_cbc_encrypt(inbuf, encbuf, 16, &aeskey, iv, AES_ENCRYPT);

//Now decrypt
unsigned char decbuf[1024];
memset(decbuf, 0, sizeof(decbuf));

AES_set_decrypt_key(deckey32, 32*8, &aesdeckey);
AES_cbc_encrypt(encbuf, decbuf, 16, &aesdeckey, deciv, AES_DECRYPT);
dump_buffer(16, decbuf);
dump_buffer(16, encbuf);

int i = memcmp(decbuf,inbuf,16);
ok (i==0, "in==out");

}



void test_page_enc_dec() {
	char* buf = readFile("xaa");
	char* dest = (char *) malloc(16384*sizeof(char));
	//fil_encrypt_page(0,buf,dest,0,0,NULL,1);

	//fil_decrypt_page(NULL, dest, 0,NULL,1);

	ulint i = memcmp(buf,dest, 16384);
	ok  (i==0, "in==out");
}

/*
 * Test if bytes for AES Key and IV are generated in the same way as in openssl commandline.
 */
void
test_bytes_to_key()
{
	plan(2);
	char salt[] = {0x0c, 0x3b, 0x72, 0x1b, 0xfe, 0x07, 0xe2, 0xb3};
	char *secret = "secret";
	char key[32];
	char iv[16];
	char keyresult[32] = {0x2E, 0xFF, 0xB7, 0x1D, 0xDB, 0x97, 0xA8, 0x3A,
			0x03, 0x5A, 0x06, 0xDF, 0xB0, 0xDD, 0x72, 0x29,
			0xA6, 0xD9, 0x1F, 0xFB, 0xE6, 0x06, 0x3B, 0x4B,
			0x81, 0x23, 0x85, 0x45, 0x71, 0x28, 0xFF, 0x1F};
	char ivresult[16] = {0x61, 0xFF, 0xC8, 0x27, 0x5B, 0x46, 0x4C, 0xBD,
			0x55, 0x82, 0x0E, 0x54, 0x8F, 0xE4, 0x44, 0xD9};

	my_bytes_to_key(&salt, secret, &key, &iv);

	ok(memcmp(key, &keyresult, 32) == 0, "BytesToKey key generated successfully.");
	ok(memcmp(iv, &ivresult, 16) == 0, "BytesToKey iv generated successfully.");
}


int
main(int argc __attribute__((unused)),char *argv[])
{
	test_cbc();
	test_cbc_large();
	test_cbc_keysizes();
	test_cbc_wrong_keylength();
	test_cbc_resultsize();
	test_cbc_enc_dec();
	test_wrong_key();
	test_bytes_to_key();
	return 0;
}

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

buffer = (byte *)malloc((filelen+1)*sizeof(char)); // Enough memory for file + \0
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
	unsigned char key[24] = {0x89, 0x9c, 0x0e, 0xcb, 0x59, 0x2b, 0x2c,
			0xee, 0x46, 0xe6, 0x41, 0x91, 0xb6, 0xe6, 0xde, 0x9b, 0x97,
			0xd8, 0xa8, 0xee, 0xa4, 0x3b, 0xef, 0x78 };
	uint8 k_len = 6;
	unsigned char iv[16] = {0xf0, 0x97, 0x40, 0x07, 0xd6, 0x19, 0x46, 0x6b,
	0x9e, 0xbf, 0x8d, 0x4f, 0x6e, 0x30, 0x2a, 0xa3};
	uint8 i_len = 16;
	char* dest = (char *) malloc(2*s_len*sizeof(char));
	unsigned long int dest_len = 0;

	int rc = my_aes_encrypt_cbc(source, s_len, dest, &dest_len,(unsigned char*) &key, k_len,(unsigned char*) &iv, i_len);
	ok(rc == -5, "Encryption - wrong keylength was detected.");
	rc = my_aes_decrypt_cbc(source, s_len, dest, &dest_len,(unsigned char*) &key, k_len,(unsigned char*) &iv, i_len);
	ok(rc == -5, "Decryption - wrong keylength was detected.");
}

void
test_cbc_keysizes()
{
	plan(2);
	char* source = MY_AES_TEST_JOSHUA;
	ulint s_len = (ulint)strlen(source);
	unsigned char key[24] = {0x89, 0x9c, 0x0e, 0xcb, 0x59, 0x2b, 0x2c,
				0xee, 0x46, 0xe6, 0x41, 0x91, 0xb6, 0xe6, 0xde, 0x9b, 0x97,
				0xd8, 0xa8, 0xee, 0xa4, 0x3b, 0xef, 0x78 };
	uint8 k_len = 24;
	unsigned char iv[16] = {0xf0, 0x97, 0x40, 0x07, 0xd6, 0x19, 0x46, 0x6b,
	0x9e, 0xbf, 0x8d, 0x4f, 0x6e, 0x30, 0x2a, 0xa3};
	uint8 i_len = 16;
	char* dest = (char *) malloc(2*s_len*sizeof(char));
	ulint dest_len = 0;
	my_aes_encrypt_cbc(source, s_len, dest, &dest_len, key, k_len, iv, i_len);
	source = (char *) malloc(strlen(MY_AES_TEST_TEXTBLOCK) * sizeof(char));
	my_aes_decrypt_cbc(dest , strlen(dest), source, &dest_len,(unsigned char*) &key, k_len,(unsigned char*) &iv, i_len);
	ok(strcmp(source, MY_AES_TEST_JOSHUA),"Decrypted text is identical to original text.");

	unsigned char key2[32] = {0x7b, 0x3b, 0x8d, 0xa9, 0x4b, 0x77,
					0xf9, 0x1a, 0x6e, 0x05, 0x03, 0x7b,
					0x21, 0xad, 0x5f, 0x6e, 0x86, 0xbd,
					0x46, 0x57, 0xc4, 0x5d, 0x97, 0xbc,
					0xb5, 0xa3};
	k_len = 32;
	dest = (char *) malloc(2*s_len*sizeof(char));
	my_aes_encrypt_cbc(source, s_len, dest, &dest_len, (unsigned char*) &key2, k_len,(unsigned char*) &iv, i_len);
	source = (char *) malloc(strlen(MY_AES_TEST_TEXTBLOCK) * sizeof(char));
	my_aes_decrypt_cbc(dest , strlen(dest), source, &dest_len, (unsigned char*) &key2, k_len, (unsigned char*) &iv, i_len);
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

	unsigned char key[16] = {0x3c, 0x5d, 0xc9, 0x15, 0x3a, 0x6f, 0xe5, 0xf2,
			0x25, 0x16, 0xe2, 0x17, 0xc1, 0x60, 0x3b, 0xf7};
	uint8 k_len = 16;
	unsigned char iv[16] = {0xf0, 0x97, 0x40, 0x00, 0x7d, 0x61, 0x94, 0x66,
			0xb9, 0xeb, 0xf8, 0xd4, 0x6e, 0x30, 0x2a, 0xa3};
	uint8 i_len = 16;
	char* dest = (char *) malloc( 2* s_len * sizeof(char));
	ulint dest_len = 0;
	my_aes_encrypt_cbc(source, s_len, dest, &dest_len, key, k_len, iv, i_len);
	source = (char *) malloc(strlen(MY_AES_TEST_TEXTBLOCK) * sizeof(char));
	my_aes_decrypt_cbc(dest , strlen(dest), source, &dest_len, (unsigned char*) &key, k_len, (unsigned char*) &iv, i_len);
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
		unsigned char key[16] = {0x3c, 0x5d, 0xc9, 0x15, 0x3a, 0x6f, 0xe5, 0xf2,
				0x25, 0x16, 0xe2, 0x17, 0xc1, 0x60, 0x3b, 0xf7};
		uint8 k_len = 16;
		unsigned char iv[16] = {0xf0, 0x97, 0x40, 0x00, 0x7d, 0x61, 0x94, 0x66,
				0xb9, 0xeb, 0xf8, 0xd4, 0x6e, 0x30, 0x2a, 0xa3};
		uint8 i_len = 16;
		char* dest = (char *) malloc( 2* s_len * sizeof(char));
		ulint dest_len = 0;
		my_aes_encrypt_cbc(source, s_len, dest, &dest_len, (unsigned char*) &key, k_len,(unsigned char*) &iv, i_len);

		iv[0] = 0xf1;
		//"F1A74007D619455B9EBF8D4F6E302AA3";
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
    unsigned char key[16] = {0x58, 0x3b, 0xe7, 0xf3, 0x34, 0xf8,
    		0x5e, 0x7d, 0x9d, 0xdb, 0x36, 0x2e, 0x9a, 0xc3, 0x81, 0x51};
	uint8 k_len = 16;
	unsigned char iv[16] = {0x33, 0x25, 0xcc, 0x3f, 0x02, 0x20, 0x3f, 0xb6, 0xb8,
			0x49, 0x99, 0x00, 0x42, 0xe5, 0x8b, 0xcb};
	uint8 i_len = 16;
	int ec = my_aes_encrypt_cbc(source, s_len, (char*) &dest, &dest_len, (unsigned char*) &key, k_len,(unsigned char*) &iv, i_len);
	ok(ec == AES_OK, "Checking return code.");
	for(i=0; i<20; i++) {
		source[i] = 0;
	}
	my_aes_decrypt_cbc(dest , dest_len, (char*)&source, &dest_len, (unsigned char*) &key, k_len, (unsigned char*) &iv, i_len);
	ok(strcmp(source, "Beam me up, Scotty."),"Decrypted text is identical to original text.");

}

void
test_cbc_resultsize()
{
	plan(2);
	char *source = (char*) malloc(5000*sizeof(char));
	source = "abcdefghijklmnopqrstfjdklfkjdsljsdlkfjsaklföjsfölkdsjfölsd"
			"kjklösjsdklfjdsklöfjsdalökfjdsklöjfölksdjfklösdajfklösdaj";
	ulint s_len = (ulint) strlen(source);
	char* dest = (char *) malloc(2 * s_len * sizeof(char));
	ulint d_len = 0;
	unsigned char key[16] = {0x58, 0x3b, 0xe7, 0xf3, 0x34, 0xf8,
	    		0x5e, 0x7d, 0x9d, 0xdb, 0x36, 0x2e, 0x9a, 0xc3, 0x81, 0x51};
	uint8 k_len = 16;
	unsigned char iv[16] = {0x33, 0x25, 0xcc, 0x3f, 0x02, 0x20, 0x3f, 0xb6, 0xb8,
				0x49, 0x99, 0x00, 0x42, 0xe5, 0x8b, 0xcb};
	uint8 i_len = 16;
	my_aes_encrypt_cbc(source, s_len, dest, &d_len, (unsigned char*)&key, k_len, (unsigned char *)&iv, i_len);
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
	unsigned char* buf = readFile("xaa");
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
	unsigned char salt[] = {0x0c, 0x3b, 0x72, 0x1b, 0xfe, 0x07, 0xe2, 0xb3};
	char *secret = "secret";
	char key[32];
	unsigned char iv[16];
    unsigned char keyresult[32] = {0x2E, 0xFF, 0xB7, 0x1D, 0xDB, 0x97, 0xA8, 0x3A,
			0x03, 0x5A, 0x06, 0xDF, 0xB0, 0xDD, 0x72, 0x29,
			0xA6, 0xD9, 0x1F, 0xFB, 0xE6, 0x06, 0x3B, 0x4B,
			0x81, 0x23, 0x85, 0x45, 0x71, 0x28, 0xFF, 0x1F};
    unsigned char ivresult[16] = {0x61, 0xFF, 0xC8, 0x27, 0x5B, 0x46, 0x4C, 0xBD,
			0x55, 0x82, 0x0E, 0x54, 0x8F, 0xE4, 0x44, 0xD9};

	my_bytes_to_key((unsigned char*) &salt, secret, (unsigned char*) &key, (unsigned char*) &iv);

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

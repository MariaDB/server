#define EP_UNIT_TEST 1
#define UNIV_INLINE

#define AES_OK 0
#define AES_BAD_DATA  -1
#define AES_BAD_KEYSIZE -5
#define AES_KEY_CREATION_FAILED -10

#define MY_AES_BLOCK_SIZE 16 /* Block size in bytes */


typedef unsigned char byte;
typedef unsigned long int ulint;
typedef unsigned long int ibool;

#include <tap.h>
#include <string.h>
#include <my_dbug.h>
extern "C" {
extern int my_aes_decrypt_cbc(const char* source, unsigned long int source_length,
		char* dest, unsigned long int *dest_length,
		const unsigned char* key, uint8 key_length,
		const unsigned char* iv, uint8 iv_length,
		int noPadding);


extern int my_aes_encrypt_cbc(const char* source, unsigned long int source_length,
					char* dest, unsigned long int *dest_length,
					const unsigned char* key, uint8 key_length,
					const unsigned char* iv, uint8 iv_length,
					int noPadding);
extern void my_bytes_to_key(const unsigned char *salt, const char *secret, unsigned char *key, unsigned char *iv);
}

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
test_cbc128noPadding()
{
	char expected[]=
	{
	0x51 , 0xBC , 0xF9 , 0x96 , 0xCB , 0x6A , 0x6D , 0x18 , 0x08 , 0xE1 , 0x08 , 0xC5 , 0x07 , 0x78 , 0x70 , 0xA6,
	0x15 , 0x3E , 0x41 , 0x34 , 0xEC , 0x5E , 0xA2 , 0x67 , 0x52 , 0x51 , 0x87 , 0x61 , 0x8A , 0x15 , 0xE0 , 0xD7,
	0x1D , 0x9A , 0x5B , 0x4A , 0xF9 , 0x9F , 0x13 , 0xEE , 0x3B , 0x77 , 0x1E , 0xD1 , 0xF6 , 0x54 , 0xAD , 0xFE
	};
	plan(1);
	int i;
	char* source = "int i = memcmp(decbuf,inbuf,16);";
	ulint s_len = strlen(source);
	char* dest = (char* ) malloc(100);
	char* result = (char*) malloc(100);

	ulint dest_len = 0;
    unsigned char key[16] = {0x58, 0x3b, 0xe7, 0xf3, 0x34, 0xf8,
    		0x5e, 0x7d, 0x9d, 0xdb, 0x36, 0x2e, 0x9a, 0xc3, 0x81, 0x51};
	uint8 k_len = 16;
	unsigned char iv[16] = {0x33, 0x25, 0xcc, 0x3f, 0x02, 0x20, 0x3f, 0xb6, 0xb8,
			0x49, 0x99, 0x00, 0x42, 0xe5, 0x8b, 0xcb};
	uint8 i_len = 16;
	int ec = my_aes_encrypt_cbc(source, s_len, dest, &dest_len, (unsigned char*) &key, k_len,(unsigned char*) &iv, i_len, 1);
	ok(ec == AES_OK, "Checking return code.");
	ok(memcmp(expected,dest,dest_len)==0, "expected cipher text");
	ok(memcmp(source,dest,32)!=0,"plain and cipher text differ");
	ok(dest_len == s_len, "input length = output length, cbc 128 no padding");

	ec = my_aes_decrypt_cbc(dest , dest_len, result, &dest_len, (unsigned char*) &key, k_len, (unsigned char*) &iv, i_len, 1);
	ok((dest_len == s_len) && (ec == AES_OK) && (strncmp(result, "int i = memcmp(decbuf,inbuf,16);",dest_len)==0),"Decrypted text is identical to original text.");
	free(result);
	free(dest);
}

void
test_cbc128()
{
	char expected[]=
	{
	0x51 , 0xBC , 0xF9 , 0x96 , 0xCB , 0x6A , 0x6D , 0x18 , 0x08 , 0xE1 , 0x08 , 0xC5 , 0x07 , 0x78 , 0x70 , 0xA6,
	0x15 , 0x3E , 0x41 , 0x34 , 0xEC , 0x5E , 0xA2 , 0x67 , 0x52 , 0x51 , 0x87 , 0x61 , 0x8A , 0x15 , 0xE0 , 0xD7,
	0x1D , 0x9A , 0x5B , 0x4A , 0xF9 , 0x9F , 0x13 , 0xEE , 0x3B , 0x77 , 0x1E , 0xD1 , 0xF6 , 0x54 , 0xAD , 0xFE
	};
	plan(2);
	int i;
	char* source = "int i = memcmp(decbuf,inbuf,16);";
	ulint s_len = strlen(source);
	char* dest = (char* ) malloc(100);
	char* result = (char*) malloc(100);

	ulint dest_len = 0;
    unsigned char key[16] = {0x58, 0x3b, 0xe7, 0xf3, 0x34, 0xf8,
    		0x5e, 0x7d, 0x9d, 0xdb, 0x36, 0x2e, 0x9a, 0xc3, 0x81, 0x51};
	uint8 k_len = 16;
	unsigned char iv[16] = {0x33, 0x25, 0xcc, 0x3f, 0x02, 0x20, 0x3f, 0xb6, 0xb8,
			0x49, 0x99, 0x00, 0x42, 0xe5, 0x8b, 0xcb};
	uint8 i_len = 16;
	int ec = my_aes_encrypt_cbc(source, s_len, dest, &dest_len, (unsigned char*) &key, k_len,(unsigned char*) &iv, i_len, 0);
	ok(ec == AES_OK, "Checking return code.");
	ok(memcmp(expected,dest,48)==0, "expected cipher text");
	ok(memcmp(source,dest,32)!=0,"plain and cipher text differ");

	ec = my_aes_decrypt_cbc(dest , dest_len, result, &dest_len, (unsigned char*) &key, k_len, (unsigned char*) &iv, i_len, 0);
	ok((dest_len == s_len) && (ec == AES_OK) && (strncmp(result, "int i = memcmp(decbuf,inbuf,16);",dest_len)==0),"Decrypted text is identical to original text.");
	free(result);
	free(dest);
}

void
test_cbc192noPadding()
{

	plan(3);
	int i;
	char* source = "int i = memcmp(decbuf,inbuf,16);";
	ulint s_len = strlen(source);
	char* dest = (char* ) malloc(100);
	char* result = (char*) malloc(100);

	ulint dest_len = 0;
    unsigned char key[24] = {0x58, 0x3b, 0xe7, 0xf3, 0x34, 0xf8,
    		0x5e, 0x7d, 0x9d, 0xdb, 0x36, 0x2e, 0x9a, 0xc3, 0x81, 0x51,
	0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
	uint8 k_len = 24;
	unsigned char iv[16] = {0x33, 0x25, 0xcc, 0x3f, 0x02, 0x20, 0x3f, 0xb6, 0xb8,
			0x49, 0x99, 0x00, 0x42, 0xe5, 0x8b, 0xcb};
	uint8 i_len = 16;
	int ec = my_aes_encrypt_cbc(source, s_len, dest, &dest_len, (unsigned char*) &key, k_len,(unsigned char*) &iv, i_len, 1);
	ok (ec == AES_OK, "Checking return code.");
	ok (dest_len == s_len, "input length = output length, cbc 192 no padding");
	ec = my_aes_decrypt_cbc(dest , dest_len, result, &dest_len, (unsigned char*) &key, k_len, (unsigned char*) &iv, i_len, 1);
	ok((dest_len == s_len) && (ec == AES_OK) && (strncmp(result, "int i = memcmp(decbuf,inbuf,16);",dest_len)==0),"Decrypted text is identical to original text.");
	free(result);
	free(dest);
}

void
test_cbc192()
{

	plan(4);
	int i;
	char* source = "int i = memcmp(decbuf,inbuf,16);";
	ulint s_len = strlen(source);
	char* dest = (char* ) malloc(100);
	char* result = (char*) malloc(100);

	ulint dest_len = 0;
    unsigned char key[24] = {0x58, 0x3b, 0xe7, 0xf3, 0x34, 0xf8,
    		0x5e, 0x7d, 0x9d, 0xdb, 0x36, 0x2e, 0x9a, 0xc3, 0x81, 0x51,
	0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
	uint8 k_len = 24;
	unsigned char iv[16] = {0x33, 0x25, 0xcc, 0x3f, 0x02, 0x20, 0x3f, 0xb6, 0xb8,
			0x49, 0x99, 0x00, 0x42, 0xe5, 0x8b, 0xcb};
	uint8 i_len = 16;
	int ec = my_aes_encrypt_cbc(source, s_len, dest, &dest_len, (unsigned char*) &key, k_len,(unsigned char*) &iv, i_len, 1);
	ok (ec == AES_OK, "Checking return code.");
	ec = my_aes_decrypt_cbc(dest , dest_len, result, &dest_len, (unsigned char*) &key, k_len, (unsigned char*) &iv, i_len, 1);
	ok((dest_len == s_len) && (ec == AES_OK) && (strncmp(result, "int i = memcmp(decbuf,inbuf,16);",dest_len)==0),"Decrypted text is identical to original text.");
	free(result);
	free(dest);
}

void
test_cbc256()
{
	char expected[] = {
			0x81, 0x22, 0x05, 0xA7, 0x3E, 0x9D, 0xB2, 0x18, 0x7F, 0xE2, 0x5C, 0xB4, 0xBD, 0xCD, 0xFB, 0x9B,
			0xB6, 0xEF, 0x64, 0x2C, 0xF4, 0x53, 0x9B, 0x29, 0x98, 0x3A, 0xD6, 0xDE, 0xB2, 0x65, 0xEF, 0x85,
			0xEF, 0x4B, 0xDA, 0x8F, 0xD9, 0xEB, 0xD7, 0x07, 0x80, 0x03, 0x0E, 0x7C, 0x55, 0x2E, 0x97, 0x47
	};
	plan(5);
	int i;
	char* source = "int i = memcmp(decbuf,inbuf,16);";
	ulint s_len = strlen(source);
	char* dest = (char* ) malloc(100);
	char* result = (char*) malloc(100);

	ulint dest_len = 0;
    unsigned char key[32] = {0x58, 0x3b, 0xe7, 0xf3, 0x34, 0xf8,
    		0x5e, 0x7d, 0x9d, 0xdb, 0x36, 0x2e, 0x9a, 0xc3, 0x81, 0x51,
	0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
	0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
	uint8 k_len = 32;
	unsigned char iv[16] = {0x33, 0x25, 0xcc, 0x3f, 0x02, 0x20, 0x3f, 0xb6, 0xb8,
			0x49, 0x99, 0x00, 0x42, 0xe5, 0x8b, 0xcb};
	uint8 i_len = 16;
	int ec = my_aes_encrypt_cbc(source, s_len, dest, &dest_len, (unsigned char*) &key, k_len,(unsigned char*) &iv, i_len, 0);


	ok(ec == AES_OK, "Checking return code.");
	ok(memcmp(expected,dest,48)==0,"Excepted cipher text - aes 256 cbc");

	ec = my_aes_decrypt_cbc(dest , dest_len, result, &dest_len, (unsigned char*) &key, k_len, (unsigned char*) &iv, i_len, 0);

	ok((dest_len == s_len) && (ec == AES_OK) && (strncmp(result, "int i = memcmp(decbuf,inbuf,16);",dest_len)==0),"Decrypted text is identical to original text.");
	free(result);
	free(dest);
}


void
test_cbc256noPadding()
{
	char expected[] = {
			0x81, 0x22, 0x05, 0xA7, 0x3E, 0x9D, 0xB2, 0x18, 0x7F, 0xE2, 0x5C, 0xB4, 0xBD, 0xCD, 0xFB, 0x9B,
			0xB6, 0xEF, 0x64, 0x2C, 0xF4, 0x53, 0x9B, 0x29, 0x98, 0x3A, 0xD6, 0xDE, 0xB2, 0x65, 0xEF, 0x85,
			0xEF, 0x4B, 0xDA, 0x8F, 0xD9, 0xEB, 0xD7, 0x07, 0x80, 0x03, 0x0E, 0x7C, 0x55, 0x2E, 0x97, 0x47
	};
	plan(6);
	int i;
	char* source = "int i = memcmp(decbuf,inbuf,16);";
	ulint s_len = strlen(source);
	char* dest = (char* ) malloc(100);
	char* result = (char*) malloc(100);

	ulint dest_len = 0;
    unsigned char key[32] = {0x58, 0x3b, 0xe7, 0xf3, 0x34, 0xf8,
    		0x5e, 0x7d, 0x9d, 0xdb, 0x36, 0x2e, 0x9a, 0xc3, 0x81, 0x51,
	0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
	0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
	uint8 k_len = 32;
	unsigned char iv[16] = {0x33, 0x25, 0xcc, 0x3f, 0x02, 0x20, 0x3f, 0xb6, 0xb8,
			0x49, 0x99, 0x00, 0x42, 0xe5, 0x8b, 0xcb};
	uint8 i_len = 16;
	int ec = my_aes_encrypt_cbc(source, s_len, dest, &dest_len, (unsigned char*) &key, k_len,(unsigned char*) &iv, i_len, 1);


	ok(ec == AES_OK, "Checking return code.");
	ok(memcmp(expected,dest,dest_len)==0,"Excepted cipher text - aes 256 cbc");
	ok(s_len==dest_len,"input length = output length, cbc 256 no padding");

	ec = my_aes_decrypt_cbc(dest , dest_len, result, &dest_len, (unsigned char*) &key, k_len, (unsigned char*) &iv, i_len, 1);

	ok((dest_len == s_len) && (ec == AES_OK) && (strncmp(result, "int i = memcmp(decbuf,inbuf,16);",dest_len)==0),"Decrypted text is identical to original text.");
	free(result);
	free(dest);
}



void
test_cbc256noPaddingWrongInputSize()
{
	char expected[] = {
			0x81, 0x22, 0x05, 0xA7, 0x3E, 0x9D, 0xB2, 0x18, 0x7F, 0xE2, 0x5C, 0xB4, 0xBD, 0xCD, 0xFB, 0x9B,
			0xB6, 0xEF, 0x64, 0x2C, 0xF4, 0x53, 0x9B, 0x29, 0x98, 0x3A, 0xD6, 0xDE, 0xB2, 0x65, 0xEF, 0x85,
			0xEF, 0x4B, 0xDA, 0x8F, 0xD9, 0xEB, 0xD7, 0x07, 0x80, 0x03, 0x0E, 0x7C, 0x55, 0x2E, 0x97, 0x47
	};
	plan(7);
	int i;
	char* source = "int i = memcmp(decbuf,inbuf,16);sdfsd";
	ulint s_len = strlen(source);
	char* dest = (char* ) malloc(100);
	char* result = (char*) malloc(100);

	ulint dest_len = 0;
    unsigned char key[32] = {0x58, 0x3b, 0xe7, 0xf3, 0x34, 0xf8,
    		0x5e, 0x7d, 0x9d, 0xdb, 0x36, 0x2e, 0x9a, 0xc3, 0x81, 0x51,
	0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
	0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
	uint8 k_len = 32;
	unsigned char iv[16] = {0x33, 0x25, 0xcc, 0x3f, 0x02, 0x20, 0x3f, 0xb6, 0xb8,
			0x49, 0x99, 0x00, 0x42, 0xe5, 0x8b, 0xcb};
	uint8 i_len = 16;
	int ec = my_aes_encrypt_cbc(source, s_len, dest, &dest_len, (unsigned char*) &key, k_len,(unsigned char*) &iv, i_len, 1);
	ok (ec== AES_BAD_DATA, "wrong input size detected");

	free(result);
	free(dest);
}







/*
 * Test if bytes for AES Key and IV are generated in the same way as in openssl commandline.
 */
void
test_bytes_to_key()
{

	char expected[32] = {
		0x2E, 0xFF , 0xB7 , 0x1D , 0xDB , 0x97 , 0xA8 , 0x3A , 0x03 , 0x5A , 0x06 , 0xDF , 0xB0 , 0xDD , 0x72 , 0x29,
		0xA6, 0xD9 , 0x1F , 0xFB , 0xE6 , 0x06 , 0x3B , 0x4B , 0x81 , 0x23 , 0x85 , 0x45 , 0x71 , 0x28 , 0xFF , 0x1F
	};
	plan(8);
	unsigned char salt[] = {0x0c, 0x3b, 0x72, 0x1b, 0xfe, 0x07, 0xe2, 0xb3};
	char *secret = "secret";
	unsigned char* key = (unsigned char*)malloc(32 * sizeof(char));
	unsigned char iv[16];
    unsigned char keyresult[32] = {0x2E, 0xFF, 0xB7, 0x1D, 0xDB, 0x97, 0xA8, 0x3A,
			0x03, 0x5A, 0x06, 0xDF, 0xB0, 0xDD, 0x72, 0x29,
			0xA6, 0xD9, 0x1F, 0xFB, 0xE6, 0x06, 0x3B, 0x4B,
			0x81, 0x23, 0x85, 0x45, 0x71, 0x28, 0xFF, 0x1F};
    unsigned char ivresult[16] = {0x61, 0xFF, 0xC8, 0x27, 0x5B, 0x46, 0x4C, 0xBD,
			0x55, 0x82, 0x0E, 0x54, 0x8F, 0xE4, 0x44, 0xD9};

	my_bytes_to_key((unsigned char*) &salt, secret, (unsigned char*) key, (unsigned char*) &iv);
	ok(memcmp(key, &keyresult, 32) == 0, "BytesToKey key generated successfully.");
	ok(memcmp(iv, &ivresult, 16) == 0, "BytesToKey iv generated successfully.");
	// following should ensure, that yassl and openssl calculate the same!
	ok(memcmp(expected,key,32)==0, "expected result");
	free(key);
}

int
main(int argc __attribute__((unused)),char *argv[])
{
	test_cbc256noPadding();
	test_cbc192noPadding();
	test_cbc128noPadding();
	test_cbc256noPaddingWrongInputSize();

	test_cbc128();
	test_cbc192();
	test_cbc256();

	test_bytes_to_key();

	
	return 0;
}




//Author Clemens Doerrhoefer

#include <keyfile.h>
#include <my_sys.h>
#include <pcre.h>
#include <my_dbug.h>
#include <string.h>

#define E_WRONG_NUMBER_OF_MATCHES 10
#define MAX_KEY_FILE_SIZE 1048576
#define MAX_BUFFER_LENGTH 512

#define KEY_FILE_PARSE_OK 0
#define KEY_FILE_TOO_BIG 100
#define KEY_BUFFER_TOO_BIG 200
#define KEY_FILE_PARSE_NULL 300
#define KEY_FILE_TOO_MANY_KEYS 400


int
isComment(char *line)
{
    const char *error_p;
    int offset;
    int m_len = (int) strlen(line);

    pcre *pattern = pcre_compile(
            "\\s*#.*",
            0,
            &error_p,
            &offset,
            NULL);
    int rc,i;
    int ovector[30];
    rc = pcre_exec(
            pattern,
            NULL,
            line,
            m_len,
            0,
            0,
            ovector,
            30
              );
    if(rc < 0) {
    	return 0;
    } else {
    	return 1;
    }
}

int
parseFile(FILE * fp, struct keyentry **allKeys, const int k_len, const char *secret)
{
	const char *MAGIC = "Salted__";
	long file_size = 0;
	char *buffer, *decrypted;
	char *line = NULL;
	if(NULL == fp) {
		fprintf(stderr, "Key file not found.\n");
		return 100;
	}

	//get size of file
	fseek(fp, 0L, SEEK_END);
	file_size = ftell(fp);
	fseek(fp, 0L, SEEK_SET);

	if(file_size > MAX_KEY_FILE_SIZE) {
		return KEY_FILE_TOO_BIG;
	}

	//Read file into buffer
	buffer = (char*) malloc((file_size+1)*sizeof(char));
	fread(buffer, file_size, 1, fp);

	//Check for file encryption
	if(memcmp(buffer, MAGIC, 8) == 0) { //If file is encrypted, decrypt it first.
		unsigned char salt[8];
		unsigned char *key = malloc(32 * sizeof(char));
		unsigned char *iv = malloc(16 * sizeof(char));
		decrypted = malloc(file_size * sizeof(char));
		memcpy(&salt, buffer+8, 8);
		my_bytes_to_key(&salt, secret, key, iv);
		unsigned long int d_size = 0;
		my_aes_decrypt_cbc(buffer + 16, file_size -16, decrypted, &d_size, key, 32, iv, 16);
		memcpy(buffer, decrypted, d_size);

		free(decrypted);
		free(key);
		free(iv);
	}

	line = strtok(buffer, "\n");
	while(line != NULL) {
	   struct keyentry *entry = (struct keyentry*) malloc(sizeof(struct keyentry));
	   if( parseLine(line, entry, k_len) == 0) {
			allKeys[entry->id] = entry;
	   }
	   line = strtok(NULL, "\n");
	}
	free(buffer);
	return KEY_FILE_PARSE_OK;
}

int
parseLine(const char *line, struct keyentry *entry, const int k_len)
{
    const char *error_p;
    int offset;

    pcre *pattern = pcre_compile(
            "([0-9]+);([0-9,a-f,A-F]{32});([0-9,a-f,A-F]{64}|[0-9,a-f,A-F]{48}|[0-9,a-f,A-F]{32})",
            0,
            &error_p,
            &offset,
            NULL);
    if( error_p != NULL ) {
        fprintf(stderr, "Error: %s\n", error_p);
        fprintf(stderr, "Offset: %d\n", offset);
    }
    int m_len = (int) strlen(line);
    char *buffer = (char*) malloc(MAX_BUFFER_LENGTH*sizeof(char));
    int rc,i;
    int ovector[30];
    rc = pcre_exec(
            pattern,
            NULL,
            line,
            m_len,
            0,
            0,
            ovector,
            30
              );
    if(rc == 4 && !isComment(line)) {
        char *substring_start = line + ovector[2];
        int substr_length = ovector[3] - ovector[2];
        sprintf( buffer, "%.*s", substr_length, substring_start );
        entry->id = atoi(buffer);
        if(entry->id >= k_len)
        	return KEY_FILE_TOO_MANY_KEYS;

        substring_start = line + ovector[4];
        substr_length = ovector[5] - ovector[4];
        entry->iv = malloc(substr_length*sizeof(char));

        sprintf( entry->iv, "%.*s", substr_length, substring_start );

        substring_start = line + ovector[6];
        substr_length = ovector[7] - ovector[6];
        entry->key = malloc(substr_length*sizeof(char));
        sprintf( entry->key, "%.*s", substr_length, substring_start );
    } else
    {
        return E_WRONG_NUMBER_OF_MATCHES;
    }
    if(entry->id == NULL || entry->iv == NULL || entry->key == NULL) {
    	return KEY_FILE_PARSE_NULL;
    }
    return KEY_FILE_PARSE_OK;
}

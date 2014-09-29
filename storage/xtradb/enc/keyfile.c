#include <keyfile.h>
#include <my_sys.h>
#include <pcre.h>
#include <string.h>

#define E_WRONG_NUMBER_OF_MATCHES 10

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

/**
 * Liest höchstens <i>k_len</i> Schüssel im Strukturarray <i>allKeys</i> ein
 */
int
parseFile(FILE * fp, struct keyentry **allKeys, const int k_len)
{
	if(NULL == fp) {
		return 100;
	}
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	struct keyentry *entry = NULL;
	int skip = FALSE;

	while( -1 != (read = getline(&line, &len, fp)) ) {
		if( FALSE == skip )
			entry = (struct keyentry*) malloc(sizeof(struct keyentry));
		if( 0 == parseLine(line, entry) && entry->id > 0 && entry->id < k_len) {
			allKeys[entry->id] = entry;
			skip = FALSE;
		}
		else
			skip = TRUE;
	}
	return 0;
}

int
parseLine(const char *line, struct keyentry *entry)
{
    const char *error_p;
    int offset;

    pcre *pattern = pcre_compile(
            "([0-9]+);([0-9,a-f,A-F]+);([0-9,a-f,A-F]+)",
            0,
            &error_p,
            &offset,
            NULL);
    if( error_p != NULL ) {
        fprintf(stderr, "Error: %s\n", error_p);
        fprintf(stderr, "Offset: %d\n", offset);
    }
    int m_len = (int) strlen(line);
    char *buffer = (char*) malloc(400*sizeof(char));
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
    if( 4 == rc && !isComment(line) ) {
        char *substring_start = line + ovector[2];
        int substr_length = ovector[3] - ovector[2];
        sprintf( buffer, "%.*s", substr_length, substring_start );
        entry->id = atoi(buffer);

        substring_start = line + ovector[4];
        substr_length = ovector[5] - ovector[4];
        entry->iv = malloc(substr_length*sizeof(char));
        sprintf( entry->iv, "%.*s", substr_length, substring_start );

        substring_start = line + ovector[6];
        substr_length = ovector[7] - ovector[6];
        entry->key = malloc(substr_length*sizeof(char));
        sprintf( entry->key, "%.*s", substr_length, substring_start );
        return 0;
    } else
    {
        return E_WRONG_NUMBER_OF_MATCHES;
    }
    return 0;
}

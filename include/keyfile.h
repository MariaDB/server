#include<stdio.h>

struct keyentry {
    int id;
    char *iv;
    char *key;
};

int
parseFile(FILE * fp, struct keyentry **allKeys, const int k_len);

int
parseLine(const char *line, struct keyentry *entry);

int
isComment(char *line);

char*
trim(char *in);

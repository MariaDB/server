#define include do not include

#define HEADER <stddef.h>
#define HELLO(str) #str

#include HEADER

static size_t something = 0;

#include HELLO(hello.c)

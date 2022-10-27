#include <mysql/service_sha2.h>
#define crypto_hash_sha512(DST,SRC,SLEN) my_sha512(DST,(char*)(SRC),SLEN)

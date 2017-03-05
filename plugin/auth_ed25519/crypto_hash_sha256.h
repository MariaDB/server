#include <mysql/service_sha2.h>
#define crypto_hash_sha256(DST,SRC,SLEN) my_sha256(DST,(char*)(SRC),SLEN)

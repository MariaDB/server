/**
  @file include/compression/lz4.h
  This service provides dynamic access to LZ4.
*/

#ifndef SERVICE_LZ4_INCLUDED
#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

extern bool COMPRESSION_LOADED_LZ4;

#define DEFINE_LZ4_compressBound(NAME) int NAME(    \
    int inputSize                                   \
)

#define DEFINE_LZ4_compress_default(NAME) int NAME( \
    const char *src,                                \
    char *dst,                                      \
    int srcSize,                                    \
    int dstCapacity                                 \
)

#define DEFINE_LZ4_decompress_safe(NAME) int NAME(  \
    const char *src,                                \
    char *dst,                                      \
    int compressedSize,                             \
    int dstCapacity                                 \
)


typedef DEFINE_LZ4_compressBound    ((*PTR_LZ4_compressBound));
typedef DEFINE_LZ4_compress_default ((*PTR_LZ4_compress_default));
typedef DEFINE_LZ4_decompress_safe  ((*PTR_LZ4_decompress_safe));

struct compression_service_lz4_st{
    PTR_LZ4_compressBound    LZ4_compressBound_ptr;
    PTR_LZ4_compress_default LZ4_compress_default_ptr;
    PTR_LZ4_decompress_safe  LZ4_decompress_safe_ptr;
};

extern struct compression_service_lz4_st *compression_service_lz4;


#define LZ4_compressBound(...)    compression_service_lz4->LZ4_compressBound_ptr    (__VA_ARGS__)
#define LZ4_compress_default(...) compression_service_lz4->LZ4_compress_default_ptr (__VA_ARGS__)
#define LZ4_decompress_safe(...)  compression_service_lz4->LZ4_decompress_safe_ptr  (__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#define SERVICE_LZ4_INCLUDED
#endif

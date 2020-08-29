/**
  @file include/compression/zstd.h
  This service provides dynamic access to ZStandard.
*/

#ifndef SERVICE_ZSTD_INCLUDED
#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdbool.h>
#include <stddef.h>
#endif

extern bool COMPRESSION_LOADED_ZSTD;

#define DEFINE_ZSTD_compress(NAME) size_t NAME( \
    void *dst,                                  \
    size_t dstCapacity,                         \
    const void *src,                            \
    size_t srcSize,                             \
    int compressionLevel                        \
)

#define DEFINE_ZSTD_compressBound(NAME) size_t NAME(    \
    size_t srcSize                                      \
)

#define DEFINE_ZSTD_decompress(NAME) size_t NAME(   \
    void *dst,                                      \
    size_t dstCapacity,                             \
    const void *src,                                \
    size_t compressedSize                           \
)

#define DEFINE_ZSTD_getErrorName(NAME) const char* NAME(    \
    size_t code                                             \
)

#define DEFINE_ZSTD_isError(NAME) unsigned NAME(    \
    size_t code                                     \
)


typedef DEFINE_ZSTD_compress      ((*PTR_ZSTD_compress));
typedef DEFINE_ZSTD_compressBound ((*PTR_ZSTD_compressBound));
typedef DEFINE_ZSTD_decompress    ((*PTR_ZSTD_decompress));
typedef DEFINE_ZSTD_getErrorName  ((*PTR_ZSTD_getErrorName));
typedef DEFINE_ZSTD_isError       ((*PTR_ZSTD_isError));


struct compression_service_zstd_st{
    PTR_ZSTD_compress      ZSTD_compress_ptr;
    PTR_ZSTD_compressBound ZSTD_compressBound_ptr;
    PTR_ZSTD_decompress    ZSTD_decompress_ptr;
    PTR_ZSTD_getErrorName  ZSTD_getErrorName_ptr;
    PTR_ZSTD_isError       ZSTD_isError_ptr;
};


extern struct compression_service_zstd_st *compression_service_zstd;


#define ZSTD_compress(...)      compression_service_zstd->ZSTD_compress_ptr      (__VA_ARGS__)
#define ZSTD_compressBound(...) compression_service_zstd->ZSTD_compressBound_ptr (__VA_ARGS__)
#define ZSTD_decompress(...)    compression_service_zstd->ZSTD_decompress_ptr    (__VA_ARGS__)
#define ZSTD_getErrorName(...)  compression_service_zstd->ZSTD_getErrorName_ptr  (__VA_ARGS__)
#define ZSTD_isError(...)       compression_service_zstd->ZSTD_isError_ptr       (__VA_ARGS__)


#ifdef __cplusplus
}
#endif

#define SERVICE_ZSTD_INCLUDED
#endif
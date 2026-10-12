/**
  @file zstd.h
  This service provides dynamic access to Zstandard (zstd).
*/

#ifndef ZSTD_INCLUDED
#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdbool.h>
#include <stddef.h>
#endif

#ifndef MYSQL_DYNAMIC_PLUGIN
#define provider_service_zstd provider_service_zstd_static
#endif

#ifndef ZSTD_VERSION_NUMBER
#define ZSTD_compressBound(...) provider_service_zstd->ZSTD_compressBound_ptr (__VA_ARGS__)
#define ZSTD_compress(...)      provider_service_zstd->ZSTD_compress_ptr      (__VA_ARGS__)
#define ZSTD_decompress(...)    provider_service_zstd->ZSTD_decompress_ptr    (__VA_ARGS__)
#define ZSTD_isError(...)       provider_service_zstd->ZSTD_isError_ptr       (__VA_ARGS__)
#endif

#define DEFINE_ZSTD_compressBound(NAME) NAME(   \
    size_t srcSize                              \
)

#define DEFINE_ZSTD_compress(NAME) NAME(        \
    void *dst,                                  \
    size_t dstCapacity,                         \
    const void *src,                            \
    size_t srcSize,                             \
    int compressionLevel                        \
)

#define DEFINE_ZSTD_decompress(NAME) NAME(      \
    void *dst,                                  \
    size_t dstCapacity,                         \
    const void *src,                            \
    size_t compressedSize                       \
)

#define DEFINE_ZSTD_isError(NAME) NAME(         \
    size_t code                                 \
)

struct provider_service_zstd_st
{
  size_t   DEFINE_ZSTD_compressBound((*ZSTD_compressBound_ptr));
  size_t   DEFINE_ZSTD_compress((*ZSTD_compress_ptr));
  size_t   DEFINE_ZSTD_decompress((*ZSTD_decompress_ptr));
  unsigned DEFINE_ZSTD_isError((*ZSTD_isError_ptr));

  bool is_loaded;
};

extern struct provider_service_zstd_st *provider_service_zstd;

#ifdef __cplusplus
}
#endif

#define ZSTD_INCLUDED
#endif

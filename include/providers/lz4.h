/**
  @file lz4.h
  This service provides dynamic access to LZ4.
*/

#ifndef LZ4_INCLUDED
#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#ifndef MYSQL_DYNAMIC_PLUGIN
#define provider_service_lz4 provider_service_lz4_static
#endif

#ifndef LZ4_VERSION_NUMBER
#define LZ4_MAX_INPUT_SIZE 0x7E000000

#define LZ4_compressBound(...)    provider_service_lz4->LZ4_compressBound_ptr       (__VA_ARGS__)
#define LZ4_compress_default(...) provider_service_lz4->LZ4_compress_default_ptr       (__VA_ARGS__)
#define LZ4_decompress_safe(...)  provider_service_lz4->LZ4_decompress_safe_ptr        (__VA_ARGS__)
#endif

#define DEFINE_LZ4_compressBound(NAME) NAME(    \
    int inputSize                               \
)

#define DEFINE_LZ4_compress_default(NAME) NAME( \
    const char *src,                            \
    char *dst,                                  \
    int srcSize,                                \
    int dstCapacity                             \
)

#define DEFINE_LZ4_decompress_safe(NAME) NAME(  \
    const char *src,                            \
    char *dst,                                  \
    int compressedSize,                         \
    int dstCapacity                             \
)

struct provider_service_lz4_st
{
  int DEFINE_LZ4_compressBound((*LZ4_compressBound_ptr));
  int DEFINE_LZ4_compress_default((*LZ4_compress_default_ptr));
  int DEFINE_LZ4_decompress_safe((*LZ4_decompress_safe_ptr));

  bool is_loaded;
};

extern struct provider_service_lz4_st *provider_service_lz4;

#ifdef __cplusplus
}
#endif

#define LZ4_INCLUDED
#endif

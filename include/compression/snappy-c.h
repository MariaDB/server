/**
  @file include/compression/snappy-c.h
  This service provides dynamic access to Snappy as a C header.
*/

#ifndef SERVICE_SNAPPY_C_INCLUDED
#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stddef.h>
#include <stdbool.h>
#endif

extern bool COMPRESSION_LOADED_SNAPPY;

typedef enum{
    SNAPPY_OK                 = 0,
    SNAPPY_INVALID_INPUT      = 1,
    SNAPPY_BUFFER_TOO_SMALL   = 2
} snappy_status;

#define DEFINE_snappy_max_compressed_length(NAME) size_t NAME(  \
    size_t source_length                                        \
)

#define DEFINE_snappy_compress(NAME) snappy_status NAME(    \
    const char *input,                                      \
    size_t input_length,                                    \
    char *compressed,                                       \
    size_t *compressed_length                               \
)

#define DEFINE_snappy_uncompress(NAME) snappy_status NAME(  \
    const char *compressed,                                 \
    size_t compressed_length,                               \
    char *uncompressed,                                     \
    size_t *uncompressed_length                             \
)

typedef DEFINE_snappy_max_compressed_length ((*PTR_snappy_max_compressed_length));
typedef DEFINE_snappy_compress              ((*PTR_snappy_compress));
typedef DEFINE_snappy_uncompress            ((*PTR_snappy_uncompress));

struct compression_service_snappy_st{
    PTR_snappy_max_compressed_length snappy_max_compressed_length_ptr;
    PTR_snappy_compress              snappy_compress_ptr;
    PTR_snappy_uncompress            snappy_uncompress_ptr;
};

extern struct compression_service_snappy_st *compression_service_snappy;

#define snappy_max_compressed_length(...) compression_service_snappy->snappy_max_compressed_length_ptr (__VA_ARGS__)
#define snappy_compress(...)              compression_service_snappy->snappy_compress_ptr              (__VA_ARGS__)
#define snappy_uncompress(...)            compression_service_snappy->snappy_uncompress_ptr            (__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#define SERVICE_SNAPPY_C_INCLUDED
#endif

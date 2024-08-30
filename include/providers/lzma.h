/**
  @file lzma.h
  This service provides dynamic access to LZMA.
*/

#ifndef LZMA_INCLUDED
#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#endif

#ifndef MYSQL_DYNAMIC_PLUGIN
#define provider_service_lzma provider_service_lzma_static
#endif

#ifndef LZMA_VERSION
typedef enum
{
    LZMA_OK                 = 0,
    LZMA_STREAM_END         = 1,
    LZMA_NO_CHECK           = 2,
    LZMA_UNSUPPORTED_CHECK  = 3,
    LZMA_GET_CHECK          = 4,
    LZMA_MEM_ERROR          = 5,
    LZMA_MEMLIMIT_ERROR     = 6,
    LZMA_FORMAT_ERROR       = 7,
    LZMA_OPTIONS_ERROR      = 8,
    LZMA_DATA_ERROR         = 9,
    LZMA_BUF_ERROR          = 10,
    LZMA_PROG_ERROR         = 11,
} lzma_ret;

typedef struct
{
    void *(*alloc)(void *opaque, size_t nmemb, size_t size);
    void (*free)(void *opaque, void *ptr);
    void *opaque;
} lzma_allocator;

typedef enum
{
    LZMA_CHECK_NONE     = 0,
    LZMA_CHECK_CRC32    = 1,
    LZMA_CHECK_CRC64    = 4,
    LZMA_CHECK_SHA256   = 10
} lzma_check;

#define lzma_stream_buffer_decode(...) provider_service_lzma->lzma_stream_buffer_decode_ptr (__VA_ARGS__)
#define lzma_easy_buffer_encode(...)   provider_service_lzma->lzma_easy_buffer_encode_ptr   (__VA_ARGS__)
#elif LZMA_VERSION < 50010030
#define lzma_maybe_const
#endif

#ifndef lzma_maybe_const
#define lzma_maybe_const const
#endif

#define DEFINE_lzma_stream_buffer_decode(NAME) NAME( \
    uint64_t *memlimit,                              \
    uint32_t flags,                                  \
    lzma_maybe_const lzma_allocator *allocator,      \
    const uint8_t *in,                               \
    size_t *in_pos,                                  \
    size_t in_size,                                  \
    uint8_t *out,                                    \
    size_t *out_pos,                                 \
    size_t out_size                                  \
)

#define DEFINE_lzma_easy_buffer_encode(NAME) NAME(   \
    uint32_t preset,                                 \
    lzma_check check,                                \
    lzma_maybe_const lzma_allocator *allocator,      \
    const uint8_t *in,                               \
    size_t in_size,                                  \
    uint8_t *out,                                    \
    size_t *out_pos,                                 \
    size_t out_size                                  \
)

struct provider_service_lzma_st
{
  lzma_ret DEFINE_lzma_stream_buffer_decode((*lzma_stream_buffer_decode_ptr));
  lzma_ret DEFINE_lzma_easy_buffer_encode((*lzma_easy_buffer_encode_ptr));

  bool is_loaded;
};

extern struct provider_service_lzma_st *provider_service_lzma;


#ifdef __cplusplus
}
#endif

#define LZMA_INCLUDED
#endif

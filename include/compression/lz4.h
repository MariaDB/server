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

#define LZ4_VERSION_NUMBER 10700
#define LZ4_MAX_INPUT_SIZE 0x7E000000

#define LZ4_MEMORY_USAGE   14

#define LZ4_HASHLOG       (LZ4_MEMORY_USAGE-2)
#define LZ4_HASHTABLESIZE (1 << LZ4_MEMORY_USAGE)
#define LZ4_HASH_SIZE_U32 (1 << LZ4_HASHLOG)

typedef struct LZ4_stream_t_internal LZ4_stream_t_internal;
struct LZ4_stream_t_internal{
    uint32_t hashTable[LZ4_HASH_SIZE_U32];
    uint32_t currentOffset;
    uint16_t dirty;
    uint16_t tableType;
    const uint8_t *dictionary;
    const LZ4_stream_t_internal *dictCtx;
    uint32_t dictSize;
};

typedef struct{
    const uint8_t *externalDict;
    size_t extDictSize;
    const uint8_t *prefixEnd;
    size_t prefixSize;
} LZ4_streamDecode_t_internal;

#define LZ4_STREAMSIZE_U64 ((1 << (LZ4_MEMORY_USAGE-3)) + 4 + ((sizeof(void*)==16) ? 4 : 0))
#define LZ4_STREAMSIZE     (LZ4_STREAMSIZE_U64  *sizeof(unsigned long long))
union LZ4_stream_u{
    unsigned long long table[LZ4_STREAMSIZE_U64];
    LZ4_stream_t_internal internal_donotuse;
};
typedef union LZ4_stream_u LZ4_stream_t;

#define LZ4_STREAMDECODESIZE_U64 (4 + ((sizeof(void*)==16) ? 2 : 0))
#define LZ4_STREAMDECODESIZE     (LZ4_STREAMDECODESIZE_U64  *sizeof(unsigned long long))
union LZ4_streamDecode_u{
    unsigned long long table[LZ4_STREAMDECODESIZE_U64];
    LZ4_streamDecode_t_internal internal_donotuse;
};
typedef union LZ4_streamDecode_u LZ4_streamDecode_t;

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

#define DEFINE_LZ4_compress_fast_continue(NAME) int NAME(   \
    LZ4_stream_t *streamPtr,                                \
    const char *src,                                        \
    char *dst,                                              \
    int srcSize,                                            \
    int dstCapacity,                                        \
    int acceleration                                        \
)

#define DEFINE_LZ4_createStream(NAME) LZ4_stream_t *NAME()

#define DEFINE_LZ4_createStreamDecode(NAME) LZ4_streamDecode_t *NAME()

#define DEFINE_LZ4_decompress_safe_continue(NAME) int NAME( \
    LZ4_streamDecode_t *LZ4_streamDecode,                   \
    const char *src,                                        \
    char *dst,                                              \
    int srcSize,                                            \
    int dstCapacity                                         \
)

#define DEFINE_LZ4_freeStream(NAME) int NAME(   \
    LZ4_stream_t *streamPtr                     \
)

#define DEFINE_LZ4_freeStreamDecode(NAME) int NAME( \
    LZ4_streamDecode_t *LZ4_stream                  \
)

#define DEFINE_LZ4_loadDict(NAME) int NAME( \
    LZ4_stream_t *streamPtr,                \
    const char *dictionary,                 \
    int dictSize                            \
)

#define DEFINE_LZ4_setStreamDecode(NAME) int NAME(  \
    LZ4_streamDecode_t *LZ4_streamDecode,           \
    const char *dictionary,                         \
    int dictSize                                    \
)

#define LZ4HC_DICTIONARY_LOGSIZE 16
#define LZ4HC_MAXD (1<<LZ4HC_DICTIONARY_LOGSIZE)
#define LZ4HC_MAXD_MASK (LZ4HC_MAXD - 1)

#define LZ4HC_HASH_LOG 15
#define LZ4HC_HASHTABLESIZE (1 << LZ4HC_HASH_LOG)
#define LZ4HC_HASH_MASK (LZ4HC_HASHTABLESIZE - 1)

typedef struct LZ4HC_CCtx_internal LZ4HC_CCtx_internal;
struct LZ4HC_CCtx_internal{
    uint32_t   hashTable[LZ4HC_HASHTABLESIZE];
    uint16_t   chainTable[LZ4HC_MAXD];
    const uint8_t *end;
    const uint8_t *base;
    const uint8_t *dictBase;
    uint32_t   dictLimit;
    uint32_t   lowLimit;
    uint32_t   nextToUpdate;
    short      compressionLevel;
    int8_t     favorDecSpeed;
    int8_t     dirty;
    const LZ4HC_CCtx_internal *dictCtx;
};

#define LZ4_STREAMHCSIZE       (4*LZ4HC_HASHTABLESIZE + 2*LZ4HC_MAXD + 56 + ((sizeof(void*)==16) ? 56 : 0))
#define LZ4_STREAMHCSIZE_SIZET (LZ4_STREAMHCSIZE / sizeof(size_t))
union LZ4_streamHC_u{
    size_t table[LZ4_STREAMHCSIZE_SIZET];
    LZ4HC_CCtx_internal internal_donotuse;
};
typedef union LZ4_streamHC_u LZ4_streamHC_t;

#define DEFINE_LZ4_compress_HC_continue(NAME) int NAME( \
    LZ4_streamHC_t *streamHCPtr,                        \
    const char *src,                                    \
    char *dst,                                          \
    int srcSize,                                        \
    int maxDstSize                                      \
)

#define DEFINE_LZ4_createStreamHC(NAME) LZ4_streamHC_t *NAME()

#define DEFINE_LZ4_freeStreamHC(NAME) int NAME( \
    LZ4_streamHC_t *streamHCPtr                 \
)

#define DEFINE_LZ4_loadDictHC(NAME) int NAME(   \
    LZ4_streamHC_t *streamHCPtr,                \
    const char *dictionary,                     \
    int dictSize                                \
)

#define DEFINE_LZ4_resetStreamHC(NAME) void NAME(   \
    LZ4_streamHC_t *streamHCPtr,                    \
    int compressionLevel                            \
)


typedef DEFINE_LZ4_compressBound            ((*PTR_LZ4_compressBound));
typedef DEFINE_LZ4_compress_default         ((*PTR_LZ4_compress_default));
typedef DEFINE_LZ4_decompress_safe          ((*PTR_LZ4_decompress_safe));
typedef DEFINE_LZ4_compress_fast_continue   ((*PTR_LZ4_compress_fast_continue));
typedef DEFINE_LZ4_createStream             ((*PTR_LZ4_createStream));
typedef DEFINE_LZ4_createStreamDecode       ((*PTR_LZ4_createStreamDecode));
typedef DEFINE_LZ4_decompress_safe_continue ((*PTR_LZ4_decompress_safe_continue));
typedef DEFINE_LZ4_freeStream               ((*PTR_LZ4_freeStream));
typedef DEFINE_LZ4_freeStreamDecode         ((*PTR_LZ4_freeStreamDecode));
typedef DEFINE_LZ4_loadDict                 ((*PTR_LZ4_loadDict));
typedef DEFINE_LZ4_setStreamDecode          ((*PTR_LZ4_setStreamDecode));

typedef DEFINE_LZ4_compress_HC_continue     ((*PTR_LZ4_compress_HC_continue));
typedef DEFINE_LZ4_createStreamHC           ((*PTR_LZ4_createStreamHC));
typedef DEFINE_LZ4_freeStreamHC             ((*PTR_LZ4_freeStreamHC));
typedef DEFINE_LZ4_loadDictHC               ((*PTR_LZ4_loadDictHC));
typedef DEFINE_LZ4_resetStreamHC            ((*PTR_LZ4_resetStreamHC));


struct compression_service_lz4_st{
    PTR_LZ4_compressBound            LZ4_compressBound_ptr;
    PTR_LZ4_compress_default         LZ4_compress_default_ptr;
    PTR_LZ4_compress_fast_continue   LZ4_compress_fast_continue_ptr;
    PTR_LZ4_createStream             LZ4_createStream_ptr;
    PTR_LZ4_createStreamDecode       LZ4_createStreamDecode_ptr;
    PTR_LZ4_decompress_safe          LZ4_decompress_safe_ptr;
    PTR_LZ4_decompress_safe_continue LZ4_decompress_safe_continue_ptr;
    PTR_LZ4_freeStream               LZ4_freeStream_ptr;
    PTR_LZ4_freeStreamDecode         LZ4_freeStreamDecode_ptr;
    PTR_LZ4_loadDict                 LZ4_loadDict_ptr;
    PTR_LZ4_setStreamDecode          LZ4_setStreamDecode_ptr;

    PTR_LZ4_compress_HC_continue     LZ4_compress_HC_continue_ptr;
    PTR_LZ4_createStreamHC           LZ4_createStreamHC_ptr;
    PTR_LZ4_freeStreamHC             LZ4_freeStreamHC_ptr;
    PTR_LZ4_loadDictHC               LZ4_loadDictHC_ptr;
    PTR_LZ4_resetStreamHC            LZ4_resetStreamHC_ptr;
};

extern struct compression_service_lz4_st *compression_service_lz4;


#define LZ4_compressBound(...)            compression_service_lz4->LZ4_compressBound_ptr            (__VA_ARGS__)
#define LZ4_compress_default(...)         compression_service_lz4->LZ4_compress_default_ptr         (__VA_ARGS__)
#define LZ4_compress_fast_continue(...)   compression_service_lz4->LZ4_compress_fast_continue_ptr   (__VA_ARGS__)
#define LZ4_createStream(...)             compression_service_lz4->LZ4_createStream_ptr             (__VA_ARGS__)
#define LZ4_createStreamDecode(...)       compression_service_lz4->LZ4_createStreamDecode_ptr       (__VA_ARGS__)
#define LZ4_decompress_safe(...)          compression_service_lz4->LZ4_decompress_safe_ptr          (__VA_ARGS__)
#define LZ4_decompress_safe_continue(...) compression_service_lz4->LZ4_decompress_safe_continue_ptr (__VA_ARGS__)
#define LZ4_freeStream(...)               compression_service_lz4->LZ4_freeStream_ptr               (__VA_ARGS__)
#define LZ4_freeStreamDecode(...)         compression_service_lz4->LZ4_freeStreamDecode_ptr         (__VA_ARGS__)
#define LZ4_loadDict(...)                 compression_service_lz4->LZ4_loadDict_ptr                 (__VA_ARGS__)
#define LZ4_setStreamDecode(...)          compression_service_lz4->LZ4_setStreamDecode_ptr          (__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#define SERVICE_LZ4_INCLUDED
#endif

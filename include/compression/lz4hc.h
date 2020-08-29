/**
  @file include/compression/lz4hc.h
  This service provides dynamic access to LZ4HC.
*/

#ifndef SERVICE_LZ4HC_INCLUDED
#ifdef __cplusplus
extern "C" {
#endif

#include "lz4.h"

#define LZ4_compress_HC_continue(...) compression_service_lz4->LZ4_compress_HC_continue_ptr (__VA_ARGS__)
#define LZ4_createStreamHC(...)       compression_service_lz4->LZ4_createStreamHC_ptr       (__VA_ARGS__)
#define LZ4_freeStreamHC(...)         compression_service_lz4->LZ4_freeStreamHC_ptr         (__VA_ARGS__)
#define LZ4_loadDictHC(...)           compression_service_lz4->LZ4_loadDictHC_ptr           (__VA_ARGS__)
#define LZ4_resetStreamHC(...)        compression_service_lz4->LZ4_resetStreamHC_ptr        (__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#define SERVICE_LZ4HC_INCLUDED
#endif

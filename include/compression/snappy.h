/**
  @file include/compression/snappy.h
  This service provides dynamic access to Snappy as a C++ header.
*/

#ifndef SERVICE_SNAPPY_INCLUDED

#ifndef MYSQL_ABI_CHECK
#include <stddef.h>
#include <stdint.h>
#endif

namespace snappy{
    size_t MaxCompressedLength(size_t source_bytes);
    void RawCompress(const char *input, size_t input_length, char *compressed, size_t *compressed_length);
    bool GetUncompressedLength(const char *compressed, size_t compressed_length, size_t *result);
    bool RawUncompress(const char *compressed, size_t compressed_length, char *uncompressed);
}

#define SERVICE_SNAPPY_INCLUDED
#endif

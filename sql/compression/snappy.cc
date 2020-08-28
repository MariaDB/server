#include "compression_libs.h"
#include <compression/snappy.h>
#include <dlfcn.h>

bool COMPRESSION_LOADED_SNAPPY = false;


DEFINE_snappy_max_compressed_length(DUMMY_snappy_max_compressed_length){
    return 0;
}

DEFINE_snappy_compress(DUMMY_snappy_compress){
    *compressed_length = 0;
    return SNAPPY_INVALID_INPUT;
}

DEFINE_snappy_uncompressed_length(DUMMY_snappy_uncompressed_length){
    return SNAPPY_INVALID_INPUT;
}

DEFINE_snappy_uncompress(DUMMY_snappy_uncompress){
    return SNAPPY_INVALID_INPUT;
}


namespace snappy{
    size_t MaxCompressedLength(size_t source_bytes){
        return snappy_max_compressed_length(source_bytes);
    }

    void RawCompress(const char *input, size_t input_length, char *compressed, size_t *compressed_length){
        snappy_compress(input, input_length, compressed, compressed_length);
    }

    bool GetUncompressedLength(const char *compressed, size_t compressed_length, size_t *result){
        return snappy_uncompressed_length(compressed, compressed_length, result) == SNAPPY_OK;
    }

    bool RawUncompress(const char *compressed, size_t compressed_length, char *uncompressed){
        size_t uncompressed_length = 0;
        if(GetUncompressedLength(compressed, compressed_length, &uncompressed_length))
            return snappy_uncompress(compressed, compressed_length, uncompressed, &uncompressed_length) == SNAPPY_OK;
        else
            return false;
    }
}


void init_snappy(struct compression_service_snappy_st *handler, bool load_library){
    //point struct to right place for static plugins
    compression_service_snappy = handler;

    compression_service_snappy->snappy_max_compressed_length_ptr = DUMMY_snappy_max_compressed_length;
    compression_service_snappy->snappy_compress_ptr              = DUMMY_snappy_compress;
    compression_service_snappy->snappy_uncompressed_length_ptr   = DUMMY_snappy_uncompressed_length;
    compression_service_snappy->snappy_uncompress_ptr            = DUMMY_snappy_uncompress;

    if(!load_library)
        return;

    //Load Snappy library dynamically
    void *library_handle = dlopen("libsnappy.so", RTLD_LAZY | RTLD_GLOBAL);
    if(!library_handle || dlerror())
        return;

    void *snappy_max_compressed_length_ptr = dlsym(library_handle, "snappy_max_compressed_length");
    void *snappy_compress_ptr              = dlsym(library_handle, "snappy_compress");
    void *snappy_uncompressed_length_ptr   = dlsym(library_handle, "snappy_uncompressed_length");
    void *snappy_uncompress_ptr            = dlsym(library_handle, "snappy_uncompress");

    if(
        !snappy_max_compressed_length_ptr ||
        !snappy_compress_ptr              ||
        !snappy_uncompressed_length_ptr   ||
        !snappy_uncompress_ptr
    )
        return;
    
    compression_service_snappy->snappy_max_compressed_length_ptr = (PTR_snappy_max_compressed_length) snappy_max_compressed_length_ptr;
    compression_service_snappy->snappy_compress_ptr              = (PTR_snappy_compress)              snappy_compress_ptr;
    compression_service_snappy->snappy_uncompressed_length_ptr   = (PTR_snappy_uncompressed_length)   snappy_uncompressed_length_ptr;
    compression_service_snappy->snappy_uncompress_ptr            = (PTR_snappy_uncompress)            snappy_uncompress_ptr;

    COMPRESSION_LOADED_SNAPPY = true;
}

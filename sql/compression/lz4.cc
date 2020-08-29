#include "compression_libs.h"
#include <dlfcn.h>

bool COMPRESSION_LOADED_LZ4 = false;


DEFINE_LZ4_compressBound(DUMMY_LZ4_compressBound){
    return 0; //returns maximum output size (0 is error)
}

DEFINE_LZ4_compress_default(DUMMY_LZ4_compress_default){
    return 0; //returns number of bytes written (0 is error)
}

DEFINE_LZ4_decompress_safe(DUMMY_LZ4_decompress_safe){
    return -1; //returns number of bytes decompressed (< 0 is error)
}


void init_lz4(struct compression_service_lz4_st *handler, bool load_library){
    //point struct to right place for static plugins
    compression_service_lz4 = handler;

    compression_service_lz4->LZ4_compressBound_ptr    = DUMMY_LZ4_compressBound;
    compression_service_lz4->LZ4_compress_default_ptr = DUMMY_LZ4_compress_default;
    compression_service_lz4->LZ4_decompress_safe_ptr  = DUMMY_LZ4_decompress_safe;

    if(!load_library)
        return;

    //Load LZ4 library dynamically
    void *library_handle = dlopen("liblz4.so", RTLD_LAZY | RTLD_GLOBAL);
    if(!library_handle || dlerror())
        return;

    void *LZ4_compressBound_ptr    = dlsym(library_handle, "LZ4_compressBound");
    void *LZ4_compress_default_ptr = dlsym(library_handle, "LZ4_compress_default");
    void *LZ4_decompress_safe_ptr  = dlsym(library_handle, "LZ4_decompress_safe");

    if(
        !LZ4_compressBound_ptr    ||
        !LZ4_compress_default_ptr ||
        !LZ4_decompress_safe_ptr  ||
    )
        return;

    compression_service_lz4->LZ4_compressBound_ptr    = (PTR_LZ4_compressBound)    LZ4_compressBound_ptr;
    compression_service_lz4->LZ4_compress_default_ptr = (PTR_LZ4_compress_default) LZ4_compress_default_ptr;
    compression_service_lz4->LZ4_decompress_safe_ptr  = (PTR_LZ4_decompress_safe)  LZ4_decompress_safe_ptr;

    COMPRESSION_LOADED_LZ4 = true;
}

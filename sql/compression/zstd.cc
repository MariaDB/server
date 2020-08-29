#include "compression_libs.h"
#include <dlfcn.h>

bool COMPRESSION_LOADED_ZSTD = false;

// Most functions return the (un)compressed size, not an error code

DEFINE_ZSTD_compress(DUMMY_ZSTD_compress){
    return 0;
}

DEFINE_ZSTD_compressBound(DUMMY_ZSTD_compressBound){
    return 0;
}

DEFINE_ZSTD_decompress(DUMMY_ZSTD_decompress){
    return 0;
}

DEFINE_ZSTD_getErrorName(DUMMY_ZSTD_getErrorName){
    return "ZStd is not loaded.";
}

DEFINE_ZSTD_isError(DUMMY_ZSTD_isError){
    return 1;
}


void init_zstd(struct compression_service_zstd_st *handler, bool link_library){
    //point struct to right place for static plugins
    compression_service_zstd = handler;

    compression_service_zstd->ZSTD_compress_ptr      = DUMMY_ZSTD_compress;
    compression_service_zstd->ZSTD_compressBound_ptr = DUMMY_ZSTD_compressBound;
    compression_service_zstd->ZSTD_decompress_ptr    = DUMMY_ZSTD_decompress;
    compression_service_zstd->ZSTD_getErrorName_ptr  = DUMMY_ZSTD_getErrorName;
    compression_service_zstd->ZSTD_isError_ptr       = DUMMY_ZSTD_isError;

    if(!link_library)
        return;

    //Load ZStd library dynamically
    void *library_handle = dlopen("libzstd.so", RTLD_LAZY | RTLD_GLOBAL);
    if(!library_handle || dlerror()){
        //sql_print_warning("Could not open libzstd.so\n");
        return;
    }

    void *ZSTD_compress_ptr      = dlsym(library_handle, "ZSTD_compress");
    void *ZSTD_compressBound_ptr = dlsym(library_handle, "ZSTD_compressBound");
    void *ZSTD_decompress_ptr    = dlsym(library_handle, "ZSTD_decompress");
    void *ZSTD_getErrorName_ptr  = dlsym(library_handle, "ZSTD_getErrorName");
    void *ZSTD_isError_ptr       = dlsym(library_handle, "ZSTD_isError");
    
    if(
        !ZSTD_compress_ptr      ||
        !ZSTD_compressBound_ptr ||
        !ZSTD_decompress_ptr    ||
        !ZSTD_getErrorName_ptr  ||
        !ZSTD_isError_ptr
    )
        return;
    
    compression_service_zstd->ZSTD_compress_ptr      = (PTR_ZSTD_compress)      ZSTD_compress_ptr;
    compression_service_zstd->ZSTD_compressBound_ptr = (PTR_ZSTD_compressBound) ZSTD_compressBound_ptr;
    compression_service_zstd->ZSTD_decompress_ptr    = (PTR_ZSTD_decompress)    ZSTD_decompress_ptr;
    compression_service_zstd->ZSTD_getErrorName_ptr  = (PTR_ZSTD_getErrorName)  ZSTD_getErrorName_ptr;
    compression_service_zstd->ZSTD_isError_ptr       = (PTR_ZSTD_isError)       ZSTD_isError_ptr;
    
    COMPRESSION_LOADED_ZSTD = true;
} 
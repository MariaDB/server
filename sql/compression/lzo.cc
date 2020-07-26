#include "compression_libs.h"
#include <dlfcn.h>

bool COMPRESSION_LOADED_LZO = false;


DEFINE_lzo1x_1_15_compress(DUMMY_lzo1x_1_15_compress){
    return LZO_E_INTERNAL_ERROR;
}

DEFINE_lzo1x_decompress_safe(DUMMY_lzo1x_decompress_safe){
    return LZO_E_INTERNAL_ERROR;
}


void init_lzo(struct compression_service_lzo_st *handler, bool load_library){
    //point struct to right place for static plugins
    compression_service_lzo = handler;

    compression_service_lzo->lzo1x_1_15_compress_ptr   = DUMMY_lzo1x_1_15_compress;
    compression_service_lzo->lzo1x_decompress_safe_ptr = DUMMY_lzo1x_decompress_safe;

    if(!load_library)
        return;

    //Load LZO library dynamically
    void *library_handle = dlopen("liblzo2.so", RTLD_LAZY | RTLD_GLOBAL);
    if(!library_handle || dlerror())
        return;

    void *lzo1x_1_15_compress_ptr   = dlsym(library_handle, "lzo1x_1_15_compress");
    void *lzo1x_decompress_safe_ptr = dlsym(library_handle, "lzo1x_decompress_safe");
    if(
        !lzo1x_1_15_compress_ptr ||
        !lzo1x_decompress_safe_ptr
    )
        return;

    compression_service_lzo->lzo1x_1_15_compress_ptr   = (PTR_lzo1x_1_15_compress)   lzo1x_1_15_compress_ptr;
    compression_service_lzo->lzo1x_decompress_safe_ptr = (PTR_lzo1x_decompress_safe) lzo1x_decompress_safe_ptr;

    COMPRESSION_LOADED_LZO = true;
}

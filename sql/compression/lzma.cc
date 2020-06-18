#include "compression_libs.h"
#include <dlfcn.h>

bool COMPRESSION_LOADED_LZMA = false;


DEFINE_lzma_stream_buffer_decode(DUMMY_lzma_stream_buffer_decode){
    return LZMA_PROG_ERROR;
}

DEFINE_lzma_easy_buffer_encode(DUMMY_lzma_easy_buffer_encode){
    return LZMA_PROG_ERROR;
}


void init_lzma(struct compression_service_lzma_st *handler, bool link_library){
    //point struct to right place for static plugins
    compression_service_lzma = handler;

    compression_service_lzma->lzma_stream_buffer_decode_ptr = DUMMY_lzma_stream_buffer_decode;
    compression_service_lzma->lzma_easy_buffer_encode_ptr   = DUMMY_lzma_easy_buffer_encode;

    if(!link_library)
        return;

    //Load LZMA library dynamically
    void *lzma_library_handle = dlopen("liblzma.so", RTLD_LAZY | RTLD_GLOBAL);
    if(!lzma_library_handle || dlerror())
        return;

    void *lzma_stream_buffer_decode_ptr = dlsym(lzma_library_handle, "lzma_stream_buffer_decode");
    void *lzma_easy_buffer_encode_ptr   = dlsym(lzma_library_handle, "lzma_easy_buffer_encode");
    if(
        !lzma_stream_buffer_decode_ptr ||
        !lzma_easy_buffer_encode_ptr
    )
        return;

    compression_service_lzma->lzma_stream_buffer_decode_ptr = (PTR_lzma_stream_buffer_decode) lzma_stream_buffer_decode_ptr;
    compression_service_lzma->lzma_easy_buffer_encode_ptr   = (PTR_lzma_easy_buffer_encode)   lzma_easy_buffer_encode_ptr;
    
    COMPRESSION_LOADED_LZMA = true;
}

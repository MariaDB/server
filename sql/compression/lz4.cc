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

DEFINE_LZ4_compress_fast_continue(DUMMY_LZ4_compress_fast_continue){
    return 0; //returns size of compressed block (0 is error)
}

DEFINE_LZ4_createStream(DUMMY_LZ4_createStream){
    return nullptr;
}

DEFINE_LZ4_createStreamDecode(DUMMY_LZ4_createStreamDecode){
    return nullptr;
}

DEFINE_LZ4_decompress_safe_continue(DUMMY_LZ4_decompress_safe_continue){
    return -1; //returns decompressed size (< 0 is error)
}

DEFINE_LZ4_freeStream(DUMMY_LZ4_freeStream){
    return -1; //return value not checked
}

DEFINE_LZ4_freeStreamDecode(DUMMY_LZ4_freeStreamDecode){
    return -1; //return value not checked
}

DEFINE_LZ4_loadDict(DUMMY_LZ4_loadDict){
    return -1; //return value not checked
}

DEFINE_LZ4_setStreamDecode(DUMMY_LZ4_setStreamDecode){
    return 0; //return value not checked (0 is error)
}

DEFINE_LZ4_compress_HC_continue(DUMMY_LZ4_compress_HC_continue){
    return 0; //returns number of bytes compressed (0 is error)
}

DEFINE_LZ4_createStreamHC(DUMMY_LZ4_createStreamHC){
    return nullptr;
}

DEFINE_LZ4_freeStreamHC(DUMMY_LZ4_freeStreamHC){
    return -1; //return value not checked
}

DEFINE_LZ4_loadDictHC(DUMMY_LZ4_loadDictHC){
    return -1; //return value not checked
}

DEFINE_LZ4_resetStreamHC(DUMMY_LZ4_resetStreamHC){
    //void function
}


void init_lz4(struct compression_service_lz4_st *handler, bool load_library){
    //point struct to right place for static plugins
    compression_service_lz4 = handler;

    compression_service_lz4->LZ4_compressBound_ptr            = DUMMY_LZ4_compressBound;
    compression_service_lz4->LZ4_compress_default_ptr         = DUMMY_LZ4_compress_default;
    compression_service_lz4->LZ4_decompress_safe_ptr          = DUMMY_LZ4_decompress_safe;
    compression_service_lz4->LZ4_compress_fast_continue_ptr   = DUMMY_LZ4_compress_fast_continue;
    compression_service_lz4->LZ4_createStream_ptr             = DUMMY_LZ4_createStream;
    compression_service_lz4->LZ4_createStreamDecode_ptr       = DUMMY_LZ4_createStreamDecode;
    compression_service_lz4->LZ4_decompress_safe_continue_ptr = DUMMY_LZ4_decompress_safe_continue;
    compression_service_lz4->LZ4_freeStream_ptr               = DUMMY_LZ4_freeStream;
    compression_service_lz4->LZ4_freeStreamDecode_ptr         = DUMMY_LZ4_freeStreamDecode;
    compression_service_lz4->LZ4_loadDict_ptr                 = DUMMY_LZ4_loadDict;
    compression_service_lz4->LZ4_setStreamDecode_ptr          = DUMMY_LZ4_setStreamDecode;

    compression_service_lz4->LZ4_compress_HC_continue_ptr     = DUMMY_LZ4_compress_HC_continue;
    compression_service_lz4->LZ4_createStreamHC_ptr           = DUMMY_LZ4_createStreamHC;
    compression_service_lz4->LZ4_freeStreamHC_ptr             = DUMMY_LZ4_freeStreamHC;
    compression_service_lz4->LZ4_loadDictHC_ptr               = DUMMY_LZ4_loadDictHC;
    compression_service_lz4->LZ4_resetStreamHC_ptr            = DUMMY_LZ4_resetStreamHC;

    if(!load_library)
        return;

    //Load LZ4 library dynamically
    void *library_handle = dlopen("liblz4.so", RTLD_LAZY | RTLD_GLOBAL);
    if(!library_handle || dlerror())
        return;

    void *LZ4_compressBound_ptr            = dlsym(library_handle, "LZ4_compressBound");
    void *LZ4_compress_default_ptr         = dlsym(library_handle, "LZ4_compress_default");
    void *LZ4_decompress_safe_ptr          = dlsym(library_handle, "LZ4_decompress_safe");
    void *LZ4_compress_fast_continue_ptr   = dlsym(library_handle, "LZ4_compress_fast_continue");
    void *LZ4_createStream_ptr             = dlsym(library_handle, "LZ4_createStream");
    void *LZ4_createStreamDecode_ptr       = dlsym(library_handle, "LZ4_createStreamDecode");
    void *LZ4_decompress_safe_continue_ptr = dlsym(library_handle, "LZ4_decompress_safe_continue");
    void *LZ4_freeStream_ptr               = dlsym(library_handle, "LZ4_freeStream");
    void *LZ4_freeStreamDecode_ptr         = dlsym(library_handle, "LZ4_freeStreamDecode");
    void *LZ4_loadDict_ptr                 = dlsym(library_handle, "LZ4_loadDict");
    void *LZ4_setStreamDecode_ptr          = dlsym(library_handle, "LZ4_setStreamDecode");

    void *LZ4_compress_HC_continue_ptr     = dlsym(library_handle, "LZ4_compress_HC_continue");
    void *LZ4_createStreamHC_ptr           = dlsym(library_handle, "LZ4_createStreamHC");
    void *LZ4_freeStreamHC_ptr             = dlsym(library_handle, "LZ4_freeStreamHC");
    void *LZ4_loadDictHC_ptr               = dlsym(library_handle, "LZ4_loadDictHC");
    void *LZ4_resetStreamHC_ptr            = dlsym(library_handle, "LZ4_resetStreamHC");
    if(
        !LZ4_compressBound_ptr            ||
        !LZ4_compress_default_ptr         ||
        !LZ4_decompress_safe_ptr          ||
        !LZ4_compress_fast_continue_ptr   ||
        !LZ4_createStream_ptr             ||
        !LZ4_createStreamDecode_ptr       ||
        !LZ4_decompress_safe_continue_ptr ||
        !LZ4_freeStream_ptr               ||
        !LZ4_freeStreamDecode_ptr         ||
        !LZ4_loadDict_ptr                 ||
        !LZ4_setStreamDecode_ptr          ||

        !LZ4_compress_HC_continue_ptr     ||
        !LZ4_createStreamHC_ptr           ||
        !LZ4_freeStreamHC_ptr             ||
        !LZ4_loadDictHC_ptr               ||
        !LZ4_resetStreamHC_ptr
    )
        return;

    compression_service_lz4->LZ4_compressBound_ptr            = (PTR_LZ4_compressBound)            LZ4_compressBound_ptr;
    compression_service_lz4->LZ4_compress_default_ptr         = (PTR_LZ4_compress_default)         LZ4_compress_default_ptr;
    compression_service_lz4->LZ4_decompress_safe_ptr          = (PTR_LZ4_decompress_safe)          LZ4_decompress_safe_ptr;
    compression_service_lz4->LZ4_compress_fast_continue_ptr   = (PTR_LZ4_compress_fast_continue)   LZ4_compress_fast_continue_ptr;
    compression_service_lz4->LZ4_createStream_ptr             = (PTR_LZ4_createStream)             LZ4_createStream_ptr;
    compression_service_lz4->LZ4_createStreamDecode_ptr       = (PTR_LZ4_createStreamDecode)       LZ4_createStreamDecode_ptr;
    compression_service_lz4->LZ4_decompress_safe_continue_ptr = (PTR_LZ4_decompress_safe_continue) LZ4_decompress_safe_continue_ptr;
    compression_service_lz4->LZ4_freeStream_ptr               = (PTR_LZ4_freeStream)               LZ4_freeStream_ptr;
    compression_service_lz4->LZ4_freeStreamDecode_ptr         = (PTR_LZ4_freeStreamDecode)         LZ4_freeStreamDecode_ptr;
    compression_service_lz4->LZ4_loadDict_ptr                 = (PTR_LZ4_loadDict)                 LZ4_loadDict_ptr;
    compression_service_lz4->LZ4_setStreamDecode_ptr          = (PTR_LZ4_setStreamDecode)          LZ4_setStreamDecode_ptr;

    compression_service_lz4->LZ4_compress_HC_continue_ptr     = (PTR_LZ4_compress_HC_continue)     LZ4_compress_HC_continue_ptr;
    compression_service_lz4->LZ4_createStreamHC_ptr           = (PTR_LZ4_createStreamHC)           LZ4_createStreamHC_ptr;
    compression_service_lz4->LZ4_freeStreamHC_ptr             = (PTR_LZ4_freeStreamHC)             LZ4_freeStreamHC_ptr;
    compression_service_lz4->LZ4_loadDictHC_ptr               = (PTR_LZ4_loadDictHC)               LZ4_loadDictHC_ptr;
    compression_service_lz4->LZ4_resetStreamHC_ptr            = (PTR_LZ4_resetStreamHC)            LZ4_resetStreamHC_ptr;

    COMPRESSION_LOADED_LZ4 = true;
}

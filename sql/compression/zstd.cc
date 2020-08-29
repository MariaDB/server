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

DEFINE_ZSTD_compress_usingCDict(DUMMY_ZSTD_compress_usingCDict){
    return 0;
}

DEFINE_ZSTD_compress_usingDict(DUMMY_ZSTD_compress_usingDict){
    return 0;
}

DEFINE_ZSTD_createCCtx(DUMMY_ZSTD_createCCtx){
    return nullptr;
}

DEFINE_ZSTD_createCCtx_advanced(DUMMY_ZSTD_createCCtx_advanced){
    return nullptr;
}

DEFINE_ZSTD_createCDict(DUMMY_ZSTD_createCDict){
    return nullptr;
}

DEFINE_ZSTD_createDCtx(DUMMY_ZSTD_createDCtx){
    return nullptr;
}

DEFINE_ZSTD_createDCtx_advanced(DUMMY_ZSTD_createDCtx_advanced){
    return nullptr;
}

DEFINE_ZSTD_createDDict_byReference(DUMMY_ZSTD_createDDict_byReference){
    return nullptr;
}

DEFINE_ZSTD_decompress(DUMMY_ZSTD_decompress){
    return 0;
}

DEFINE_ZSTD_decompress_usingDDict(DUMMY_ZSTD_decompress_usingDDict){
    return 0;
}

DEFINE_ZSTD_decompress_usingDict(DUMMY_ZSTD_decompress_usingDict){
    return 0;
}

DEFINE_ZSTD_freeCCtx(DUMMY_ZSTD_freeCCtx){
    return 0;
}

DEFINE_ZSTD_freeCDict(DUMMY_ZSTD_freeCDict){
    return 0;
}

DEFINE_ZSTD_freeDCtx(DUMMY_ZSTD_freeDCtx){
    return 0;
}

DEFINE_ZSTD_freeDDict(DUMMY_ZSTD_freeDDict){
    return 0;
}

DEFINE_ZSTD_getErrorName(DUMMY_ZSTD_getErrorName){
    return "ZStd is not loaded.";
}

DEFINE_ZSTD_isError(DUMMY_ZSTD_isError){
    return 1;
}

DEFINE_ZSTD_sizeof_DDict(DUMMY_ZSTD_sizeof_DDict){
    return 0;
}

DEFINE_ZSTD_versionNumber(DUMMY_ZSTD_versionNumber){
    return 0;
}


void init_zstd(struct compression_service_zstd_st *handler, bool link_library){
    //point struct to right place for static plugins
    compression_service_zstd = handler;

    compression_service_zstd->ZSTD_compress_ptr                = DUMMY_ZSTD_compress;
    compression_service_zstd->ZSTD_compressBound_ptr           = DUMMY_ZSTD_compressBound;
    compression_service_zstd->ZSTD_compress_usingCDict_ptr     = DUMMY_ZSTD_compress_usingCDict;
    compression_service_zstd->ZSTD_compress_usingDict_ptr      = DUMMY_ZSTD_compress_usingDict;
    compression_service_zstd->ZSTD_createCCtx_ptr              = DUMMY_ZSTD_createCCtx;
    compression_service_zstd->ZSTD_createCCtx_advanced_ptr     = DUMMY_ZSTD_createCCtx_advanced;
    compression_service_zstd->ZSTD_createCDict_ptr             = DUMMY_ZSTD_createCDict;
    compression_service_zstd->ZSTD_createDCtx_ptr              = DUMMY_ZSTD_createDCtx;
    compression_service_zstd->ZSTD_createDCtx_advanced_ptr     = DUMMY_ZSTD_createDCtx_advanced;
    compression_service_zstd->ZSTD_createDDict_byReference_ptr = DUMMY_ZSTD_createDDict_byReference;
    compression_service_zstd->ZSTD_decompress_ptr              = DUMMY_ZSTD_decompress;
    compression_service_zstd->ZSTD_decompress_usingDDict_ptr   = DUMMY_ZSTD_decompress_usingDDict;
    compression_service_zstd->ZSTD_decompress_usingDict_ptr    = DUMMY_ZSTD_decompress_usingDict;
    compression_service_zstd->ZSTD_freeCCtx_ptr                = DUMMY_ZSTD_freeCCtx;
    compression_service_zstd->ZSTD_freeCDict_ptr               = DUMMY_ZSTD_freeCDict;
    compression_service_zstd->ZSTD_freeDCtx_ptr                = DUMMY_ZSTD_freeDCtx;
    compression_service_zstd->ZSTD_freeDDict_ptr               = DUMMY_ZSTD_freeDDict;
    compression_service_zstd->ZSTD_getErrorName_ptr            = DUMMY_ZSTD_getErrorName;
    compression_service_zstd->ZSTD_isError_ptr                 = DUMMY_ZSTD_isError;
    compression_service_zstd->ZSTD_sizeof_DDict_ptr            = DUMMY_ZSTD_sizeof_DDict;
    compression_service_zstd->ZSTD_versionNumber_ptr           = DUMMY_ZSTD_versionNumber;

    if(!link_library)
        return;

    //Load ZStd library dynamically
    
    //TODO: enforce library version on load.
    void *library_handle = dlopen("libzstd.so", RTLD_LAZY | RTLD_GLOBAL);
    if(!library_handle || dlerror()){
        //sql_print_warning("Could not open libzstd.so\n");
        return;
    }

    void *ZSTD_compress_ptr                = dlsym(library_handle, "ZSTD_compress");
    void *ZSTD_compressBound_ptr           = dlsym(library_handle, "ZSTD_compressBound");
    void *ZSTD_compress_usingCDict_ptr     = dlsym(library_handle, "ZSTD_compress_usingCDict");
    void *ZSTD_compress_usingDict_ptr      = dlsym(library_handle, "ZSTD_compress_usingDict");
    void *ZSTD_createCCtx_ptr              = dlsym(library_handle, "ZSTD_createCCtx");
    void *ZSTD_createCCtx_advanced_ptr     = dlsym(library_handle, "ZSTD_createCCtx_advanced");
    void *ZSTD_createCDict_ptr             = dlsym(library_handle, "ZSTD_createCDict");
    void *ZSTD_createDCtx_ptr              = dlsym(library_handle, "ZSTD_createDCtx");
    void *ZSTD_createDCtx_advanced_ptr     = dlsym(library_handle, "ZSTD_createDCtx_advanced");
    void *ZSTD_createDDict_byReference_ptr = dlsym(library_handle, "ZSTD_createDDict_byReference");
    void *ZSTD_decompress_ptr              = dlsym(library_handle, "ZSTD_decompress");
    void *ZSTD_decompress_usingDDict_ptr   = dlsym(library_handle, "ZSTD_decompress_usingDDict");
    void *ZSTD_decompress_usingDict_ptr    = dlsym(library_handle, "ZSTD_decompress_usingDict");
    void *ZSTD_freeCCtx_ptr                = dlsym(library_handle, "ZSTD_freeCCtx");
    void *ZSTD_freeCDict_ptr               = dlsym(library_handle, "ZSTD_freeCDict");
    void *ZSTD_freeDCtx_ptr                = dlsym(library_handle, "ZSTD_freeDCtx");
    void *ZSTD_freeDDict_ptr               = dlsym(library_handle, "ZSTD_freeDDict");
    void *ZSTD_sizeof_DDict_ptr            = dlsym(library_handle, "ZSTD_sizeof_DDict");
    void *ZSTD_getErrorName_ptr            = dlsym(library_handle, "ZSTD_getErrorName");
    void *ZSTD_isError_ptr                 = dlsym(library_handle, "ZSTD_isError");
    void *ZSTD_versionNumber_ptr           = dlsym(library_handle, "ZSTD_versionNumber");
    
    if(
        !ZSTD_compress_ptr                ||
        !ZSTD_compressBound_ptr           ||
        !ZSTD_compress_usingCDict_ptr     ||
        !ZSTD_compress_usingDict_ptr      ||
        !ZSTD_createCCtx_ptr              ||
        !ZSTD_createCCtx_advanced_ptr     ||
        !ZSTD_createCDict_ptr             ||
        !ZSTD_createDCtx_ptr              ||
        !ZSTD_createDCtx_advanced_ptr     ||
        !ZSTD_createDDict_byReference_ptr ||
        !ZSTD_decompress_ptr              ||
        !ZSTD_decompress_usingDDict_ptr   ||
        !ZSTD_decompress_usingDict_ptr    ||
        !ZSTD_freeCCtx_ptr                ||
        !ZSTD_freeCDict_ptr               ||
        !ZSTD_freeDCtx_ptr                ||
        !ZSTD_freeDDict_ptr               ||
        !ZSTD_sizeof_DDict_ptr            ||
        !ZSTD_getErrorName_ptr            ||
        !ZSTD_isError_ptr                 ||
        !ZSTD_versionNumber_ptr
    )
        return;
    
    compression_service_zstd->ZSTD_compress_ptr                = (PTR_ZSTD_compress)                ZSTD_compress_ptr;
    compression_service_zstd->ZSTD_compressBound_ptr           = (PTR_ZSTD_compressBound)           ZSTD_compressBound_ptr;
    compression_service_zstd->ZSTD_compress_usingCDict_ptr     = (PTR_ZSTD_compress_usingCDict)     ZSTD_compress_usingCDict_ptr;
    compression_service_zstd->ZSTD_compress_usingDict_ptr      = (PTR_ZSTD_compress_usingDict)      ZSTD_compress_usingDict_ptr;
    compression_service_zstd->ZSTD_createCCtx_ptr              = (PTR_ZSTD_createCCtx)              ZSTD_createCCtx_ptr;
    compression_service_zstd->ZSTD_createCCtx_advanced_ptr     = (PTR_ZSTD_createCCtx_advanced)     ZSTD_createCCtx_advanced_ptr;
    compression_service_zstd->ZSTD_createCDict_ptr             = (PTR_ZSTD_createCDict)             ZSTD_createCDict_ptr;
    compression_service_zstd->ZSTD_createDCtx_ptr              = (PTR_ZSTD_createDCtx)              ZSTD_createDCtx_ptr;
    compression_service_zstd->ZSTD_createDCtx_advanced_ptr     = (PTR_ZSTD_createDCtx_advanced)     ZSTD_createDCtx_advanced_ptr;
    compression_service_zstd->ZSTD_createDDict_byReference_ptr = (PTR_ZSTD_createDDict_byReference) ZSTD_createDDict_byReference_ptr;
    compression_service_zstd->ZSTD_decompress_ptr              = (PTR_ZSTD_decompress)              ZSTD_decompress_ptr;
    compression_service_zstd->ZSTD_decompress_usingDDict_ptr   = (PTR_ZSTD_decompress_usingDDict)   ZSTD_decompress_usingDDict_ptr;
    compression_service_zstd->ZSTD_decompress_usingDict_ptr    = (PTR_ZSTD_decompress_usingDict)    ZSTD_decompress_usingDict_ptr;
    compression_service_zstd->ZSTD_freeCCtx_ptr                = (PTR_ZSTD_freeCCtx)                ZSTD_freeCCtx_ptr;
    compression_service_zstd->ZSTD_freeCDict_ptr               = (PTR_ZSTD_freeCDict)               ZSTD_freeCDict_ptr;
    compression_service_zstd->ZSTD_freeDCtx_ptr                = (PTR_ZSTD_freeDCtx)                ZSTD_freeDCtx_ptr;
    compression_service_zstd->ZSTD_freeDDict_ptr               = (PTR_ZSTD_freeDDict)               ZSTD_freeDDict_ptr;
    compression_service_zstd->ZSTD_getErrorName_ptr            = (PTR_ZSTD_getErrorName)            ZSTD_getErrorName_ptr;
    compression_service_zstd->ZSTD_isError_ptr                 = (PTR_ZSTD_isError)                 ZSTD_isError_ptr;
    compression_service_zstd->ZSTD_sizeof_DDict_ptr            = (PTR_ZSTD_sizeof_DDict)            ZSTD_sizeof_DDict_ptr;
    compression_service_zstd->ZSTD_versionNumber_ptr           = (PTR_ZSTD_versionNumber)           ZSTD_versionNumber_ptr;
    
    COMPRESSION_LOADED_ZSTD = true;
} 
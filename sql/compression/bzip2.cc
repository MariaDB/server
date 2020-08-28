#include "compression_libs.h"
#include <dlfcn.h>

bool COMPRESSION_LOADED_BZIP2 = false;


DEFINE_BZ2_bzBuffToBuffCompress(DUMMY_BZ2_bzBuffToBuffCompress){
    return -1;
}

DEFINE_BZ2_bzBuffToBuffDecompress(DUMMY_BZ2_bzBuffToBuffDecompress){
    return -1;
}

DEFINE_BZ2_bzCompress(DUMMY_BZ2_bzCompress){
    return -1;
}

DEFINE_BZ2_bzCompressEnd(DUMMY_BZ2_bzCompressEnd){
    return -1;
}

DEFINE_BZ2_bzCompressInit(DUMMY_BZ2_bzCompressInit){
    return -1;
}

DEFINE_BZ2_bzDecompress(DUMMY_BZ2_bzDecompress){
    return -1;
}

DEFINE_BZ2_bzDecompressEnd(DUMMY_BZ2_bzDecompressEnd){
    return -1;
}

DEFINE_BZ2_bzDecompressInit(DUMMY_BZ2_bzDecompressInit){
    return -1;
}


void init_bzip2(struct compression_service_bzip2_st *handler, bool load_library){
    //point struct to right place for static plugins
    compression_service_bzip2 = handler;
    
    compression_service_bzip2->BZ2_bzBuffToBuffCompress_ptr   = DUMMY_BZ2_bzBuffToBuffCompress;
    compression_service_bzip2->BZ2_bzBuffToBuffDecompress_ptr = DUMMY_BZ2_bzBuffToBuffDecompress;
    compression_service_bzip2->BZ2_bzCompress_ptr             = DUMMY_BZ2_bzCompress;
    compression_service_bzip2->BZ2_bzCompressEnd_ptr          = DUMMY_BZ2_bzCompressEnd;
    compression_service_bzip2->BZ2_bzCompressInit_ptr         = DUMMY_BZ2_bzCompressInit;
    compression_service_bzip2->BZ2_bzDecompress_ptr           = DUMMY_BZ2_bzDecompress;
    compression_service_bzip2->BZ2_bzDecompressEnd_ptr        = DUMMY_BZ2_bzDecompressEnd;
    compression_service_bzip2->BZ2_bzDecompressInit_ptr       = DUMMY_BZ2_bzDecompressInit;

	if(!load_library)
		return;

    //Load BZip2 library dynamically
    void *library_handle = dlopen("libbz2.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if(!library_handle || dlerror())
		return;

    void *BZ2_bzBuffToBuffCompress_ptr   = dlsym(library_handle, "BZ2_bzBuffToBuffCompress");
    void *BZ2_bzBuffToBuffDecompress_ptr = dlsym(library_handle, "BZ2_bzBuffToBuffDecompress");
    void *BZ2_bzCompress_ptr             = dlsym(library_handle, "BZ2_bzCompress");
    void *BZ2_bzCompressEnd_ptr          = dlsym(library_handle, "BZ2_bzCompressEnd");
    void *BZ2_bzCompressInit_ptr         = dlsym(library_handle, "BZ2_bzCompressInit");
    void *BZ2_bzDecompress_ptr           = dlsym(library_handle, "BZ2_bzDecompress");
    void *BZ2_bzDecompressEnd_ptr        = dlsym(library_handle, "BZ2_bzDecompressEnd");
    void *BZ2_bzDecompressInit_ptr       = dlsym(library_handle, "BZ2_bzDecompressInit");
	if(
        !BZ2_bzBuffToBuffCompress_ptr   ||
        !BZ2_bzBuffToBuffDecompress_ptr ||
        !BZ2_bzCompress_ptr             ||
        !BZ2_bzCompressEnd_ptr          ||
        !BZ2_bzCompressInit_ptr         ||
        !BZ2_bzDecompress_ptr           ||
        !BZ2_bzDecompressEnd_ptr        ||
        !BZ2_bzDecompressInit_ptr
    )
        return;

    compression_service_bzip2->BZ2_bzBuffToBuffCompress_ptr   = (PTR_BZ2_bzBuffToBuffCompress)   BZ2_bzBuffToBuffCompress_ptr;
    compression_service_bzip2->BZ2_bzBuffToBuffDecompress_ptr = (PTR_BZ2_bzBuffToBuffDecompress) BZ2_bzBuffToBuffDecompress_ptr;
    compression_service_bzip2->BZ2_bzCompress_ptr             = (PTR_BZ2_bzCompress)             BZ2_bzCompress_ptr;
    compression_service_bzip2->BZ2_bzCompressEnd_ptr          = (PTR_BZ2_bzCompressEnd)          BZ2_bzCompressEnd_ptr;
    compression_service_bzip2->BZ2_bzCompressInit_ptr         = (PTR_BZ2_bzCompressInit)         BZ2_bzCompressInit_ptr;
    compression_service_bzip2->BZ2_bzDecompress_ptr           = (PTR_BZ2_bzDecompress)           BZ2_bzDecompress_ptr;
    compression_service_bzip2->BZ2_bzDecompressEnd_ptr        = (PTR_BZ2_bzDecompressEnd)        BZ2_bzDecompressEnd_ptr;
    compression_service_bzip2->BZ2_bzDecompressInit_ptr       = (PTR_BZ2_bzDecompressInit)       BZ2_bzDecompressInit_ptr;

    COMPRESSION_LOADED_BZIP2 = true;
}

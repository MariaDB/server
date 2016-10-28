
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#ifndef DENA_ALLOCATOR_HPP
#define DENA_ALLOCATOR_HPP

#if 0
extern "C" {
#include <tlsf.h>
};
#define DENA_MALLOC(x) tlsf_malloc(x)
#define DENA_REALLOC(x, y) tlsf_realloc(x, y)
#define DENA_FREE(x) tlsf_free(x)
#define DENA_NEWCHAR(x) static_cast<char *>(tlsf_malloc(x))
#define DENA_DELETE(x) tlsf_free(x)
#endif

#if 1
#define DENA_MALLOC(x) malloc(x)
#define DENA_REALLOC(x, y) realloc(x, y)
#define DENA_FREE(x) free(x)
#define DENA_NEWCHAR(x) (new char[x])
#define DENA_DELETE(x) (delete [] x)
#endif

#if 1
#define DENA_ALLOCA_ALLOCATE(typ, len) \
	(typ *) alloca((len) * sizeof(typ))
#define DENA_ALLOCA_FREE(x)
#else
#define DENA_ALLOCA_ALLOCATE(typ, len) \
	static_cast<typ *>(malloc((len) * sizeof(typ)))
#define DENA_ALLOCA_FREE(x) free(x)
#endif

#endif


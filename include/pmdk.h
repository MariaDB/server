#ifndef USE_PMDK
#error USE_PMDK not defined
#endif
#pragma once

#include <dlfcn.h>
#include <libpmem.h>
#include <stdio.h>
#include <iostream>

#ifdef __linux__

#include <mntent.h>

#endif

#include <stdexcept>
#include <thread>
#include <assert.h>

extern void *pmdk_handle;

extern decltype(&pmem_map_file) original_pmem_map_file;
extern decltype(&pmem_errormsg) original_pmem_errormsg;
extern decltype(&pmem_memcpy_nodrain) original_pmem_memcpy_nodrain;
extern decltype(&pmem_memmove_nodrain) original_pmem_memmove_nodrain;
extern decltype(&pmem_memcpy_persist) original_pmem_memcpy_persist;
extern decltype(&pmem_flush) original_pmem_flush;
extern decltype(&pmem_unmap) original_pmem_unmap;

template<typename T>
static inline bool regist_func(void *handler, T &func,
			       const char *description) {
	func = (T) dlsym(handler, description);
	if (func == nullptr) {
		dlclose(pmdk_handle);
		return false;
	}
	return true;
}

#define OPEN_LIB(handler, desp, error_msg)                     \
  handler = dlopen(desp, RTLD_NOW);                            \
  if (handler == nullptr) {                                    \
    error_msg = std::string("open ") + desp +                  \
                std::string(".so failed, make sure ") + desp + \
                std::string(".so exists in library path.");    \
    return true;                                               \
  }

#define REGIST_FUNC(handler, T, tv, desp, error_msg)             \
  if (!regist_func<T>(handler, tv, desp)) {                      \
    error_msg = std::string("open") + std::string(desp) +        \
                std::string("function failed using libpmem.so"); \
    return true;                                                 \
  }

bool init_pmdk_library(std::string &);


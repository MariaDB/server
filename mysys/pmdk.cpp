/*
 * pmdk.cc is used to wrapper function for PMDK lib and NUMA lib
 *
 * PMDK lib and NUMA lib open and use with dlopen,
 * All functions are wrapped by dlsym
 */
#ifdef USE_PMDK
#include "pmdk.h"

void *pmdk_handle = nullptr;
void *numa_handle = nullptr;
/*
 * Wrapper primitive pmem function
 */
decltype(&pmem_map_file) original_pmem_map_file;
decltype(&pmem_errormsg) original_pmem_errormsg;
decltype(&pmem_memcpy_nodrain) original_pmem_memcpy_nodrain;
decltype(&pmem_memmove_nodrain) original_pmem_memmove_nodrain;
decltype(&pmem_memcpy_persist) original_pmem_memcpy_persist;
decltype(&pmem_flush) original_pmem_flush;
decltype(&pmem_unmap) original_pmem_unmap;

//decltype(&numa_available) my_numa_available;
//decltype(&numa_bitmask_alloc) my_numa_bitmask_alloc;
//decltype(&numa_node_to_cpus) my_numa_node_to_cpus;
//decltype(&numa_num_possible_cpus) my_numa_num_possible_cpus;
//decltype(&numa_bitmask_isbitset) my_numa_bitmask_isbitset;

void *pmem_map_file(const char *path, size_t len, int flags, mode_t mode,
                    size_t *mapped_lenp, int *is_pmemp) {
  return original_pmem_map_file(path, len, flags, mode, mapped_lenp, is_pmemp);
}

const char *pmem_errormsg() {
  return original_pmem_errormsg();
}

void *pmem_memcpy_nodrain(void *pmemdest, const void *src, size_t len) {
  return original_pmem_memcpy_nodrain(pmemdest, src, len);
}

void *pmem_memmove_nodrain(void *pmemdest, const void *src, size_t len) {
  return original_pmem_memmove_nodrain(pmemdest, src, len);
}

void *pmem_memcpy_persist(void *pmemdest, const void *src, size_t len) {
  return original_pmem_memcpy_persist(pmemdest, src, len);
}

void pmem_flush(const void *addr, size_t len) {
  return original_pmem_flush(addr, len);
}

int pmem_unmap(void *addr, size_t len) {
  return original_pmem_unmap(addr, len);
}

bool init_pmdk_library(std::string &error_message) {
  OPEN_LIB(pmdk_handle, "libpmem.so", error_message)

  REGIST_FUNC(pmdk_handle, decltype(&pmem_map_file), original_pmem_map_file, "pmem_map_file", error_message);

  REGIST_FUNC(pmdk_handle, decltype(&pmem_errormsg), original_pmem_errormsg, "pmem_errormsg", error_message);

  REGIST_FUNC(pmdk_handle, decltype(&pmem_memcpy_nodrain), original_pmem_memcpy_nodrain, "pmem_memcpy_nodrain",
              error_message);

  REGIST_FUNC(pmdk_handle, decltype(&pmem_memmove_nodrain), original_pmem_memmove_nodrain, "pmem_memmove_nodrain",
              error_message);

  REGIST_FUNC(pmdk_handle, decltype(&pmem_memcpy_persist), original_pmem_memcpy_persist, "pmem_memcpy_persist",
              error_message);

  REGIST_FUNC(pmdk_handle, decltype(&pmem_flush), original_pmem_flush, "pmem_flush", error_message);

  REGIST_FUNC(pmdk_handle, decltype(&pmem_unmap), original_pmem_unmap, "pmem_unmap", error_message);

  return false;
}

//bool init_numa_library(std::string &error_message) {
//  OPEN_LIB(numa_handle, "libnuma.so", error_message)
//
//  REGIST_FUNC(numa_handle, decltype(&numa_available), my_numa_available, "numa_available", error_message);
//
//  REGIST_FUNC(numa_handle, decltype(&numa_bitmask_alloc), my_numa_bitmask_alloc, "numa_bitmask_alloc",
//              error_message);
//
//  REGIST_FUNC(numa_handle, decltype(&numa_node_to_cpus), my_numa_node_to_cpus, "numa_node_to_cpus",
//              error_message);
//
//  REGIST_FUNC(numa_handle, decltype(&numa_num_possible_cpus), my_numa_num_possible_cpus, "numa_num_possible_cpus",
//              error_message);
//
//  REGIST_FUNC(numa_handle, decltype(&numa_bitmask_isbitset), my_numa_bitmask_isbitset, "numa_bitmask_isbitset",
//              error_message);
//
//  if (my_numa_available()) {
//    error_message = std::string("NUMA is not available in this system");
//    return true;
//  }
//  return false;
//}

#endif //USE_PMDK
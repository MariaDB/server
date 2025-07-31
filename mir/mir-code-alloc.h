/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef MIR_CODE_ALLOC_H
#define MIR_CODE_ALLOC_H

#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_FAILED NULL

typedef enum MIR_mem_protect {
  PROT_WRITE_EXEC,
  PROT_READ_EXEC
} MIR_mem_protect_t;

typedef struct MIR_code_alloc {
  void *(*mem_map) (size_t, void *);
  int (*mem_unmap) (void *, size_t, void *);
  int (*mem_protect) (void *, size_t, MIR_mem_protect_t, void *);
  void *user_data;
} *MIR_code_alloc_t;

static inline void *MIR_mem_map (MIR_code_alloc_t code_alloc, size_t len) {
  return code_alloc->mem_map (len, code_alloc->user_data);
}

static inline int MIR_mem_unmap (MIR_code_alloc_t code_alloc, void *addr, size_t len) {
  return code_alloc->mem_unmap (addr, len, code_alloc->user_data);
}

static inline int MIR_mem_protect (MIR_code_alloc_t code_alloc, void *addr, size_t len, MIR_mem_protect_t prot) {
  return code_alloc->mem_protect (addr, len, prot, code_alloc->user_data);
}

#ifdef __cplusplus
}
#endif

#endif /* #ifndef MIR_CODE_ALLOC_H */

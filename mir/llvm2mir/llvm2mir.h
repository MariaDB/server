/* This file is a part of MIR project.
   Copyright (C) 2019-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.

   Interface of translator of LLVM bitcode into MIR.
*/

#include <llvm-c/Core.h>
#include "mir.h"

extern MIR_module_t llvm2mir (MIR_context_t context, LLVMModuleRef module);

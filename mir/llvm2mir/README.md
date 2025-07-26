# LLVM bitcode to MIR translator

## Reasons for implementing the translator

* Facilitating future MIR support for more programming languages and their extensions
* Facilitating future MIR support for more ABIs
* Facilitating future MIR support for optimization hints
* Facilitating future MIR support for cross-compiling and support for more targets

## The current state of the translator

* Currently we are focused on translating only native LLVM bitcode of standard C/C++
* LLVM bitcode vectors are no supported yet.  In future the vector code will be scalarized to be translated into MIR
  * Use -fno-vectorize to avoid LLVM bitcode vectors
* Full x86-64 call ABI (multiple return regs) are not implemented yet
* Many optimization hints are ignored.  Others should result in a compilation failure
* To implement the translator, we use LLVM C interface as more stable one

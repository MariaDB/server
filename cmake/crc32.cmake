IF(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|AARCH64")
  IF(CMAKE_COMPILER_IS_GNUCC AND NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 5.1)
    include(CheckCXXSourceCompiles)

    CHECK_CXX_SOURCE_COMPILES("
    #define CRC32CX(crc, value) __asm__(\"crc32cx %w[c], %w[c], %x[v]\":[c]\"+r\"(crc):[v]\"r\"(value))
    asm(\".arch_extension crc\");
    unsigned int foo(unsigned int ret) {
      CRC32CX(ret, 0);
      return ret;
    }
    int main() { foo(0); }" HAVE_ARMV8_CRC)

    CHECK_CXX_SOURCE_COMPILES("
    asm(\".arch_extension crypto\");
    unsigned int foo(unsigned int ret) {
      __asm__(\"pmull  v2.1q,          v2.1d,  v1.1d\");
      return ret;
    }
    int main() { foo(0); }" HAVE_ARMV8_CRYPTO)

    CHECK_C_COMPILER_FLAG(-march=armv8-a+crc+crypto HAVE_ARMV8_CRC_CRYPTO_INTRINSICS)
    IF(HAVE_ARMV8_CRC_CRYPTO_INTRINSICS)
      SET(ARMV8_CRC_COMPILE_FLAGS "${ARMV8_CRC_COMPILE_FLAGS} -march=armv8-a+crc+crypto")
    ENDIF()

    SET(CRC32_LIBRARY crc32_armv8_neon)
    ADD_SUBDIRECTORY(extra/crc32_armv8_neon)
  ENDIF()
ENDIF()

IF(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64")
  SET(HAVE_CRC32_VPMSUM 1)
  SET(CRC32_LIBRARY crc32-vpmsum)
  ADD_SUBDIRECTORY(extra/crc32-vpmsum)
ENDIF()

IF(NOT CMAKE_CROSSCOMPILING AND NOT MSVC)
  STRING(TOLOWER ${CMAKE_SYSTEM_PROCESSOR}  processor)
  IF(processor MATCHES "86" OR processor MATCHES "amd64" OR processor MATCHES "x64")
  #Check for PCLMUL instruction
  CHECK_C_SOURCE_RUNS("
  int main()
  {
    asm volatile (\"pclmulqdq \\$0x00, %%xmm1, %%xmm0\":::\"cc\");
    return 0;
  }"  HAVE_CLMUL_INSTRUCTION)
  ENDIF()

  IF(HAVE_CLMUL_INSTRUCTION)
    SET(CRC32_LIBRARY crc32-x86-pclmul)
    ADD_SUBDIRECTORY(extra/crc32-x86-pclmul)
  ENDIF()
ENDIF()

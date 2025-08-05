IF(MSVC_INTEL)
  PROJECT(wolfssl C ASM_MASM)
ELSEIF(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  PROJECT(wolfssl C ASM)
ELSE()
  PROJECT(wolfssl C)
ENDIF()

IF(CMAKE_SIZEOF_VOID_P MATCHES 8)
IF(MSVC_INTEL AND NOT (CMAKE_C_COMPILER_ID MATCHES Clang))
  SET(WOLFSSL_INTELASM ON)
  SET(HAVE_INTEL_RDSEED 1)
  SET(HAVE_INTEL_RDRAND 1)
ELSEIF(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  IF(CMAKE_C_COMPILER_ID MATCHES GNU  AND CMAKE_C_COMPILER_VERSION VERSION_LESS 4.9)
    MESSAGE_ONCE(NO_INTEL_ASSEMBLY "Disable Intel assembly for WolfSSL - compiler is too old")
  ELSEIF(WITH_MSAN)
    MESSAGE_ONCE(MSAN_CANT_HANDLE_IT "Disable Intel assembly for WolfSSL - MSAN can't handle it")
  ELSE()
    MY_CHECK_C_COMPILER_FLAG(-maes)
    MY_CHECK_C_COMPILER_FLAG(-msse4)
    MY_CHECK_C_COMPILER_FLAG(-mpclmul)
    IF(have_C__maes AND have_C__msse4 AND have_C__mpclmul)
      SET(WOLFSSL_INTELASM ON)
      MY_CHECK_C_COMPILER_FLAG(-mrdrnd)
      MY_CHECK_C_COMPILER_FLAG(-mrdseed)
      IF(have_C__mrdrnd)
        SET(HAVE_INTEL_RDRAND ON)
      ENDIF()
      IF(have_C__mrdseed)
        SET(HAVE_INTEL_RDSEED ON)
      ENDIF()
    ENDIF()
  ENDIF()
ENDIF()
ENDIF()

SET(WOLFSSL_SRCDIR ${CMAKE_CURRENT_SOURCE_DIR}/wolfssl/src)
SET(WOLFCRYPT_SRCDIR ${CMAKE_CURRENT_SOURCE_DIR}/wolfssl/wolfcrypt/src)
ADD_DEFINITIONS(${SSL_DEFINES})
ADD_DEFINITIONS(-DWOLFSSL_LIB -DBUILDING_WOLFSSL)
ADD_DEFINITIONS(-DWOLFSSL_SP_4096)
INCLUDE_DIRECTORIES(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/wolfssl)
INCLUDE_DIRECTORIES(${SSL_INCLUDE_DIRS})

add_library(wolfssl STATIC
${WOLFSSL_SRCDIR}/crl.c
${WOLFSSL_SRCDIR}/internal.c
${WOLFSSL_SRCDIR}/keys.c
${WOLFSSL_SRCDIR}/tls.c
${WOLFSSL_SRCDIR}/wolfio.c
${WOLFSSL_SRCDIR}/ocsp.c
${WOLFSSL_SRCDIR}/ssl.c
${WOLFSSL_SRCDIR}/tls13.c
${WOLFCRYPT_SRCDIR}/aes.c
${WOLFCRYPT_SRCDIR}/arc4.c
${WOLFCRYPT_SRCDIR}/asn.c
${WOLFCRYPT_SRCDIR}/coding.c
${WOLFCRYPT_SRCDIR}/des3.c
${WOLFCRYPT_SRCDIR}/dh.c
${WOLFCRYPT_SRCDIR}/dsa.c
${WOLFCRYPT_SRCDIR}/ecc.c
${WOLFCRYPT_SRCDIR}/error.c
${WOLFCRYPT_SRCDIR}/hmac.c
${WOLFCRYPT_SRCDIR}/logging.c
${WOLFCRYPT_SRCDIR}/md4.c
${WOLFCRYPT_SRCDIR}/md5.c
${WOLFCRYPT_SRCDIR}/memory.c
${WOLFCRYPT_SRCDIR}/pkcs12.c
${WOLFCRYPT_SRCDIR}/pwdbased.c
${WOLFCRYPT_SRCDIR}/random.c
${WOLFCRYPT_SRCDIR}/rsa.c
${WOLFCRYPT_SRCDIR}/sha.c
${WOLFCRYPT_SRCDIR}/sha256.c
${WOLFCRYPT_SRCDIR}/sha512.c
${WOLFCRYPT_SRCDIR}/poly1305.c
${WOLFCRYPT_SRCDIR}/chacha.c
${WOLFCRYPT_SRCDIR}/chacha20_poly1305.c
${WOLFCRYPT_SRCDIR}/wc_port.c
${WOLFCRYPT_SRCDIR}/wc_encrypt.c
${WOLFCRYPT_SRCDIR}/hash.c
${WOLFCRYPT_SRCDIR}/wolfmath.c
${WOLFCRYPT_SRCDIR}/kdf.c
${WOLFCRYPT_SRCDIR}/sp_int.c
${WOLFCRYPT_SRCDIR}/sp_c32.c
${WOLFCRYPT_SRCDIR}/sp_c64.c
)

# Optimizations, assembly
if(WOLFSSL_INTELASM)
  set(WOLFSSL_X86_64_BUILD 1)
  set(WOLFSSL_SP_X86_64 1)
  set(WOLFSSL_SP_X86_64_ASM 1)
  set(WOLFSSL_AESNI 1)
  target_sources(wolfssl PRIVATE
    ${WOLFCRYPT_SRCDIR}/cpuid.c
    ${WOLFCRYPT_SRCDIR}/sp_x86_64.c
  )
  if(MSVC_INTEL)
    target_sources(wolfssl PRIVATE
      ${WOLFCRYPT_SRCDIR}/aes_asm.asm
      ${WOLFCRYPT_SRCDIR}/aes_gcm_asm.asm
      ${WOLFCRYPT_SRCDIR}/sp_x86_64_asm.asm
    )
    target_compile_options(wolfssl PRIVATE
      $<$<COMPILE_LANG_AND_ID:C,Clang>:-maes -msse4.2 -mpclmul -mrdrnd -mrdseed>
      $<$<COMPILE_LANGUAGE:ASM_MASM>:/Zi>
    )
  else()
    set(USE_INTEL_SPEEDUP 1)
    target_sources(wolfssl PRIVATE
      ${WOLFCRYPT_SRCDIR}/aes_asm.S
      ${WOLFCRYPT_SRCDIR}/aes_gcm_asm.S
      ${WOLFCRYPT_SRCDIR}/chacha_asm.S
      ${WOLFCRYPT_SRCDIR}/poly1305_asm.S
      ${WOLFCRYPT_SRCDIR}/sha512_asm.S
      ${WOLFCRYPT_SRCDIR}/sha256_asm.S
      ${WOLFCRYPT_SRCDIR}/sp_x86_64_asm.S
    )
    target_compile_options(wolfssl PRIVATE -maes -msse4.2 -mpclmul)
    # Workaround 5.5.4 bug (user_settings.h not included into aes_asm.S)
    set_property(SOURCE ${WOLFCRYPT_SRCDIR}/aes_asm.S APPEND PROPERTY COMPILE_OPTIONS "-DWOLFSSL_X86_64_BUILD")
  endif()
endif()

if (MSVC_ARM64 AND (CMAKE_C_COMPILER_ID MATCHES Clang))
  # Compile flags
  MESSAGE("WolfSSL - Activate ARM64 optimizations")
  set(WOLFSSL_ARM64_ASM 1)
  set(WOLFSSL_SP_ARM64 1)
  set(WOLFSSL_ARMASM 1)
  set(WOLFSSL_SP_ARM64_ASM 1)
  set(WOLFSSL_ARMASM_CRYPTO_SHA512 1)
  set(WOLFSSL_ARMASM_INLINE 1)
  set(TFM_ARM 1)
  set(WOLFSSL_AARCH64_BUILD 1)
  set(POLY130564 1)
  set(SQR_MUL_ASM 1)

  target_compile_options(wolfssl PRIVATE -march=armv8.2-a+crc+crypto+sha3)
  set(WOLFCRYPT_PORT_SRCDIR ${CMAKE_CURRENT_SOURCE_DIR}/wolfssl/wolfcrypt/src/port/arm)
  target_sources(wolfssl PRIVATE
    ${WOLFCRYPT_SRCDIR}/cpuid.c
    ${WOLFCRYPT_SRCDIR}/sp_arm64.c
    ${WOLFCRYPT_PORT_SRCDIR}/armv8-aes.c
    ${WOLFCRYPT_PORT_SRCDIR}/armv8-aes.c
    ${WOLFCRYPT_PORT_SRCDIR}/armv8-chacha.c
    ${WOLFCRYPT_PORT_SRCDIR}/armv8-sha256.c
    ${WOLFCRYPT_PORT_SRCDIR}/armv8-sha512.c
    ${WOLFCRYPT_PORT_SRCDIR}/armv8-sha512-asm.S
    ${WOLFCRYPT_PORT_SRCDIR}/armv8-sha512-asm_c.c
    ${WOLFCRYPT_PORT_SRCDIR}/armv8-poly1305.c
  )
endif()

# Silence some warnings
if(MSVC)
  # truncation warnings
  target_compile_options(wolfssl PRIVATE $<$<COMPILE_LANGUAGE:C>:/wd4244 /wd5287 /wd5286>)
  target_compile_definitions(wolfssl PUBLIC WC_NO_STATIC_ASSERT)
  if(CMAKE_C_COMPILER_ID MATCHES Clang)
    target_compile_options(wolfssl PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-function-pointer-types>)
  endif()
  remove_definitions(-DHAVE_CONFIG_H)
  target_compile_definitions(wolfssl PRIVATE
    WOLFSSL_HAVE_MIN WOLFSSL_HAVE_MAX)
  # Workaround https://github.com/wolfSSL/wolfssl/issues/9004
  target_compile_definitions(wolfssl PRIVATE WOLFSSL_NO_SOCK SOCKET_INVALID=-1)
endif()

CONFIGURE_FILE(user_settings.h.in user_settings.h)


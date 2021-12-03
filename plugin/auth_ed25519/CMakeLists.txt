SET(REF10_SOURCES
      ref10/fe_0.c ref10/fe_1.c ref10/fe_add.c ref10/fe_cmov.c ref10/fe_copy.c
      ref10/fe_frombytes.c ref10/fe_invert.c ref10/fe_isnegative.c
      ref10/fe_isnonzero.c ref10/fe_mul.c ref10/fe_neg.c ref10/fe_pow22523.c
      ref10/fe_sq.c ref10/fe_sq2.c ref10/fe_sub.c ref10/fe_tobytes.c
      ref10/ge_add.c ref10/ge_double_scalarmult.c ref10/ge_frombytes.c
      ref10/ge_madd.c ref10/ge_msub.c ref10/ge_p1p1_to_p2.c
      ref10/ge_p1p1_to_p3.c ref10/ge_p2_0.c ref10/ge_p2_dbl.c ref10/ge_p3_0.c
      ref10/ge_p3_dbl.c ref10/ge_p3_to_cached.c ref10/ge_p3_to_p2.c
      ref10/ge_p3_tobytes.c ref10/ge_precomp_0.c ref10/ge_scalarmult_base.c
      ref10/ge_sub.c ref10/ge_tobytes.c ref10/keypair.c ref10/open.c
      ref10/sc_muladd.c ref10/sc_reduce.c ref10/sign.c ref10/verify.c)

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR})

ADD_CONVENIENCE_LIBRARY(ref10 ${REF10_SOURCES})
IF(MSVC)
   # Silence conversion (integer truncantion) warnings from reference code
  SET_SOURCE_FILES_PROPERTIES(${REF10_SOURCES} PROPERTY COMPILE_FLAGS "/wd4244 /wd4146")
ENDIF()

IF(CMAKE_C_COMPILER_ID MATCHES "GNU" AND CMAKE_C_COMPILER_VERSION LESS 11 AND CMAKE_C_COMPILER_VERSION GREATER 6)
  SET_SOURCE_FILES_PROPERTIES(${REF10_SOURCES} PROPERTY COMPILE_FLAGS -fno-sanitize=shift)
ENDIF()

# server plugin *cannot* link with the library, it needs all sources to be
# compiled with MYSQL_DYNAMIC_PLUGIN
MYSQL_ADD_PLUGIN(auth_ed25519 server_ed25519.c ${REF10_SOURCES} MODULE_ONLY)

# client plugin and unit test ed25519-t can use the library
#MYSQL_ADD_PLUGIN(client_ed25519 client_ed25519.c MODULE_ONLY
#  CLIENT LINK_LIBRARIES ref10 mysys_ssl COMPONENT ClientPlugins)

IF(WITH_UNIT_TESTS)
  MY_ADD_TESTS(ed25519 LINK_LIBRARIES ref10 mysys_ssl)
ENDIF()

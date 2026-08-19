# MariaDB service ABI definitions and generated ABI files.
#
# This file is included from the top-level CMakeLists.txt:
#
#     include(cmake/mariadb_services.cmake)
#
# It is the single source of truth for the service ABI versions.
#
# Each service is:
#
#     <name> <version> <ELF symbol>
#
# Version format:
#
#     0xMMmm
#
# which becomes:
#
#     MARIADB_<name>_MM.mm
#
# For example:
#
#     my_sha1  0x0101  my_sha1_service
#
# becomes:
#
#     VERSION_my_sha1 = 0x0101
#     MARIADB_SYMVER_my_sha1_service = "MARIADB_my_sha1_1.1"
#
# and the linker script exports:
#
#     MARIADB_my_sha1_1.1 {
#         global:
#             my_sha1_service;
#     };


# ---------------------------------------------------------------------------
# Service definitions
# ---------------------------------------------------------------------------

set(MARIADB_SERVICES
    # name                  version  ELF symbol

    debug_sync              0x1000   debug_sync_service
    kill_statement          0x1000   thd_kill_statement_service

    base64                  0x0100   base64_service
    encryption              0x0300   encryption_service
    encryption_scheme       0x0100   encryption_scheme_service
    logger                  0x0100   logger_service
    thd_log_warnings        0x0100   thd_log_warnings_service
    my_crypt                0x0100   my_crypt_service
    my_md5                  0x0100   my_md5_service
    my_print_error          0x0100   my_print_error_service
    my_sha1                 0x0101   my_sha1_service
    my_sha2                 0x0100   my_sha2_service
    my_snprintf             0x0100   my_snprintf_service
    progress_report         0x0100   progress_report_service

    thd_alloc               0x0200   thd_alloc_service
    thd_autoinc             0x0100   thd_autoinc_service
    thd_error_context       0x0200   thd_error_context_service
    thd_rnd                 0x0100   thd_rnd_service
    thd_specifics           0x0100   thd_specifics_service
    thd_timezone            0x0100   thd_timezone_service
    thd_wait                0x0100   thd_wait_service

    wsrep                   0x0500   wsrep_service
    json                    0x0100   json_service
    sql_service             0x0102   sql_service_handler
    thd_mdl                 0x0100   thd_mdl_service
    print_check_msg         0x0100   print_check_msg_service

    provider_bzip2          0x0100   provider_service_bzip2
    provider_lz4            0x0100   provider_service_lz4
    provider_lzma           0x0100   provider_service_lzma
    provider_lzo            0x0100   provider_service_lzo
    provider_snappy         0x0100   provider_service_snappy
)


# ---------------------------------------------------------------------------
# Generated files
# ---------------------------------------------------------------------------

set(MARIADB_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

set(MARIADB_VERSION_HEADER
    "${CMAKE_CURRENT_BINARY_DIR}/include/service_versions.h"
)

set(MARIADB_VERSION_SCRIPT
    "${CMAKE_CURRENT_BINARY_DIR}/sql/mariadb_services.lds"
)

# ---------------------------------------------------------------------------
# Generate service_versions.h and the linker version script
# ---------------------------------------------------------------------------

set(_MARIADB_HEADER [=[
/* Generated file -- DO NOT EDIT. */
#pragma once

#ifdef _WIN32
#define SERVICE_VERSION __declspec(dllexport) void *
#else
#define SERVICE_VERSION void *
#endif


#if defined(__ELF__) && defined(__GNUC__)

#define MARIADB_SERVICE_VERSION(name, symbol, version) \
  MARIADB_SERVICE_VERSION_IMPL(name, symbol, version)

#define MARIADB_SERVICE_VERSION_IMPL(name, symbol, version) \
  _Static_assert(                                             \
      (version) == VERSION_##name,                            \
      "MariaDB service ABI version mismatch: " #name);        \
  __asm__(".symver " #symbol "," #symbol "@"                 \
          MARIADB_SYMVER_##symbol)

#else

#define MARIADB_SERVICE_VERSION(name, symbol, version) \
  _Static_assert(                                       \
      (version) == VERSION_##name,                      \
      "MariaDB service ABI version mismatch: " #name)

#endif
]=])

set(_MARIADB_LDS [=[
/* Generated file -- DO NOT EDIT. */
]=])

list(LENGTH MARIADB_SERVICES _service_count)

math(EXPR _service_last
    "${_service_count} - 1"
)

if(_service_count GREATER 0)
    foreach(_i RANGE 0 ${_service_last} 3)

        math(EXPR _version_index "${_i} + 1")
        math(EXPR _symbol_index  "${_i} + 2")

        list(GET MARIADB_SERVICES ${_i}            _name)
        list(GET MARIADB_SERVICES ${_version_index} _version)
        list(GET MARIADB_SERVICES ${_symbol_index}  _symbol)

        # Validate the version.
        if(NOT _version MATCHES "^0x[0-9A-Fa-f]+$")
            message(FATAL_ERROR
                "Invalid version '${_version}' for MariaDB service '${_name}'"
            )
        endif()

        # Convert 0xMMmm into major/minor.
        math(EXPR _major "${_version} >> 8")
        math(EXPR _minor "${_version} & 0xff")

        set(_elf_version
            "MARIADB_${_name}_${_major}.${_minor}"
        )

        # -------------------------------------------------------------------
        # service_versions.h
        # -------------------------------------------------------------------

        string(APPEND _MARIADB_HEADER
            "#define VERSION_${_name} ${_version}\n"
        )

        string(APPEND _MARIADB_HEADER
            "#define MARIADB_SYMVER_${_symbol} \"${_elf_version}\"\n"
        )

        string(APPEND _MARIADB_HEADER
            "\n"
        )

        # -------------------------------------------------------------------
        # linker script
        # -------------------------------------------------------------------

        string(APPEND _MARIADB_LDS
            "${_elf_version} {\n"
            "    global:\n"
            "        ${_symbol};\n"
            "};\n"
            "\n"
        )

    endforeach()
endif()


# ---------------------------------------------------------------------------
# Write generated files
# ---------------------------------------------------------------------------

file(GENERATE
    OUTPUT "${MARIADB_VERSION_HEADER}"
    CONTENT "${_MARIADB_HEADER}"
)

file(GENERATE
    OUTPUT "${MARIADB_VERSION_SCRIPT}"
    CONTENT "${_MARIADB_LDS}"
)


# ---------------------------------------------------------------------------
# Convenience target
# ---------------------------------------------------------------------------

add_custom_target(mariadb_service_versions
    DEPENDS
        "${MARIADB_VERSION_HEADER}"
        "${MARIADB_VERSION_SCRIPT}"
)


# ---------------------------------------------------------------------------
# Include generated headers from the build tree.
#
# This makes:
#
#     #include "service_versions.h"
#
# work for targets that use the normal MariaDB include paths.
# ---------------------------------------------------------------------------

include_directories(
    "${MARIADB_GENERATED_DIR}"
)

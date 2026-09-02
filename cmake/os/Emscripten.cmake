# Copyright (c) 2026 MariaDB
#
# Emscripten / WebAssembly target. Included because CMAKE_SYSTEM_NAME is
# "Emscripten" when configured with emcmake.

SET(SYSTEM_TYPE "Emscripten")
SET(_FILE_OFFSET_BITS 64)
SET(_GNU_SOURCE 1)

# wasm32: do not pick up host x86 CRC / WolfSSL Intel asm.
SET(CMAKE_SYSTEM_PROCESSOR "wasm32")

# emcmake injects a Node emulator. pthread-enabled WASM binaries are not
# usable as host tools (gen_lex_hash, comp_err, …); use native ones instead.
UNSET(CMAKE_CROSSCOMPILING_EMULATOR)
UNSET(CMAKE_CROSSCOMPILING_EMULATOR CACHE)

# Embedded server in one WASM module; no dlopen plugins, no shared libs.
SET(WITH_EMBEDDED_SERVER ON CACHE BOOL "Compile MariaDB with embedded server" FORCE)
SET(DISABLE_SHARED ON CACHE BOOL "Don't build shared libraries" FORCE)
SET(WITHOUT_DYNAMIC_PLUGINS 1)
SET(WITH_NONE ON)
SET(WITH_UNIT_TESTS OFF CACHE BOOL "Compile MySQL with unit tests" FORCE)
SET(WITH_WSREP OFF CACHE BOOL "WSREP replication API" FORCE)
SET(SECURITY_HARDENED OFF CACHE BOOL "Use security-enhancing compiler features" FORCE)
SET(ENABLED_PROFILING OFF CACHE BOOL "Enable profiling" FORCE)
SET(WITH_INNODB_PMEM OFF CACHE BOOL "Support memory-mapped InnoDB redo log" FORCE)
SET(WITH_NUMA OFF CACHE STRING "Build with non-uniform memory access" FORCE)
SET(WITH_URING OFF CACHE BOOL "Require that io_uring be used" FORCE)
SET(WITH_LIBAIO OFF CACHE BOOL "Require that libaio is used" FORCE)
SET(WITH_SYSTEMD "no" CACHE STRING "Enable systemd scripts and notification support" FORCE)
SET(WITH_SSL "bundled" CACHE STRING "SSL library" FORCE)
SET(WITH_ZLIB "bundled" CACHE STRING "Use bundled zlib" FORCE)
SET(WITH_PCRE "bundled" CACHE STRING "Which pcre to use" FORCE)
SET(WITH_LIBFMT "bundled" CACHE STRING "Which libfmt to use" FORCE)
SET(WITHOUT_ABI_CHECK ON)
SET(FEATURE_SUMMARY OFF CACHE BOOL "Print feature summary" FORCE)
SET(UPDATE_SUBMODULES OFF CACHE BOOL "Update submodules automatically" FORCE)
SET(ENABLE_DTRACE OFF CACHE BOOL "Enable dtrace" FORCE)

# Host-only storage engines / plugins (WITH_NONE already drops non-DEFAULT).
SET(PLUGIN_COLUMNSTORE "NO" CACHE STRING "How to build plugin COLUMNSTORE")
SET(PLUGIN_ROCKSDB "NO" CACHE STRING "How to build plugin ROCKSDB")
SET(PLUGIN_MROONGA "NO" CACHE STRING "How to build plugin MROONGA")
SET(PLUGIN_SPIDER "NO" CACHE STRING "How to build plugin SPIDER")
SET(PLUGIN_CONNECT "NO" CACHE STRING "How to build plugin CONNECT")
SET(PLUGIN_OQGRAPH "NO" CACHE STRING "How to build plugin OQGRAPH")
SET(PLUGIN_SPHINX "NO" CACHE STRING "How to build plugin SPHINX")
SET(PLUGIN_FEDERATED "NO" CACHE STRING "How to build plugin FEDERATED")
SET(PLUGIN_FEDERATEDX "NO" CACHE STRING "How to build plugin FEDERATEDX")
SET(PLUGIN_S3 "NO" CACHE STRING "How to build plugin S3")
SET(PLUGIN_DUCKDB "NO" CACHE STRING "How to build plugin DUCKDB")
SET(PLUGIN_VIDEX "NO" CACHE STRING "How to build plugin VIDEX")
SET(PLUGIN_PERFSCHEMA "NO" CACHE STRING "How to build plugin PERFSCHEMA")
SET(PLUGIN_AUTH_GSSAPI "NO" CACHE STRING "How to build plugin AUTH_GSSAPI")
SET(PLUGIN_AUTH_PAM "NO" CACHE STRING "How to build plugin AUTH_PAM")
SET(PLUGIN_CRACKLIB_PASSWORD_CHECK "NO" CACHE STRING "How to build plugin CRACKLIB_PASSWORD_CHECK")
SET(PLUGIN_HASHICORP "NO" CACHE STRING "How to build plugin HASHICORP")

# Simulated AIO (tpool non-Linux path) — no io_uring / libaio.
# Threads are required by InnoDB's tpool / page cleaner.
ADD_COMPILE_OPTIONS(-pthread -fexceptions -fno-strict-aliasing)
ADD_LINK_OPTIONS(-pthread -fexceptions)

# Do not pass -Wl,--no-undefined; Emscripten JS glue provides some symbols.
SET(LINK_FLAG_NO_UNDEFINED "")

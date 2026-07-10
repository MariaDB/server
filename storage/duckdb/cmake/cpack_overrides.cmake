# DuckDB plugin-specific CPack overrides applied at package time.
# Referenced via CPACK_PROJECT_CONFIG_FILE and included by CPack after
# CPackConfig.cmake, so these settings win over the main project's.

# Faster payload compression.
set(CPACK_RPM_COMPRESSION_TYPE "zstd")

# Disable debuginfo/debugsource for the DuckDB plugin package.
set(CPACK_RPM_DEBUGINFO_PACKAGE OFF)
set(CPACK_RPM_PACKAGE_DEBUG 0)
set(CPACK_STRIP_FILES OFF)

# Prevent rpmbuild itself from running brp-strip / find-debuginfo.
# CPACK_STRIP_FILES only affects CPack's own stripping.
if(DEFINED CPACK_RPM_SPEC_MORE_DEFINE)
    set(CPACK_RPM_SPEC_MORE_DEFINE "${CPACK_RPM_SPEC_MORE_DEFINE}\n%define __strip /bin/true\n%define __objdump /bin/true\n%define __os_install_post %nil\n%define __debug_install_post %nil")
else()
    set(CPACK_RPM_SPEC_MORE_DEFINE "%define __strip /bin/true\n%define __objdump /bin/true\n%define __os_install_post %nil\n%define __debug_install_post %nil")
endif()

# Disable debugsource mapping. Without this, CPackRPM requires the source
# directory path to be strictly longer than /usr/src/debug/<name>-<ver>,
# which fails for short source paths like /home/rocky/mdb-server.
unset(CPACK_RPM_BUILD_SOURCE_DIRS_PREFIX)

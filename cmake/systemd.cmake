INCLUDE(FindPkgConfig)
# http://www.cmake.org/cmake/help/v3.0/module/FindPkgConfig.html

MACRO(CHECK_SYSTEMD)
  IF(UNIX)
    SET(WITH_SYSTEMD "auto" CACHE STRING "Compile with systemd socket activation and notification")
    IF(WITH_SYSTEMD  STREQUAL "yes" OR WITH_SYSTEMD STREQUAL "auto")
      IF(PKG_CONFIG_FOUND)
        IF(WITH_SYSTEMD  STREQUAL "yes")
          pkg_check_modules(LIBSYSTEMD REQUIRED libsystemd)
        ELSE()
          pkg_check_modules(LIBSYSTEMD libsystemd)
        ENDIF()
        IF(HAVE_DLOPEN)
          SET(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} ${LIBSYSTEMD_LIBRARIES})
          SET(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} ${LIBSYSTEMD_CFLAGS}")
          SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LIBSYSTEMD_LDFLAGS}")
        ELSE()
          SET(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} ${LIBSYSTEMD_STATIC_LIBRARIES})
          SET(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} ${LIBSYSTEMD_STATIC_CFLAGS}")
          SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LIBSYSTEMD_STATIC_LDFLAGS}")
        ENDIF()
      ELSE()
        SET(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} systemd)
      ENDIF()
      CHECK_C_SOURCE_COMPILES(
      "
      #include <systemd/sd-daemon.h>
      int main()
      {
        sd_listen_fds(0);
      }"
      HAVE_SYSTEMD)
      CHECK_INCLUDE_FILES(systemd/sd-daemon.h HAVE_SYSTEMD_SD_DAEMON_H)
      CHECK_FUNCTION_EXISTS(sd_listen_fds HAVE_SYSTEMD_SD_LISTEN_FDS)
      CHECK_FUNCTION_EXISTS(sd_notify HAVE_SYSTEMD_SD_NOTIFY)
      CHECK_FUNCTION_EXISTS(sd_notifyf HAVE_SYSTEMD_SD_NOTIFYF)
      IF(HAVE_SYSTEMD AND HAVE_SYSTEMD_SD_DAEMON_H AND HAVE_SYSTEMD_SD_LISTEN_FDS
         AND HAVE_SYSTEMD_SD_NOTIFY AND HAVE_SYSTEMD_SD_NOTIFYF)
        SET(LIBSYSTEMD "systemd-daemon")
        MESSAGE(STATUS "Systemd features enabled")
      ELSE()
        MESSAGE(STATUS "Systemd features not enabled")
        MESSAGE(FATAL_ERROR "Requested WITH_SYSTEMD=YES however no dependencies installed/found")
      ENDIF()
    ENDIF()
  ENDIF()
ENDMACRO()

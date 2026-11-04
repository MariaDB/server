# Copyright (c) 2026, MariaDB Corporation.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA

# Support for moving the MariaDB configuration directory away from
# /etc/mysql by only setting DEFAULT_SYSCONFDIR.
#
# Files that are not configured with @MARIADB_CONFDIR@ and still refer
# to /etc/mysql can be copied with COPY_WITH_SYSCONFDIR, which replaces
# /etc/mysql with MARIADB_CONFDIR.
#
# The Debian maintainer scripts are updated in place the same way, except
# lines containing 'old_sysconfdir'. This is used by the maintainer
# scripts to refer to the old directory when moving files from an old
# installation. The other files in debian/ refer to the configuration
# directory used there directly.

FIND_PROGRAM(SED_EXECUTABLE sed)
MARK_AS_ADVANCED(SED_EXECUTABLE)

STRING(REGEX REPLACE "^/" "" confdir_rel "${MARIADB_CONFDIR}")
SET(sysconfdir_sed_script ${CMAKE_BINARY_DIR}/sysconfdir.sed)
# The substitution is done twice to also handle adjacent paths
STRING(CONCAT sysconfdir_sed_cmd
  "s#(^|[^[:alnum:]_./-])(/?)etc/mysql([/\"'[:space:])]|$)#"
  "\\1\\2${confdir_rel}\\3#g")
# @DEB_SYSCONFDIR_MOVE_VERSION@ is replaced on all lines, so the
# maintainer scripts know which versions used the old directory
FILE(WRITE ${sysconfdir_sed_script}
"s#@DEB_SYSCONFDIR_MOVE_VERSION@#${DEB_SYSCONFDIR_MOVE_VERSION}#g
/old_sysconfdir/b
${sysconfdir_sed_cmd}
${sysconfdir_sed_cmd}
")


# Copy src to dst, with /etc/mysql replaced with MARIADB_CONFDIR

FUNCTION(COPY_WITH_SYSCONFDIR src dst)
  IF(MARIADB_CONFDIR STREQUAL "/etc/mysql")
    CONFIGURE_FILE(${src} ${dst} COPYONLY)
    RETURN()
  ENDIF()
  IF(NOT SED_EXECUTABLE)
    MESSAGE(FATAL_ERROR "sed is needed when DEFAULT_SYSCONFDIR is set")
  ENDIF()
  EXECUTE_PROCESS(COMMAND ${SED_EXECUTABLE} -E -f ${sysconfdir_sed_script}
                  ${src}
                  OUTPUT_FILE ${dst} RESULT_VARIABLE res)
  IF(res)
    MESSAGE(FATAL_ERROR "Could not create ${dst} from ${src}")
  ENDIF()
  # Rerun cmake if src changes
  SET_PROPERTY(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${src})
ENDFUNCTION()


# Update the Debian maintainer scripts in place, like debian/rules
# already does for __MARIADB_MAJOR_VER__. This is idempotent as no
# /etc/mysql references or @DEB_SYSCONFDIR_MOVE_VERSION@ are left
# after the first run, except on the old_sysconfdir lines.

FUNCTION(UPDATE_DEBIAN_SYSCONFDIR)
  SET(debian_dir ${CMAKE_SOURCE_DIR}/debian)
  IF(MARIADB_CONFDIR STREQUAL "/etc/mysql")
    RETURN()
  ENDIF()
  IF(NOT SED_EXECUTABLE)
    MESSAGE(FATAL_ERROR "sed is needed when DEFAULT_SYSCONFDIR is set")
  ENDIF()
  IF(NOT DEB_SYSCONFDIR_MOVE_VERSION)
    MESSAGE(FATAL_ERROR "DEB_SYSCONFDIR_MOVE_VERSION must be set to the "
            "first package version using ${MARIADB_CONFDIR}")
  ENDIF()
  FILE(GLOB files
       ${debian_dir}/*.preinst ${debian_dir}/*.postinst
       ${debian_dir}/*.prerm ${debian_dir}/*.postrm)
  IF(files)
    EXECUTE_PROCESS(COMMAND ${SED_EXECUTABLE} -E -i
                    -f ${sysconfdir_sed_script} ${files}
                    RESULT_VARIABLE res)
    IF(res)
      MESSAGE(FATAL_ERROR "Could not update files in ${debian_dir}")
    ENDIF()
  ENDIF()
ENDFUNCTION()


IF(DEB)
  UPDATE_DEBIAN_SYSCONFDIR()
ENDIF()

# Copyright (C) 2013-2016  Kouhei Sutou <kou@clear-code.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

AC_PREREQ(2.59)
m4_define([groonga_normalizer_mysql_version], m4_include(version))
AC_INIT([groonga-normalizer-mysql],
        groonga_normalizer_mysql_version,
        [groonga-talk@lists.sourceforge.net])

AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([normalizers/mysql.c])
AM_CONFIG_HEADER([config.h])

AM_INIT_AUTOMAKE([1.9 foreign tar-pax])

AC_PROG_LIBTOOL

AC_MSG_CHECKING([for clang])
if test "$CC" = "clang"; then
  CLANG=yes
else
  CLANG=no
fi
AC_MSG_RESULT([$CLANG])

if test "$GCC" = "yes"; then
  CFLAGS="$CFLAGS -Wall -Wextra"
  CFLAGS="$CFLAGS -Wmissing-declarations -Wmissing-prototypes"
fi

# For debug
AC_ARG_ENABLE(debug,
  [AS_HELP_STRING([--enable-debug],
                  [use debug flags (default=no)])],
  [groonga_normalizer_mysql_debug="$enableval"],
  [groonga_normalizer_mysql_debug="no"])
if test "x$groonga_normalizer_mysql_debug" != "xno"; then
  if test "$CLANG" = "yes"; then
    CFLAGS="$CFLAGS -O0 -g"
    CXXFLAGS="$CXXFLAGS -O0 -g"
  elif test "$GCC" = "yes"; then
    CFLAGS="$CFLAGS -O0 -g3"
    CXXFLAGS="$CXXFLAGS -O0 -g3"
  fi
fi

m4_define([groonga_required_version], m4_include(required_groonga_version))
GROONGA_REQUIRED_VERSION=groonga_required_version
PKG_CHECK_MODULES([GROONGA], [groonga >= ${GROONGA_REQUIRED_VERSION}])

_PKG_CONFIG(GROONGA_PLUGINS_DIR, [variable=pluginsdir],    [groonga])
_PKG_CONFIG(GROONGA,             [variable=groonga],       [groonga])

GROONGA_PLUGINS_DIR="${pkg_cv_GROONGA_PLUGINS_DIR}"
GROONGA="${pkg_cv_GROONGA}"

AC_SUBST(GROONGA_PLUGINS_DIR)
AC_SUBST(GROONGA)

normalizers_pluginsdir="\${GROONGA_PLUGINS_DIR}/normalizers"
AC_SUBST(normalizers_pluginsdir)

AC_ARG_ENABLE([embed],
  AS_HELP_STRING([--enable-embed],
    [Build as a static library to embed into an application (default: no)]),
  [GROONGA_NORMALIZER_MYSQL_EMBED="$enableval"],
  [GROONGA_NORMALIZER_MYSQL_EMBED="no"])

if test "x$GROONGA_NORMALIZER_MYSQL_EMBED" = "xyes"; then
  AC_DEFINE_UNQUOTED(GROONGA_NORMALIZER_MYSQL_EMBED,
                     [1],
                     [Define to 1 if groonga-normalizer-mysql is built for embedding.])
fi

# check Ruby for test
ac_cv_ruby_available="no"
AC_ARG_WITH([ruby],
            AS_HELP_STRING([--with-ruby=PATH],
                           [Ruby interpreter path (default: auto)]),
            [RUBY="$withval"],
            [RUBY="auto"])

if test "x$RUBY" = "xno"; then
  RUBY=
else
  if test "x$RUBY" = "xauto"; then
    AC_PATH_PROGS(RUBY,
                  [ dnl
                    ruby2.3 ruby23 dnl
                    ruby2.2 ruby22 dnl
                    ruby2.1 ruby21 dnl
                    ruby dnl
                  ],
                  ruby-not-found)
    if test "$RUBY" != "ruby-not-found"; then
      ruby_version="`$RUBY --version`"
      if echo "$ruby_version" | grep -q -- 'ruby \(1\.9\|2\.\)'; then
        ac_cv_ruby_available="yes"
      else
        AC_MSG_WARN([$RUBY isn't Ruby 1.9 or later ($ruby_version)])
      fi
    fi
  else
    ruby_not_found_warning_message="$RUBY is not found. Disable test."
    case "$RUBY" in
    /*)
      AC_CHECK_FILE([$RUBY],
                    [ac_cv_ruby_available="yes"],
                    [AC_MSG_WARN([$ruby_not_found_warning_message])
                     RUBY="$RUBY-not-found"])
      ;;
    *)
      ruby_not_found="$RUBY-not-found"
      AC_PATH_PROGS(RUBY, "$RUBY", "$ruby_not_found")
      if test "$RUBY" = "$ruby_not_found"; then
        AC_MSG_WARN([$ruby_not_found_warning_message])
      else
        ac_cv_ruby_available="yes"
      fi
      ;;
    esac
  fi
fi
AC_SUBST(RUBY)
AM_CONDITIONAL([WITH_TEST], [test "$ac_cv_ruby_available" = "yes"])

# For package
AC_ARG_WITH(rsync-path,
            [AS_HELP_STRING([--with-rsync-path=PATH],
            [specify rsync path to upload Groonga packages.])],
            [RSYNC_PATH="$withval"],
            [RSYNC_PATH="packages@packages.groonga.org:public"])
AC_SUBST(RSYNC_PATH)

AC_ARG_WITH(launchpad-uploader-pgp-key,
            [AS_HELP_STRING([--with-launchpad-uploader-pgp-key=KEY],
            [specify PGP key UID to upload Groonga packages to Launchpad.])],
            [LAUNCHPAD_UPLOADER_PGP_KEY="$withval"],
            [LAUNCHPAD_UPLOADER_PGP_KEY=""])
AC_SUBST(LAUNCHPAD_UPLOADER_PGP_KEY)

AC_ARG_WITH([groonga-source-path],
            AS_HELP_STRING([--with-groonga-source-path=PATH],
                           [Specify Groonga source path for
			    groonga-normalizer-mysql's release manager.]),
			   [GROONGA_SOURCE_PATH="$withval"])
case "$GROONGA_SOURCE_PATH" in
  ""|/*)
    : # do nothing
    ;;
  *)
    GROONGA_SOURCE_PATH="\$(top_builddir)/${GROONGA_SOURCE_PATH}"
    ;;
esac
AC_SUBST(GROONGA_SOURCE_PATH)

AC_ARG_WITH([cutter-source-path],
            AS_HELP_STRING([--with-cutter-source-path=PATH],
                           [Specify Cutter source path for
			    groonga-normalizer-mysql's release manager.]),
			   [CUTTER_SOURCE_PATH="$withval"])
case "$CUTTER_SOURCE_PATH" in
  ""|/*)
    : # do nothing
    ;;
  *)
    CUTTER_SOURCE_PATH="\$(top_builddir)/${CUTTER_SOURCE_PATH}"
    ;;
esac
AC_SUBST(CUTTER_SOURCE_PATH)

GPG_UID=m4_include(gpg_uid)
AC_SUBST(GPG_UID)

AC_CONFIG_FILES([
  Makefile
  groonga-normalizer-mysql.pc
  build/Makefile
  build/cmake_modules/Makefile
  normalizers/Makefile
  packages/Makefile
  packages/apt/Makefile
  packages/apt/env.sh
  packages/ubuntu/Makefile
  packages/rpm/Makefile
  packages/rpm/centos/Makefile
  packages/rpm/centos/groonga-normalizer-mysql.spec
  packages/rpm/fedora/Makefile
  packages/rpm/fedora/groonga-normalizer-mysql.spec
  packages/yum/Makefile
  packages/yum/env.sh
  packages/source/Makefile
  test/Makefile
  doc/Makefile
  doc/text/Makefile
])

AC_OUTPUT

echo "$PACKAGE_NAME $PACKAGE_VERSION configuration:"
echo "-----------------------"
echo "  Compiler:              ${CC}"
echo "  CFLAGS:                ${CFLAGS}"
echo "  CXXFLAGS:              ${CXXFLAGS}"
echo "  Libraries:             ${LIBS}"
echo
echo "Groonga"
echo "  CFLAGS:                ${GROONGA_CFLAGS}"
echo "  Libraries:             ${GROONGA_LIBS}"
echo "  install directory:     ${normalizers_pluginsdir}"

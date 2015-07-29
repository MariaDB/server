#!/bin/bash

# Build MariaDB .deb packages.
# Based on OurDelta .deb packaging scripts, which are in turn based on Debian
# MySQL packages.

# Exit immediately on any error
set -e

# Debug script and command lines
#set -x

# Don't run the mysql-test-run test suite as part of build.
# It takes a lot of time, and we will do a better test anyway in
# Buildbot, running the test suite from installed .debs on a clean VM.
export DEB_BUILD_OPTIONS="nocheck"

export MARIADB_OPTIONAL_DEBS=""

# Find major.minor version.
#
source ./VERSION
UPSTREAM="${MYSQL_VERSION_MAJOR}.${MYSQL_VERSION_MINOR}.${MYSQL_VERSION_PATCH}${MYSQL_VERSION_EXTRA}"
RELEASE_EXTRA=""

RELEASE_NAME=""
PATCHLEVEL="+maria"
LOGSTRING="MariaDB build"

# Look up distro-version specific stuff.
#
# Libreadline changed to GPLv3. Old GPLv2 version is available, but it
# is called different things on different versions.
CODENAME="$(lsb_release -sc)"
case "${CODENAME}" in
  lucid)  LIBREADLINE_DEV='libreadline5-dev | libreadline-dev' ;;
  squeeze)  LIBREADLINE_DEV=libreadline5-dev ;;
  *)  LIBREADLINE_DEV=libreadline-gplv2-dev ;;
esac

# add libcrack2 (>= 2.9.0) as a build dependency
# but only where the distribution can possibly satisfy it
if apt-cache madison cracklib2|grep 'cracklib2 *| *2\.[0-8]\.' >/dev/null 2>&1
then
  MAYBE_LIBCRACK=''
  MARIADB_OPTIONAL_DEBS="${MARIADB_OPTIONAL_DEBS} cracklib-password-check-10.1"
else
  MAYBE_LIBCRACK='libcrack2-dev (>= 2.9.0),'
fi

# Clean up build file symlinks that are distro-specific. First remove all, then set
# new links.
DISTRODIRS="$(ls ./debian/dist)"
for distrodir in ${DISTRODIRS}; do
  DISTROFILES="$(ls ./debian/dist/${distrodir})"
  for distrofile in ${DISTROFILES}; do
    rm -f "./debian/${distrofile}";
  done;
done;

# Set no symlinks for build files in the debian dir, so we avoid adding AppArmor on Debian.
DISTRO="$(lsb_release -si)"
echo "Copying distribution specific build files for ${DISTRO}"
DISTROFILES="$(ls ./debian/dist/${DISTRO})"
for distrofile in ${DISTROFILES}; do
  rm -f "./debian/${distrofile}"
  sed -e "s/\\\${LIBREADLINE_DEV}/${LIBREADLINE_DEV}/g" \
      -e "s/\\\${MAYBE_LIBCRACK}/${MAYBE_LIBCRACK}/g"             \
    < "./debian/dist/${DISTRO}/${distrofile}" > "./debian/${distrofile}"
  chmod --reference="./debian/dist/${DISTRO}/${distrofile}" "./debian/${distrofile}"
done;

# Adjust changelog, add new version.
#
echo "Incrementing changelog and starting build scripts"

dch -b -D ${CODENAME} -v "${UPSTREAM}${PATCHLEVEL}-${RELEASE_NAME}${RELEASE_EXTRA:+-${RELEASE_EXTRA}}1~${CODENAME}" "Automatic build with ${LOGSTRING}."

echo "Creating package version ${UPSTREAM}${PATCHLEVEL}-${RELEASE_NAME}${RELEASE_EXTRA:+-${RELEASE_EXTRA}}1~${CODENAME} ... "

# Build the package.
#
fakeroot dpkg-buildpackage -us -uc

[ -e debian/autorm-file ] && rm -vf `cat debian/autorm-file`

echo "Build complete"

# end of autobake script

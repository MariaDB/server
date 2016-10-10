#!/bin/bash

# Build MariaDB .deb packages.
# Based on OurDelta .deb packaging scripts, which are in turn based on Debian
# MySQL packages.

# Exit immediately on any error
set -e

# Debug script and command lines
#set -x

# On Buildbot, don't run the mysql-test-run test suite as part of build.
# It takes a lot of time, and we will do a better test anyway in
# Buildbot, running the test suite from installed .debs on a clean VM.
# On Travis-CI we want to simulate the full build, including tests.
# Also on Travis-CI it is useful not to override the DEB_BUILD_OPTIONS
# at this stage at all.
if [[ ! $TRAVIS ]]
then
  export DEB_BUILD_OPTIONS="nocheck"
fi

export MARIADB_OPTIONAL_DEBS=""

# Find major.minor version.
#
source ./VERSION
UPSTREAM="${MYSQL_VERSION_MAJOR}.${MYSQL_VERSION_MINOR}.${MYSQL_VERSION_PATCH}${MYSQL_VERSION_EXTRA}"
PATCHLEVEL="+maria"
LOGSTRING="MariaDB build"

# Look up distro-version specific stuff.

CODENAME="$(lsb_release -sc)"

# Add libcrack2 (>= 2.9.0) as a build dependency if available in the distribution
# This matches Debian Jessie, Stretch and Ubuntu Trusty, Wily, Xenial, Yakkety
# Update check when version 2.10 or newer is available.
if apt-cache madison libcrack2-dev | grep 'libcrack2-dev *| *2\.9' >/dev/null 2>&1
then
  sed 's/Standards-Version/,libcrack2-dev (>= 2.9.0)\nStandards-Version/' debian/control
  cat <<EOT >> debian/control

Package: mariadb-cracklib-password-check-10.2
Architecture: any
Depends: libcrack2 (>= 2.9.0),
         mariadb-server-10.2,
         \${misc:Depends},
         \${shlibs:Depends}
Description: CrackLib Password Validation Plugin for MariaDB
 This password validation plugin uses cracklib to allow only
 sufficiently secure (as defined by cracklib) user passwords in MariaDB.
EOT
fi

# Add libpcre3-dev (>= 2:8.35-3.2~) as a build dependency if available in the distribution
# This matches Debian Jessie, Stretch and Ubuntu Wily, Xenial, Yakkety
# Update check when version 2:8.40 or newer is available.
if apt-cache madison libpcre3-dev | grep 'libpcre3-dev *| *2:8\.3[2-9]' >/dev/null 2>&1
then
  sed 's/Standards-Version/,libpcre3-dev (>= 2:8.35-3.2~)\nStandards-Version/' debian/control
fi

# On Travis-CI, the log must stay under 4MB so make the build less verbose
if [[ $TRAVIS ]]
then
  sed -i -e '/Add support for verbose builds/,+2d' debian/rules
fi


# Adjust changelog, add new version.
#
echo "Incrementing changelog and starting build scripts"

dch -b -D ${CODENAME} -v "${UPSTREAM}${PATCHLEVEL}~${CODENAME}" "Automatic build with ${LOGSTRING}."

echo "Creating package version ${UPSTREAM}${PATCHLEVEL}~${CODENAME} ... "

# Build the package.
# Pass -I so that .git and other unnecessary temporary and source control files
# will be ignored by dpkg-source when createing the tar.gz source package
fakeroot dpkg-buildpackage -us -uc -I

[ -e debian/autorm-file ] && rm -vf `cat debian/autorm-file`

echo "Build complete"

# end of autobake script

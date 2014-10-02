#!/bin/sh

warn() {
	echo "  WARNING: $@" 1>&2
}

# init

LIBTOOLIZE=libtoolize
ACLOCAL=aclocal
AUTOCONF=autoconf
AUTOHEADER=autoheader
AUTOMAKE=automake

case `uname -s` in
Darwin)
	LIBTOOLIZE=glibtoolize
	;;
FreeBSD)
	ACLOCAL_ARGS="$ACLOCAL_ARGS -I /usr/local/share/aclocal/"
	;;
esac


# libtoolize
echo "Searching libtoolize..."
if [ `which $LIBTOOLIZE` ] ; then
  echo "  FOUND: libtoolize -> $LIBTOOLIZE"
else
  warn "Cannot Found libtoolize... input libtool command"
  read LIBTOOLIZE
  LIBTOOLIZE=`which $LIBTOOLIZE`
  if [ `which $LIBTOOLIZE` ] ; then
    echo "  SET: libtoolize -> $LIBTOOLIZE"
  else
    warn "$LIBTOOLIZE: Command not found."
    exit 1;
  fi
fi

# aclocal
echo "Searching aclocal..."
if [ `which $ACLOCAL` ] ; then
  echo "  FOUND: aclocal -> $ACLOCAL"
else
  warn "Cannot Found aclocal... input aclocal command"
  read ACLOCAL
  ACLOCAL=`which $ACLOCAL`
  if [ `which $ACLOCAL` ] ; then
    echo "  SET: aclocal -> $ACLOCAL"
  else
    warn "$ACLOCAL: Command not found."
    exit 1;
  fi
fi

# automake
echo "Searching automake..."
if [ `which $AUTOMAKE` ] ; then
  echo "  FOUND: automake -> $AUTOMAKE"
else
  warn "Cannot Found automake... input automake command"
  read AUTOMAKE
  ACLOCAL=`which $AUTOMAKE`
  if [ `which $AUTOMAKE` ] ; then
    echo "  SET: automake -> $AUTOMAKE"
  else
    warn "$AUTOMAKE: Command not found."
    exit 1;
  fi
fi

# autoheader
echo "Searching autoheader..."
if [ `which $AUTOHEADER` ] ; then
  echo "  FOUND: autoheader -> $AUTOHEADER"
else
  warn "Cannot Found autoheader... input autoheader command"
  read AUTOHEADER
  ACLOCAL=`which $AUTOHEADER`
  if [ `which $AUTOHEADER` ] ; then
    echo "  SET: autoheader -> $AUTOHEADER"
  else
    warn "$AUTOHEADER: Command not found."
    exit 1;
  fi
fi

# autoconf
echo "Searching autoconf..."
if [ `which $AUTOCONF` ] ; then
  echo "  FOUND: autoconf -> $AUTOCONF"
else
  warn "Cannot Found autoconf... input autoconf command"
  read AUTOCONF
  ACLOCAL=`which $AUTOCONF`
  if [ `which $AUTOCONF` ] ; then
    echo "  SET: autoconf -> $AUTOCONF"
  else
    warn "$AUTOCONF: Command not found."
    exit 1;
  fi
fi

set -e

echo "Running libtoolize ..."
$LIBTOOLIZE --force --copy
echo "Running aclocal ..."
$ACLOCAL ${ACLOCAL_ARGS}
echo "Running autoheader..."
$AUTOHEADER
echo "Running automake ..."
$AUTOMAKE --add-missing --copy
echo "Running autoconf ..."
$AUTOCONF

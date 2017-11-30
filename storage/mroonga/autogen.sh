#!/bin/sh

case $(uname -s) in
FreeBSD)
  ACLOCAL_ARGS="$ACLOCAL_ARGS -I /usr/local/share/aclocal/"
  ;;
esac

mkdir -p m4

${AUTORECONF:-autoreconf} --force --install "$@"

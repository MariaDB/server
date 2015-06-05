#!/bin/sh

./version-gen.sh

case `uname -s` in
Darwin)
        homebrew_aclocal=/usr/local/share/aclocal
        if [ -d $homebrew_aclocal ]; then
          ACLOCAL_ARGS="$ACLOCAL_ARGS -I $homebrew_aclocal"
        fi
        gettext_aclocal="$(echo /usr/local/Cellar/gettext/*/share/aclocal)"
        if [ -d $gettext_aclocal ]; then
          ACLOCAL_ARGS="$ACLOCAL_ARGS -I $gettext_aclocal"
        fi
	;;
FreeBSD)
	ACLOCAL_ARGS="$ACLOCAL_ARGS -I /usr/local/share/aclocal/"
	;;
esac

if [ ! -e vendor/mruby-source/.git ]; then
  rm -rf vendor/mruby-source
fi
git submodule update --init

${AUTORECONF:-autoreconf} --force --install

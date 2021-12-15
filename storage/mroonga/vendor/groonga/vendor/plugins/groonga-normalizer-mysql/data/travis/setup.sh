#!/bin/sh
#
# Copyright (C) 2013  Kouhei Sutou <kou@clear-code.com>
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

set -e

if [ "$GROONGA_NORMALIZER_MYSQL_MASTER" = "yes" ]; then
    if ! pkg-config --exists groonga; then
      sudo apt-get install -qq -y -V libgroonga-dev
    fi
    git clone --depth 1 https://github.com/groonga/groonga-normalizer-mysql.git
    cd groonga-normalizer-mysql
    ./autogen.sh
    ./configure CFLAGS="-O0 -g3" CXXFLAGS="-O0 -g3" --prefix=/usr
    make -j$(grep '^processor' /proc/cpuinfo | wc -l) > /dev/null
    sudo make install > /dev/null
    cd ..
else
    sudo apt-get install -qq -y -V groonga-normalizer-mysql
fi

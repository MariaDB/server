#!/bin/sh
# Copyright (c) 2000, 2014, Oracle and/or its affiliates. All rights reserved.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# This script reports various configuration settings that may be needed
# when using the MariaDB client library.

which ()
{
  IFS="${IFS=   }"; save_ifs="$IFS"; IFS=':'
  for file
  do
    for dir in $PATH
    do
      if test -f $dir/$file
      then
        echo "$dir/$file"
        continue 2
      fi
    done
    echo "which: no $file in ($PATH)"
    exit 1
  done
  IFS="$save_ifs"
}

#
# If we can find the given directory relatively to where mysql_config is
# we should use this instead of the incompiled one.
# This is to ensure that this script also works with the binary MariaDB
# version

fix_path ()
{
  var=$1
  shift
  for filename
  do
    path=$basedir/$filename
    if [ -d "$path" ] ;
    then
      eval "$var"=$path
      return
    fi
  done
}

get_full_path ()
{
  file=$1

  # if the file is a symlink, try to resolve it
  if [ -h $file ];
  then
    file=`ls -l $file | awk '{ print $NF }'`
  fi

  case $file in
    /*) echo "$file";;
    */*) tmp=`pwd`/$file; echo $tmp | sed -e 's;/\./;/;' ;;
    *) which $file ;;
  esac
}

me=`get_full_path $0`

# Script might have been renamed but assume mysql_<something>config<something>
basedir=`echo $me | sed -e 's;/bin/mysql_.*config.*;;'`

ldata='@localstatedir@'
execdir='@libexecdir@'
bindir='@bindir@'

# If installed, search for the compiled in directory first (might be "lib64")
pkglibdir='@pkglibdir@'
pkglibdir_rel=`echo $pkglibdir | sed -e "s;^$basedir/;;"`
fix_path pkglibdir $pkglibdir_rel @libsubdir@/mysql @libsubdir@

plugindir='@pkgplugindir@'
plugindir_rel=`echo $plugindir | sed -e "s;^$basedir/;;"`
fix_path plugindir $plugindir_rel @libsubdir@/mysql/plugin @libsubdir@/plugin

pkgincludedir='@pkgincludedir@'
fix_path pkgincludedir include/mysql

version='@VERSION@'
socket='@MYSQL_UNIX_ADDR@'

if [ @MYSQL_TCP_PORT_DEFAULT@ -eq 0 ]; then
  port=0
else
  port=@MYSQL_TCP_PORT@
fi

# Create options 
libs="-L$pkglibdir @RPATH_OPTION@ @LIBS_FOR_CLIENTS@"
embedded_libs="-L$pkglibdir @RPATH_OPTION@ @EMB_LIBS_FOR_CLIENTS@"

include="-I$pkgincludedir"
if [ "$basedir" != "/usr" ]; then
  include="$include -I$pkgincludedir/.."
fi
cflags="$include @CFLAGS_FOR_CLIENTS@"

usage () {
        cat <<EOF
Usage: $0 [OPTIONS]
Options:
        --cflags         [$cflags]
        --include        [$include]
        --libs           [$libs]
        --libs_r         [$libs]
        --plugindir      [$plugindir]
        --socket         [$socket]
        --port           [$port]
        --version        [$version]
        --libmysqld-libs [$embedded_libs]
        --variable=VAR   VAR is one of:
                pkgincludedir [$pkgincludedir]
                pkglibdir     [$pkglibdir]
                plugindir     [$plugindir]
EOF
  exit $1
}

if test $# -le 0; then usage 0 ; fi

while test $# -gt 0; do
        case $1 in
        --cflags)  echo "$cflags" ;;
        --include) echo "$include" ;;
        --libs)    echo "$libs" ;;
        --libs_r)  echo "$libs" ;;
        --plugindir) echo "$plugindir" ;;
        --socket)  echo "$socket" ;;
        --port)    echo "$port" ;;
        --version) echo "$version" ;;
        --embedded-libs | --embedded | --libmysqld-libs) echo "$embedded_libs" ;;
        --variable=*)
          var=`echo "$1" | sed 's,^[^=]*=,,'`
          case "$var" in
            pkgincludedir) echo "$pkgincludedir" ;;
            pkglibdir) echo "$pkglibdir" ;;
            plugindir) echo "$plugindir" ;;
            *) usage 1 >&2 ;;
          esac
          ;;
        *) usage 1 >&2 ;;
        esac

        shift
done

#echo "ldata: '"$ldata"'"
#echo "execdir: '"$execdir"'"
#echo "bindir: '"$bindir"'"
#echo "pkglibdir: '"$pkglibdir"'"
#echo "pkgincludedir: '"$pkgincludedir"'"
#echo "version: '"$version"'"
#echo "socket: '"$socket"'"
#echo "port: '"$port"'"
#echo "ldflags: '"$ldflags"'"
#echo "client_libs: '"$client_libs"'"

exit 0

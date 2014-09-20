#!/bin/sh

if [ $# != 4 ]; then
    echo "Usage: $0 PACKAGE SPEC_DIR MYSQL_VARIANTS ARCHITECTURES"
    echo " e.g.: $0 mroonga ../rpm/centos 'mysql55 mariadb' 'i386 x86_64'"
    exit 1
fi

PACKAGE="$1"
SPEC_DIR="$2"
MYSQL_VARIANTS="$3"
ARCHITECTURES="$4"

run()
{
  "$@"
  if test $? -ne 0; then
    echo "Failed $@"
    exit 1
  fi
}

run vagrant destroy --force

for mysql_variant in ${MYSQL_VARIANTS}; do
  rm -rf tmp/centos/
  mkdir -p tmp/centos/
  cp ${SPEC_DIR}/${mysql_variant}-${PACKAGE}.spec tmp/centos/

  architectures="${ARCHITECTURES}"
  case ${mysql_variant} in
    mysql55)
      centos_versions="5 6"
      ;;
    mysql56-community)
      centos_versions="6 7"
      ;;
    mariadb)
      centos_versions="7"
      ;;
  esac

  for architecture in ${architectures}; do
    for centos_version in ${centos_versions}; do
      if [ ${mysql_variant} = mysql55 -a ${centos_version} = 6 -a ${architecture} = i386 ]; then
        continue
      fi
      if [ ${centos_version} = 7 -a ${architecture} = i386 ]; then
        continue
      fi
      id=centos-${centos_version}-${architecture}
      vagrant up ${id}
      build_status=$?
      if [ $build_status -ne 0 ]; then
        exit $build_status
      fi
      vagrant destroy --force ${id}
    done
  done
done

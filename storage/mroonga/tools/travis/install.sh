#!/bin/sh
#
# Copyright(C) 2012-2017 Kouhei Sutou <kou@clear-code.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

set -x
set -e

# export GROONGA_MASTER=yes
# export GROONGA_NORMALIZER_MYSQL_MASTER=yes

#mariadb_download_base=http://mirror.jmu.edu/pub/mariadb
mariadb_download_base=http://ftp.osuosl.org/pub/mariadb

version=$(echo "$MYSQL_VERSION" | sed -r -e 's/^(mysql|mariadb|percona-server)-//')
series=$(echo "$version" | sed -r -e 's/^([0-9]+\.[0-9]+).*$/\1/g')

setup_mariadb_apt()
{
  distribution=$(lsb_release --short --id | tr 'A-Z' 'a-z')
  code_name=$(lsb_release --short --codename)
  component=main
  apt_url_base="${mariadb_download_base}/repo/${series}"
  cat <<EOF | sudo tee /etc/apt/sources.list.d/mariadb.list
deb ${apt_url_base}/${distribution}/ ${code_name} ${component}
deb-src ${apt_url_base}/${distribution}/ ${code_name} ${component}
EOF
  sudo apt-key adv --recv-keys --keyserver keyserver.ubuntu.com 0xcbcb082a1bb943db
  sudo apt-get -qq update
}

setup_percona_apt()
{
  code_name=$(lsb_release --short --codename)
  release_deb_version=0.1-4
  release_deb=percona-release_${release_deb_version}.${code_name}_all.deb
  wget http://www.percona.com/downloads/percona-release/ubuntu/${release_deb_version}/${release_deb}
  sudo dpkg -i ${release_deb}
  sudo apt-get -qq update
}

if [ "${MROONGA_BUNDLED}" = "yes" ]; then
  mkdir -p .mroonga
  mv * .mroonga/
  mv .mroonga/tools ./
  setup_mariadb_apt
  sudo apt-get -qq -y build-dep mariadb-server
  # Support MariaDB for now.
  download_base=${mariadb_download_base}/${MYSQL_VERSION}
  tar_gz=${MYSQL_VERSION}.tar.gz
  curl -O ${download_base}/source/${tar_gz}
  tar xzf $tar_gz
  mv ${MYSQL_VERSION}/* ./
  rm -rf storage/mroonga
  mv .mroonga storage/mroonga
  rm -rf ${MYSQL_VERSION}
  git clone --recursive --depth 1 \
      https://github.com/groonga/groonga.git \
      storage/mroonga/vendor/groonga
  git clone --recursive --depth 1 \
      https://github.com/groonga/groonga-normalizer-mysql.git \
      storage/mroonga/vendor/groonga/vendor/plugins/groonga-normalizer-mysql
else
  curl --silent --location \
       https://raw.githubusercontent.com/groonga/groonga/master/data/travis/setup.sh | sh
  curl --silent --location \
       https://raw.githubusercontent.com/groonga/groonga-normalizer-mysql/master/data/travis/setup.sh | sh
  # curl --silent --location \
  #      https://raw.githubusercontent.com/clear-code/cutter/master/data/travis/setup.sh | sh

  if [ ! -f /usr/lib/groonga/plugins/tokenizers/mecab.so ]; then
    sudo apt-get -qq -y install groonga-tokenizer-mecab
  fi

  mkdir -p vendor
  cd vendor

  case "$MYSQL_VERSION" in
    mysql-*)
      sudo apt-get -qq update
      sudo apt-get -qq -y build-dep mysql-server
      if [ "$version" = "system" ]; then
        sudo apt-get -y remove --purge \
             mysql-server-5.6 \
             mysql-server-core-5.6 \
             mysql-client-5.6 \
             mysql-client-core-5.6
        sudo rm -rf /var/lib/mysql
        sudo apt-get -y install \
             mysql-server \
             mysql-client \
             mysql-testsuite \
             libmysqld-dev
        apt-get source mysql-server
        ln -s $(find . -maxdepth 1 -type d | sort | tail -1) mysql
      else
        repository_deb=mysql-apt-config_0.8.3-1_all.deb
        curl -O http://repo.mysql.com/${repository_deb}
        sudo env MYSQL_SERVER_VERSION=mysql-${series} \
             dpkg -i ${repository_deb}
        sudo apt-get -qq update
        sudo apt-get -qq -y remove --purge mysql-common
        sudo apt-get -qq -y build-dep mysql-server
        sudo apt-get -qq -y install \
             mysql-server \
             libmysqlclient-dev \
             libmysqld-dev \
             mysql-testsuite
        apt-get -qq source mysql-server
        ln -s $(find . -maxdepth 1 -type d | sort | tail -1) mysql
      fi
      ;;
    mariadb-*)
      sudo apt-get -y remove --purge \
           mysql-server-5.6 \
           mysql-server-core-5.6 \
           mysql-client-5.6 \
           mysql-client-core-5.6 \
           mysql-common
      sudo rm -rf /var/lib/mysql
      setup_mariadb_apt
      sudo apt-get -qq -y build-dep mariadb-server
      sudo apt-get -y install \
           mariadb-server \
           mariadb-client \
           mariadb-test \
           libmariadbclient-dev
      apt-get source mariadb-server
      ln -s $(find . -maxdepth 1 -type d | sort | tail -1) mysql
      ;;
    percona-server-*)
      setup_percona_apt
      sudo apt-get -qq -y build-dep percona-server-server-${series}
      sudo apt-get -qq -y install \
           percona-server-server-${series} \
           percona-server-client-${series} \
           percona-server-test-${series}
      apt-get -qq source percona-server-server-${series}
      ln -s $(find . -maxdepth 1 -type d | sort | tail -1) mysql
      ;;
  esac

  cd ..
fi

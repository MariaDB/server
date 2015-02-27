#!/bin/sh
#
# Copyright(C) 2012-2013 Kouhei Sutou <kou@clear-code.com>
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
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# set -x
set -e

mariadb_download_base=http://mirror.jmu.edu/pub/mariadb

# export GROONGA_MASTER=yes
# export GROONGA_NORMALIZER_MYSQL_MASTER=yes

curl --silent --location https://github.com/groonga/groonga/raw/master/data/travis/setup.sh | sh
curl --silent --location https://github.com/groonga/groonga-normalizer-mysql/raw/master/data/travis/setup.sh | sh
# curl --silent --location https://github.com/clear-code/cutter/raw/master/data/travis/setup.sh | sh

if [ ! -f /usr/lib/groonga/plugins/tokenizers/mecab.so ]; then
    sudo apt-get -qq -y install groonga-tokenizer-mecab
fi

if [ "${MROONGA_BUNDLED}" = "yes" ]; then
    mkdir -p .mroonga
    mv * .mroonga/
    mv .mroonga/tools ./
    sudo apt-get -qq -y build-dep mysql-server
    # Support MariaDB for now.
    download_base=${mariadb_download_base}/${MYSQL_VERSION}
    tar_gz=${MYSQL_VERSION}.tar.gz
    curl -O ${download_base}/source/${tar_gz}
    tar xzf $tar_gz
    mv ${MYSQL_VERSION}/* ./
    rm -rf storage/mroonga
    mv .mroonga storage/mroonga
    rm -rf ${MYSQL_VERSION}
else
    mkdir -p vendor
    cd vendor

    version=$(echo "$MYSQL_VERSION" | sed -e 's/^\(mysql\|mariadb\)-//')
    series=$(echo "$version" | sed -e 's/\.[0-9]*\(-\?[a-z]*\)\?$//g')
    case "$MYSQL_VERSION" in
	mysql-*)
	    sudo apt-get -qq update
	    sudo apt-get -qq -y build-dep mysql-server
	    if [ "$version" = "system" ]; then
		sudo apt-get -qq -y install \
		    mysql-server mysql-server-5.5 mysql-server-core-5.5 \
		    mysql-testsuite libmysqld-dev
		apt-get -qq source mysql-server
		ln -s $(find . -maxdepth 1 -type d | sort | tail -1) mysql
	    else
		download_base="http://cdn.mysql.com/Downloads/MySQL-${series}/"
		if [ "$(uname -m)" = "x86_64" ]; then
		    architecture=x86_64
		else
		    architecture=i686
		fi
		deb=mysql-${version}-debian6.0-${architecture}.deb
		tar_gz=mysql-${version}.tar.gz
		curl -O ${download_base}${deb} &
		curl -O ${download_base}${tar_gz} &
		wait
		sudo apt-get -qq -y install libaio1
		sudo dpkg -i $deb
		tar xzf $tar_gz
		ln -s mysql-${version} mysql
	    fi
	    ;;
	mariadb-*)
	    sudo apt-get -qq -y remove --purge mysql-common

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
	    sudo apt-get -qq -y build-dep mariadb-server
	    sudo apt-get -qq -y install \
		mariadb-server libmariadbclient-dev mariadb-test
	    apt-get -qq source mariadb-server
	    ln -s $(find . -maxdepth 1 -type d | sort | tail -1) mysql
	    ;;
    esac

    cd ..
fi

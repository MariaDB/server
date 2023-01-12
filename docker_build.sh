# docker run -it -v $(pwd):/build/server ubuntu
apt-get update 
apt-get build-dep mariadb-server
apt-get install -y build-essential libncurses5-dev gnutls-dev bison zlib1g-dev ccache g++ cmake ninja-build vim wget

cd build
mkdir build-mariadb-server-debug
cd build-mariadb-server-debug
cmake ../server -DCONC_WITH_{UNITTEST,SSL}=OFF -DWITH_UNIT_TESTS=OFF -DCMAKE_BUILD_TYPE=Debug -DWITH_SAFEMALLOC=OFF -DWITH_SSL=bundled -DMYSQL_MAINTAINER_MODE=OFF -G Ninja

# cmake --build . --parallel 4

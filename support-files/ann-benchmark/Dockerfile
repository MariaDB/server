FROM ubuntu:22.04

WORKDIR /build

# Install dependencies for MariaDB build
RUN apt-get update
RUN apt-get install -yq --no-install-recommends libmariadb-dev devscripts equivs ccache ninja-build curl
COPY debian/control /tmp/control
RUN mk-build-deps -r -i /tmp/control -t 'apt-get -y -o Debug::pkgProblemResolver=yes --no-install-recommends'

# Install the required python lib for ann-benchmarks in advance. 
# We do NOT install the whole ann-benchmarks in docker but instead keep it in mounted volume to allow local customization to benchmark configuration.
RUN apt-get install -yq python3-numpy python3-scipy python3-pip build-essential git
# Ann-benchmarks requires python 3.10, but higher version should also work
RUN python3 -c 'import sys; exit(sys.version_info < (3,10))'
RUN curl -LO https://github.com/HugoWenTD/ann-benchmarks/raw/mariadb/requirements.txt
RUN pip3 install -r requirements.txt mariadb

WORKDIR /build/server

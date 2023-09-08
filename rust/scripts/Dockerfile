# dockerfile meant for running subcommands

FROM rust:1.72

WORKDIR /checkout
RUN apt-get update \
    && apt-get install -y \
        binutils \
        bison \
        build-essential \
        cmake \
        cmake-curses-gui \
        libboost-all-dev \
        less \
        liblz4-dev \
        liblzo2-dev \
        libsnappy-dev \
        libzstd-dev \
        ninja-build \
        sccache \
        vim-tiny \
        wget \
        xxd \
        clang

ENV RUST_BACKTRACE=1
ENV CARGO_TARGET_DIR=/obj/build-cargo
ENV BUILD_DIR=/obj/build-mariadb
ENV SCCACHE_DIR=/obj/sccache
ENV SCCACHE_CACHE_SIZE="1G"

# ^ clang has to go last because it doesn't play well with other installs

# RUN apt-get install -y \

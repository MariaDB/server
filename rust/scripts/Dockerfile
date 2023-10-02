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
        mariadb-client \
        ninja-build \
        sccache \
        vim-tiny \
        wget \
        xxd \
        clang
# ^ clang has to go last because it doesn't play well with other installs

ENV RUST_BACKTRACE=1
ENV RUST_LOG=debug
ENV CARGO_TARGET_DIR=/obj/build-cargo
ENV BUILD_DIR=/obj/build-mariadb
ENV SCCACHE_DIR=/obj/sccache
ENV SCCACHE_CACHE_SIZE="1G"

FROM rust:1.70

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
        vim-tiny \
        wget \
        xxd \
        clang \
    && export RUST_BACKTRACE=1 \
    && export CARGO_TARGET_DIR=/obj/build-cargo
    && export BUILD_DIR=/obj/build-mariadb

# ^ clang has to go last because it doesn't play well with other installs

RUN apt-get install -y \

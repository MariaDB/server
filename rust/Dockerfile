FROM rust:1.70

WORKDIR /checkout
RUN apt-get update \
    && apt-get install -y \
        binutils \
        bison \
        build-essential \
        cmake \
        cmake-curses-gui \
        less \
        libboost-all-dev \
        liblz4-dev \
        liblzo2-dev \
        libsnappy-dev \
        libzstd-dev \
        ninja-build \
        vim-tiny \
        wget \
        xxd \
    && export RUST_BACKTRACE=1 \
    && export CARGO_TARGET_DIR=/obj/build-cargo

RUN apt-get install -y \

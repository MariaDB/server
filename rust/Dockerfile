FROM rust:1.70

WORKDIR /checkout
RUN apt-get update \
    && apt-get install -y \
        binutils \
        bison \
        libboost-all-dev \
        build-essential \
        cmake \
        less \
        libzstd-dev \
        liblz4-dev \
        liblzo2-dev \
        libsnappy-dev \
        ninja-build \
        vim-tiny \
        wget \
        xxd \
    && export RUST_BACKTRACE=1

RUN apt-get install -y \

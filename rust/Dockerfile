FROM rust:1.70

WORKDIR /checkout
RUN apt-get update \
    && apt-get install -y \
        binutils \
        bison \
        build-essential \
        cmake \
        less \
        ninja-build \
        vim-tiny \
        wget \
        xxd \
    && export RUST_BACKTRACE=1
    # && apt-get build-dep mariadb-server \

# Quick test for our example plugins, build against the current repo but runs
# with the published 10.11 image
#
# ```
# # Build the image. Change the directory (../) if not building in `rust/`
# docker build -f Dockerfile ../ --tag mdb-plugin-ex
#
# # Run the container
# docker run --rm -e MARIADB_ROOT_PASSWORD=example --name mdb-plugin-ex-c \
#   mdb-plugin-ex --plugin-maturity=experimental \
#   --plugin-load=libbasic
#
# # Enter a SQL console
# docker exec -it mdb-plugin-ex-c mysql -pexample
# 
# # Install desired plugins
# INSTALL PLUGIN basic_key_management SONAME 'libbasic.so';
# INSTALL PLUGIN encryption_example SONAME 'libencryption.so';
# ```

# This build is space inefficient, but should rerun somewhat quick

FROM rust:latest AS build

WORKDIR /build

RUN apt-get update \
    # build requirements
    && apt-get install -y cmake clang bison \
    # development helpers
    xxd less vim-tiny binutils \
    && mkdir /output

COPY . .

WORKDIR /build/rust

RUN --mount=type=cache,target=/usr/local/cargo/registry \
    --mount=type=cache,target=/build/target \
    cargo build
    # -p basic
    # -p encryption
    # --release \
    # && cp target/release/*.so /output

RUN cp target/debug/*.so /output

RUN export RUST_BACKTRACE=1

FROM mariadb:10.11-rc

COPY --from=build /output/* /usr/lib/mysql/plugin/

# create database db; use db; create table t1 (id int) encrypted=yes;
# flush tables t1 for export;

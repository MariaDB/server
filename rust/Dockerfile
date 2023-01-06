# Quick test for our example plugins, build against the current repo but runs
# with the published 10.11 image
#
# ```
# # Build the image. Change the directory (../) if not building in `rust/`
# docker build -f Dockerfile ../ --tag mdb-plugin-ex
#
# # Run the container
# docker run --rm -e MARIADB_ROOT_PASSWORD=example --name mdb-plugin-ex-c mdb-plugin-ex
#
# # Enter a SQL console
# docker exec -it mdb-plugin-ex-c mysql -pexample
# ```

FROM rust:latest AS build

WORKDIR /build

RUN apt-get update \
    && apt-get install -y cmake clang bison

COPY . .

WORKDIR /build/rust

RUN --mount=type=cache,target=/usr/local/cargo/registry \
    --mount=type=cache,target=/build/target \
    cargo build --release -p examples \
    && mkdir /output \
    && cp target/release/*.so /output

FROM mariadb:10.11-rc

COPY --from=build /output/* /usr/lib/mysql/plugin/

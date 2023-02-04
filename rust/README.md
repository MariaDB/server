# Rust support for MariaDB

The purpose of this module is to be able to write plugins for MariaDB in Rust.

## Building

The Rust portion of this repository does not yet integrate with the main MariaDB
CMake build system to statically link plugins (adding this is a goal).

To build dynamically, simply run `cargo build` within this `/rust` directory.

## Testing with Docker


```sh
# Build the image. Change the directory (../) if not building in `rust/`
docker build -f Dockerfile ../ --tag mdb-plugin-ex

# Run the container
docker run --rm -e MARIADB_ROOT_PASSWORD=example --name mdb-plugin-ex-c \
  mdb-plugin-ex \
  --plugin-maturity=experimental
#   --plugin-load=libbasic \
#   --plugin-load=libencryption
#   --plugin-load=libdebug_key_management

# Enter a SQL console
docker exec -it mdb-plugin-ex-c mysql -pexample

# Install desired plugins
INSTALL PLUGIN basic_key_management SONAME 'libbasic.so';
INSTALL PLUGIN encryption_example SONAME 'libencryption_example.so';
```

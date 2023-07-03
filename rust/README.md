# Rust support for MariaDB

The purpose of this module is to be able to write plugins for MariaDB in Rust.

## Building

The Rust portion of this repository does not yet integrate with the main MariaDB
CMake build system to statically link plugins (adding this is a goal).

To build dynamically, simply run `cargo build` within this `/rust` directory.

## API Documentation

To view the API documentation, just run `cargo doc --open` within this `rust/`
directory. This can take a little bit of time to document everything (including
third party dependencies), so `--no-deps` can speed things up (alternatively,
specify a specific path to only document one crate).

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
docker exec -it mdb-plugin-ex-c mariadb -pexample

# Install desired plugins
INSTALL PLUGIN basic_key_management SONAME 'libbasic.so';
INSTALL PLUGIN encryption_example SONAME 'libencryption_example.so';
```

## Project structure

### `bindings/`

This directory contains everything needed for Rust to link to C. It used
`bindgen` at build time to generate required types.

### `examples/`

This directory contains simplified plugins using the Rust API.

### `macros/`

This directory contains procedural macros (code transforming programs that run
at build time). This implementation can look spooky so stay away if you don't
want to be scared - but it's how we get friendly APIs and nice error messages
for things like plugin registration, like this:


```rust
register_plugin! {
    DebugKeyMgmt,
    ptype: PluginType::MariaEncryption,
    name: "debug_key_management",
    author: "Trevor Gross",
    description: "Debug key management plugin",
    license: License::Gpl,
    maturity: Maturity::Experimental,
    // version: "0.2",
}
```

```
$ cargo check -p keymgt-debug
error: field 'version' is expected for encryption plugins, but not provided
       (in macro 'register_plugin')
   --> examples/keymgt-debug/src/lib.rs:92:1
    |
92  | / register_plugin! {
93  | |     DebugKeyMgmt,
94  | |     ptype: PluginType::MariaEncryption,
95  | |     name: "debug_key_management",
...   |
100 | |     // version: "0.2",
101 | | }
    | |_^
    |
    = note: this error originates in the macro `register_plugin`
```

### `mariadb/`

This contains our Rust API, a safe wrapper around the C API.

### `plugins/`

These are plugins designed to be production-ready-ish.

### `test-runner/`

Todo

We want this to be able to initialize a docker container and run tests against
it, but not sure how. Preferably using our built mariadb.

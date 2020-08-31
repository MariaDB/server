# How to create a new compression service

Suggested: read `libservices/HOWTO` first.

## Overview
Compression services use `#define`s to replace library functions with calls to a function pointer.
Eg:

If the original function call is
```c++
FOO_compress_buffer(src, srcLen, dst, dstLen);
```

then, it turns into
```c++
compression_service_foo->FOO_compress_buffer(src, srcLen, dst, dstLen);
```

with the help of defines like this
```c++
#define FOO_compress_buffer(...) compression_service_foo->FOO_compress_buffer_ptr(__VA_ARGS__)
```
This is to avoid changing external code.

On startup, the server tries to load the libraries specified in the `--use-compression` switch (defaults to all) using `dlopen`.
First, the pointers are initialized with dummy functions that return error codes to prevent a SegFault if one of those functions are used.
If the server is able to open the library and resolve all symbols successfully, then it replaces the dummy functions with the resolved ones like this

Here is an example service for a library `foo`, with two functions: `FOO_compress_buffer` and `FOO_decompress_buffer` and an `enum` `foo_ret`.

`include/compression/foo.h`

```c
#ifndef SERVICE_FOO_INCLUDED
/**
  @file include/compression/foo.h
  This service provides dynamic access to Foo.
*/

#ifdef __cplusplus
extern "C" {
#endif

// This include guard is necessary to prevent
// standard headers (which might be system-dependent) from
// being included in the ABI Check

// ABI stands for "Application Binary Interface", and is just the
// header files after processing them through the preprocessor.
// This is because if the preprocessor output changes, then old plugins
// might not load correctly

// If you need access to a non-system header, then
// include it OUTSIDE the include guard
#ifndef MYSQL_ABI_CHECK
#include <stdbool.h>
#endif

extern bool COMPRESSION_LOADED_FOO;

// Declare the additional enum, since it won't be provided by the real foo.h
enum foo_ret{
    FOO_OK             = 0,
    FOO_DST_TOO_SMALL  = 1,
    FOO_INTERNAL_ERROR = 2
};

// Add defines to avoid repetition
#define DEFINE_FOO_compress_buffer(NAME) foo_ret NAME(  \
    const char *src, int srcLen,                        \
    char *dst,       int *dstLen                        \
)

#define DEFINE_FOO_decompress_buffer(NAME) foo_ret NAME(    \
    const char *src, int srcLen,                            \
    char *dst                                               \
)

// typedef pointers
typedef DEFINE_FOO_compress_buffer   ((*PTR_FOO_compress_buffer));
typedef DEFINE_FOO_decompress_buffer ((*PTR_FOO_decompress_buffer));

// This is the struct that will hold all the function pointers
struct compression_service_foo_st{
    PTR_FOO_compress_buffer   FOO_compress_buffer_ptr;
    PTR_FOO_decompress_buffer FOO_decompress_buffer_ptr;
}

// And this is the pointer through which the correct functions will be called
extern struct compression_service_foo_st *compression_service_foo;

// Defines replace calls to the function with calls to the function pointers
#define FOO_compress_buffer(...)   compression_service_foo->FOO_compress_buffer_ptr   (__VA_ARGS__)
#define FOO_decompress_buffer(...) compression_service_foo->FOO_decompress_buffer_ptr (__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#define SERVICE_FOO_INCLUDED
#endif
```

`sql/compression/foo.cc`
```c++
// Define a status variable to keep track of the status of the library
bool COMPRESSION_LOADED_FOO = false;

// Use the defines from the header files

// This is what a dummy function might look like
DEFINE_FOO_compress_buffer(DUMMY_FOO_compress_buffer){
    // Some callers expect the function to do fulfill conditions,
    // like setting `dstLen` to a valid value
    // So, some dummy functions might have to do *dstLen = 0; to prevent errors

    // Return an error code so the caller will not use the result
    // Try to use an appropriate error code
    return FOO_INTERNAL_ERROR;
}

DEFINE_FOO_decompress_buffer(DUMMY_FOO_decompress_buffer){
    // Here, the function returns the number of bytes written
    // The exact return value might vary, but should indicate an error
    // as per that library's specification
    return 0;
}

void init_foo(struct compression_service_foo_st *handler, bool load_library){
    // Point struct to right place for static plugins
    // This needs to be done because of the way services are implemented
    // for static and dynamic plugins.
    compression_service_foo = handler;

    // The pointers are initialized to dummy functions
    compression_service_foo->FOO_compress_buffer_ptr = DUMMY_FOO_compress_buffer;
    compression_service_foo->FOO_decompress_buffer   = DUMMY_FOO_decompress_buffer;

    // Skip loading the library if it is disabled
    if(!load_library)
        return;

    // Load the Foo library dynamically
    void *library_handle = dlopen("libfoo.so", RTLD_LAZY | RTLD_GLOBAL);
    // If the library fails to load then stop
    if(!library_handle || dlerror())
        return;

    // Load the required symbols from the so file
    void *FOO_compress_buffer_ptr = dlsym(library_handle, "FOO_compress_buffer");
    void *FOO_decompress_buffer   = dlsym(library_handle, "FOO_decompress_buffer");
    // If one or more symbols fail to load then stop
    if(
        !FOO_compress_buffer_ptr ||
        !FOO_decompress_buffer
    )
        return;

    // Set the pointers to the loaded functions
    compression_service_foo->FOO_compress_buffer_ptr = (PTR_FOO_compress_buffer)   DUMMY_FOO_compress_buffer;
    compression_service_foo->FOO_decompress_buffer   = (PTR_FOO_decompress_buffer) DUMMY_FOO_decompress_buffer;

    // Finally, mark the library as loaded
    COMPRESSION_LOADED_FOO = true;
}
```

## Implement a new service
- Create a new file `include/compression/<foo>.h`.
  This file must have the same name as the header you want to replace.
  If the header is inside a subfolder (eg `<bar>/<foo>.h`) then you need to create a folder `<bar>` and put `<foo>.h` inside it.

- The template is:

```c
/* standard GPL header */

#ifndef SERVICE_<FOO>_INCLUDED
/**
  @file include/compression/<foo>.h
  This service provides dynamic access to <foo>.
*/

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdbool.h>
// add any other standard headers here
#endif

extern bool COMPRESSION_LOADED_<FOO>;

// define any structs/enums that are used

// modify as required
#define DEFINE_<foo_function_1>(NAME) int NAME( \
  int a                                         \
  const char *b                                 \
)

#define DEFINE_<foo_function_2>(NAME) int NAME()

typedef DEFINE_<foo_function_1>((*PTR_<foo_function_1>));
typedef DEFINE_<foo_function_2>((*PTR_<foo_function_2>));

struct compression_service_<foo>_st{
    PTR_<foo_function_1> <foo_function_1>_ptr;
    PTR_<foo_function_2> <foo_function_2>_ptr;
}

extern struct compression_service_<foo>_st *compression_service_<foo>;

#define <foo_function_1>(...) <foo>_handler_ptr-><foo_function_1>_ptr(__VA_ARGS__)
#define <foo_function_2>(...) <foo>_handler_ptr-><foo_function_2>_ptr(__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#define SERVICE_<FOO>_INCLUDED
#endif
```

The `foo.h` file should be self-contained, if it needs system headers then include them.
Eg: if you use `size_t` then `#include <stdlib.h>`.

It should also declare all the accompanying data structures, as necessary.
Eg: `include/compression/lzma.h` declares `enum lzma_ret`.

```c++
#include "compression_libs.h"
#include <dlfcn.h>

bool COMPRESSION_LOADED_<FOO> = false;

DEFINE_<foo_function_1>(DUMMY_<foo_function_1>){
    //return an error code
}

DEFINE_<foo_function_2>(DUMMY_<foo_function_2>){
    //return an error code
}

void init_<foo>(struct compression_service_<foo>_st *handler, bool load_library){
    //point struct to right place for static plugins
    compression_service_<foo> = handler;

    compression_service_<foo>-><foo_function_1>_ptr = DUMMY_<foo_function_1>;
    compression_service_<foo>-><foo_function_2>_ptr = DUMMY_<foo_function_2>;

    if(!load_library)
        return;

    //Load <foo> library dynamically
    void *library_handle = dlopen("lib<foo>.so", RTLD_LAZY | RTLD_GLOBAL);
    if(!library_handle || dlerror())
        return;

    void *<foo_function_1>_ptr = dlsym(library_handle, "<foo_function_1>");
    void *<foo_function_2>_ptr = dlsym(library_handle, "<foo_function_2>");
    if(
        !<foo_function_1> ||
        !<foo_function_2>
    )
        return;

    compression_service_<foo>-><foo_function_1>_ptr = (PTR_<foo_function_1>) <foo_function_1>_ptr;
    compression_service_<foo>-><foo_function_2>_ptr = (PTR_<foo_function_2>) <foo_function_2>_ptr;

    COMPRESSION_LOADED_<FOO> = true;
}
```

- add the version of your service to include/service_versions.h:
```c
    #define VERSION_compression_<foo> 0x0100
```

- create a new file libservices/compression_service_<foo>.c using the following template:
```c
    /* GPL header */
    SERVICE_VERSION compression_service_<foo> = (void*) VERSION_compression_<foo>;
```

- Define a new flag for the library in `sql/compression/compression_libs.cc`

```c
#define COMPRESSION_LOADED_<FOO>  1 << n
```

- Add it to the list of valid values in `sql/sys_vars.cc`

```c++
static const char *compression_libraries[] =
{
  "bzip2", "lz4", "lzma", "lzo", "snappy", "zlib", "zstd", "<foo>", "ALL", NULL
};
```

- Add the new header to:

`include/mysql/services.h`
```c
//Dynamic Compression Libraries
...
#include <compression/<foo>.h>
```

`sql/compression/compression_libs.h`
```c
#include <compression/<foo>.h>
```

- Add the new struct to:

`sql/compression/compression_libs.h`
```c
void init_compression(
    ...
    struct compression_service_<foo>_st   *
)
```

`sql/compression/compression_libs.cc`
```c++
void init_compression(
    ...
    struct compression_service_<foo>_st   *<foo>_handler
)
```

`sql/sql_plugin.cc`
```c++
init_compression(
    ...
    &compression_handler_<foo>
)
```

- Call the init function in `sql/compression/compression_libs.cc`
```c++
    init_<foo> (<foo>_handler,  (enabled_compression_libraries & COMPRESSION_<FOO>));
```

- Add `compression_service_<foo>.c` to
`libservices/CMakeLists.txt`
```cmake
SET(MYSQLSERVICES_SOURCES
  ...
  compression_service_<foo>.c
)
```

`libmysqld/CMakeLists.txt`
```cmake
SET(SQL_EMBEDDED_SOURCES
  ...
  ../libservices/compression_service_<foo>.c
)
```

`sql/CMakeLists.txt`
```cmake
SET(SQL_SOURCE
  #Dynamic compression
  ...
  compression/<foo>.cc   ../libservices/compression_service_<foo>.c
)
```

- Add any necessary defines to `cmake/plugin.cmake` to force the engines to build with support for the library.

- Register your service for dynamic linking in `sql/sql_plugin_services.ic` as follows:

```c
// updated in init_compression()
...
static struct compression_service_<foo>_st compression_handler_<foo> = {};
```

And add it to the list of services
```c
  { "compression_service_<foo>",   VERSION_compression_<foo>,   &compression_handler_<foo> }
```

- Increase the minor plugin ABI version in `include/mysql/plugin.h`.
   (`MARIA_PLUGIN_INTERFACE_VERSION` = `MARIA_PLUGIN_INTERFACE_VERSION + 1`)

Don't forget to update test result!
Use something like
```sh
sed -i -e "s/\b1\.15\b/-16/" $(grep -rl "\b1\.15" mysql-test)
```

- Add it to `mysql-test/var/log/compression_libs/compression.test`
  And update the tests with `./mtr --record compression_libs`

- Add the variable to the `status_vars` array in `sql/mysqld.cc`

```c
  {"Compression_loaded_<foo>",   (char*) &COMPRESSION_LOADED_<FOO>,   SHOW_BOOL},
```

- Update the ABI using `make abi_update`.

- Optionally, modify the storage engine to only use the `foo` library for compression if it is loaded, by checking the value of `COMPRESSION_LOADED_<FOO>`.

That's all!

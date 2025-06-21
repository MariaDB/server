# Building MIR

You can build MIR in source directory simply with `make` or `make
all`

You can also build MIR in a separate directory.  In this case you
need to use `make SRC_DIR=<path to MIR sources> -f <path to MIR sources>/GNUmakefile`.
**All other calls of make** should have the
same `SRC_DIR` value and `-f` argument on the make command line.

 By default MIR is built in release mode (with optimizations).  If you
want to build debugging version (without optimizations and additional
checks), you can use `make debug` instead of `make`.  Please don't
forget to remove debug version up before installing (you can do this
with `make clean all`).

# Testing and Benchmarking MIR

 Please use `make test` or `make bench` for testing and benchmarking
with the right `SRC_DIR` if you build MIR in a different directory.

# Installing MIR
 
 `make install` installs the following:

  * `libmir.a` - a static library containing MIR API functions,
    MIR generator and interpreter, and C-to-MIR compiler
  * `libmir.so.x.x.x` - a dynamic library containing MIR API functions,
    MIR generator and interpreter, and C-to-MIR compiler
  * `mir.h`, `mir-gen.h`, `c2mir.h` - include files to use MIR API functions
    and interpreter, MIR-generator, and C-to-MIR compiler
  * `c2m` - a standalone C compiler based on MIR
  * `b2m` - an utility to transform binary MIR file into textual one
  * `m2b` - an utility to transform textual MIR file into binary one
  * `b2ctab` - an utility to transform binary MIR file into C file
    containing this file as an byte array with name `mir_code`.  This utility
    can be useful to generate a standalone executable based on using
    MIR interpreter or generator

  The default destination is `/usr/local/include` for include files,
`/usr/local/lib` for the library, and `/usr/local/bin` for the
executables.

  You can change the default destination by using `PREFIX` variable on the make command line.  For example,

```
make PREFIX=/usr install
```
  will use `/usr/include`, `/usr/lib`, and `/usr/bin` for the destinations.

  `make uninstall` undoes `make install` if the both use the same `PREFIX` value.


# Cleaning

  `make clean` removes all files created during building, testing, and
benchmarking.

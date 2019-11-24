PerconaFT
======

PerconaFT is a high-performance, transactional key-value store, used in the
TokuDB storage engine for Percona Server and MySQL, and in TokuMX, the
high-performance MongoDB distribution.

PerconaFT is provided as a shared library with an interface similar to
Berkeley DB.

To build the full MySQL product, see the instructions for
[Percona/percona-server][percona-server].  This document covers PerconaFT only.

[percona-server]: https://github.com/Percona/percona-server


Building
--------

PerconaFT is built using CMake >= 2.8.9.  Out-of-source builds are
recommended.  You need a C++11 compiler, though only some versions
of GCC >= 4.7 and Clang are tested.  You also need zlib development
packages (`yum install zlib-devel` or `apt-get install zlib1g-dev`).

You will also need the source code for jemalloc, checked out in
`third_party/`.

```sh
git clone git://github.com/Percona/PerconaFT.git percona-ft
cd percona-ft
git clone git://github.com/Percona/jemalloc.git third_party/jemalloc
mkdir build
cd build
CC=gcc47 CXX=g++47 cmake \
    -D CMAKE_BUILD_TYPE=Debug \
    -D BUILD_TESTING=OFF \
    -D USE_VALGRIND=OFF \
    -D CMAKE_INSTALL_PREFIX=../prefix/ \
    ..
cmake --build . --target install
```

This will build `libft.so` and `libtokuportability.so` and install it,
some header files, and some examples to `percona-ft/prefix/`.  It will also
build jemalloc and install it alongside these libraries, you should link
to that if you are planning to run benchmarks or in production.

### Platforms

PerconaFT is supported on 64-bit Centos, Debian, and Ubuntu and should work
on other 64-bit linux distributions, and may work on OSX 10.8 and FreeBSD.
PerconaFT is not supported on 32-bit systems.

[Transparent hugepages][transparent-hugepages] is a feature in newer linux
kernel versions that causes problems for the memory usage tracking
calculations in PerconaFT and can lead to memory overcommit.  If you have
this feature enabled, PerconaFT will not start, and you should turn it off.
If you want to run with transparent hugepages on, you can set an
environment variable `TOKU_HUGE_PAGES_OK=1`, but only do this for testing,
and only with a small cache size.

[transparent-hugepages]: https://access.redhat.com/site/documentation/en-US/Red_Hat_Enterprise_Linux/6/html/Performance_Tuning_Guide/s-memory-transhuge.html


Testing
-------

PerconaFT uses CTest for testing.  The CDash testing dashboard is not
currently public, but you can run the tests without submitting them.

There are some large data files not stored in the git repository, that
will be made available soon.  For now, the tests that use these files will
not run.

In the build directory from above:

```sh
cmake -D BUILD_TESTING=ON ..
ctest -D ExperimentalStart \
      -D ExperimentalConfigure \
      -D ExperimentalBuild \
      -D ExperimentalTest
```


Contributing
------------

Please report bugs in PerconaFT to the [issue tracker][jira].

We have two publicly accessible mailing lists for TokuDB:

 - tokudb-user@googlegroups.com is for general and support related
   questions about the use of TokuDB.
 - tokudb-dev@googlegroups.com is for discussion of the development of
   TokuDB.

All source code and test contributions must be provided under a [BSD 2-Clause][bsd-2] license. For any small change set, the license text may be contained within the commit comment and the pull request. For larger contributions, the license must be presented in a COPYING.<feature_name> file in the root of the PerconaFT project. Please see the [BSD 2-Clause license template][bsd-2] for the content of the license text.

[jira]: https://jira.percona.com/projects/TDB
[bsd-2]: http://opensource.org/licenses/BSD-2-Clause/


License
-------

Portions of the PerconaFT library (the 'locktree' and 'omt') are available under the Apache version 2 license.
PerconaFT is available under the GPL version 2, and AGPL version 3.
See [COPYING.APACHEv2][apachelicense],
[COPYING.AGPLv3][agpllicense],
[COPYING.GPLv2][gpllicense], and
[PATENTS][patents].

[apachelicense]: http://github.com/Percona/PerconaFT/blob/master/COPYING.APACHEv2
[agpllicense]: http://github.com/Percona/PerconaFT/blob/master/COPYING.AGPLv3
[gpllicense]: http://github.com/Percona/PerconaFT/blob/master/COPYING.GPLv2
[patents]: http://github.com/Percona/PerconaFT/blob/master/PATENTS

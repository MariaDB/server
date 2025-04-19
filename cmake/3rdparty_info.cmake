# This file is used for SBOM generation.

# It consists of the list of 3rd party products
# which can be compiled together with MariaDB server
# and their licenses, copyright notices, and CPE prefixes
# this is the vendor:product part of CPE identifier from
# https://nvd.nist.gov/products/cpe

# We use both git submodules, and CMake external projects
# dependencies (as well we zlib, which is part of the code)
# so the information is here for all these types

SET("zlib.license" "Zlib")
SET("zlib.copyright" "Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler")
SET("zlib.cpe-prefix" "zlib:zlib")
SET("minizip.license" "Zlib")
SET("minizip.copyright" "Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler")
SET("minizip.cpe-prefix" "zlib:zlib")
SET("fmt.license" "MIT")
SET("fmt.copyright" "Copyright (C) 2012 - present, Victor Zverovich")
SET("fmt.cpe-prefix" "fmt:fmt")
SET("pcre2.license" "BSD-3-Clause")
SET("pcre2.cpe-prefix" "pcre:pcre2")
SET("wolfssl.license" "GPL-2.0")
SET("wolfssl.copyright" "Copyright (C) 2006-2024 wolfSSL Inc.")
SET("wolfssl.cpe-prefix" "wolfssl:wolfssl")
SET("boost.license" "BSL-1.0")
SET("boost.cpe-prefix" "boost:boost")
SET("mariadb-connector-c.license" "LGPL-2.1")
SET("mariadb-connector-c.cpe-prefix" "mariadb:connector\\\\/c")
SET("rocksdb.license" "GPL-2.0")
SET("wsrep-lib.license" "GPL-2.0")
SET("wsrep-api.license" "GPL-2.0")
SET("mariadb-columnstore-engine.license" "GPL-2.0")
SET("libmarias3.license" "LGPL-2.1")
SET("thrift.license" "Apache-2.0")
SET("thrift.cpe-prefix" "apache:thrift")

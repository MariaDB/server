#!/bin/sh
# dep8 smoke test for mysql-server
# Author: Robie Basak <robie.basak at canonical.com>
#
# This test should be declared in debian/tests/control with a dependency
# on the package that provides a configured MariaDB server (eg.
# mariadb-server).
#
# This test should be declared in debian/tests/control with the
# following restrictions:
# - allow-stderr (set -x always outputs to stderr)
# - needs-root (to be able to log into the database)
# - isolation-container (to be able to start service)
#
# This test:
#
# 1) Creates a test database and test user as the root user.
#
# 2) Creates a test table and checks it appears to operate normally
# using the test user and test database.
#
# 3) Checks compression support for InnoDB & RocksDB engine.

echo "Running test 'smoke'"
set -ex

# Start the daemon if it was not running. For example in Docker testing
# environments there might not be any systemd et al and the service needs to
# be started manually.
if ! command -v systemctl
then
  if ! /etc/init.d/mariadb status
  then
    echo "Did not find systemctl and daemon was not running, starting it.."
    /etc/init.d/mariadb start
  fi
else
  # If systemd (and systemctl) is available, but the service did not start, then
  # this smoke test is supposed to fail if next commands don't work.
  echo "Found systemctl, continuing smoke test.."
  # Compression plugins are separated from main server package
  # to own packages (for example LZ4 package mariadb-plugin-provider-lz4)
  # and they are installed after mariadb-server.
  # which means that they don't exist if MariaDB is not restarted
  systemctl restart mariadb
fi

mysql <<EOT
CREATE DATABASE testdatabase;
CREATE USER 'testuser'@'localhost' identified by 'testpassword';
GRANT ALL ON testdatabase.* TO 'testuser'@'localhost';
EOT

mysql testdatabase <<EOT
CREATE TABLE foo (bar INTEGER);
INSERT INTO foo (bar) VALUES (41);
EOT

result=$(echo 'SELECT bar+1 FROM foo;'|mysql --batch --skip-column-names --user=testuser --password=testpassword testdatabase)
if [ "$result" != "42" ]
then
  echo "Unexpected result" >&2
  exit 1
fi

mysql --user=testuser --password=testpassword testdatabase <<EOT
DROP TABLE foo;
EOT

mysql <<EOT
DROP DATABASE testdatabase;
DROP USER 'testuser'@'localhost';
EOT

# This will never fail but exists purely for debugging purposes in case a later
# step would fail
mariadb <<EOT
SHOW GLOBAL STATUS WHERE Variable_name LIKE 'Innodb_have_%';
EOT

mariadb <<EOT
SET GLOBAL innodb_compression_algorithm=lz4;
SET GLOBAL innodb_compression_algorithm=lzo;
SET GLOBAL innodb_compression_algorithm=lzma;
SET GLOBAL innodb_compression_algorithm=bzip2;
SET GLOBAL innodb_compression_algorithm=snappy;
SET GLOBAL innodb_compression_algorithm=zlib;
SET GLOBAL innodb_compression_algorithm=none;
EOT

# Check whether RocksDB should be installed or not
plugin=mariadb-plugin-rocksdb
if [ "$(dpkg-architecture -qDEB_HOST_ARCH_BITS)" != 32 ] &&
   [ "$(dpkg-architecture -qDEB_HOST_ARCH_ENDIAN)" = little ]
then
  dpkg-query -W $plugin

  LOG=/var/lib/mysql/#rocksdb/LOG
  # XXX: The server may only be started during the install of
  #      mariadb-server, which happens before that of the plugin.
  [ -e $LOG ] || mariadb -e "INSTALL PLUGIN RocksDB SONAME 'ha_rocksdb';"
  # XXX: rocksdb_supported_compression_types variable does not report ZSTD.

  # Print RocksDB supported items so test log is easier to debug
  grep -F " supported:" $LOG

  # Check that the expected compression methods are supported
  for a in LZ4 Snappy Zlib ZSTD
  do
    if ! grep -qE "k$a(Compression)? supported: 1" $LOG
    then
      # Fail with explicit error message
      echo "Error: Compression method $a not supported by RocksDB!" >&2
      exit 1
    fi
  done
else
  if dpkg-query -W $plugin
  then
    echo "Error: Plugin $plugin was found even though it should not exist on a 32-bit and little-endian system"
    exit 1
  fi
fi

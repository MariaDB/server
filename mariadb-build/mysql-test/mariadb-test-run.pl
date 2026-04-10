#!/usr/bin/env perl
# Call mtr in out-of-source build
$ENV{MTR_BINDIR} = '/Users/aryankancherla/Downloads/DLAB_MariaDB/server/mariadb-build';
chdir('/Users/aryankancherla/Downloads/DLAB_MariaDB/server/mysql-test');
exit(system($^X, '/Users/aryankancherla/Downloads/DLAB_MariaDB/server/mysql-test/mariadb-test-run.pl', @ARGV) >> 8);

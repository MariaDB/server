package My::Suite::Rocksdb;

use My::SysInfo;

#
# Note: ../rocksdb_sys_vars/suite.pm file has a similar
#  function. If you modify this file, consider modifying that one, too.
#
@ISA = qw(My::Suite);
use My::Find;
use File::Basename;
use strict;

#sub is_default { not $::opt_embedded_server }

my $sst_dump=
::mtr_exe_maybe_exists(
  "$::bindir/storage/rocksdb$::multiconfig/sst_dump",
  "$::path_client_bindir/sst_dump");
return "RocksDB is not compiled, no sst_dump" unless $sst_dump;
$ENV{MARIAROCKS_SST_DUMP}="$sst_dump";

## Temporarily disable testing under valgrind, due to MDEV-12439
#return "RocksDB tests disabled under valgrind" if ($::opt_valgrind);


bless { };


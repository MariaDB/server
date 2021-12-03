package My::Suite::Rocksdb_sys_vars;

#
# Note: The below is copied from ../rocksdb/suite.pm
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

bless { };


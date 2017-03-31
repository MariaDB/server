package My::Suite::Rocksdb;

@ISA = qw(My::Suite);

sub is_default { not $::opt_embedded_server }

my ($sst_dump) = grep { -x "$_/sst_dump" } "$::bindir/storage/rocksdb", $::path_client_bindir;
return "RocksDB is not compiled, no sst_dump" unless $sst_dump;
$ENV{MARIAROCKS_SST_DUMP}="$sst_dump/sst_dump";

bless { };


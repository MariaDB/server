package My::Suite::TokuDB;
use File::Basename;
@ISA = qw(My::Suite);

# Ensure we can run the TokuDB tests even if hugepages are enabled
$ENV{TOKU_HUGE_PAGES_OK}=1;
my $exe_tokuftdump=
    ::mtr_exe_maybe_exists(
           ::vs_config_dirs('storage/tokudb/PerconaFT/tools', 'tokuftdump'),
           "$::path_client_bindir/tokuftdump",
           "$::bindir/storage/tokudb/PerconaFT/tools/tokuftdump");
$ENV{'MYSQL_TOKUFTDUMP'}= ::native_path($exe_tokuftdump);

return "Doesn't work with tcmalloc" if $ENV{LD_PRELOAD} =~ /tcmalloc/ and
                                       $mysql_version_id < 100000;

#return "Not run for embedded server" if $::opt_embedded_server;
return "No TokuDB engine" unless $ENV{HA_TOKUDB_SO} or $::mysqld_variables{tokudb};

sub is_default { not $::opt_embedded_server }

bless { };


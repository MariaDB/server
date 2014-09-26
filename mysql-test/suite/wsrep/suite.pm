package My::Suite::WSREP;
use File::Basename;
use My::Find;

@ISA = qw(My::Suite);

return "Not run for embedded server" if $::opt_embedded_server;

return "WSREP is not compiled in" unless defined $::mysqld_variables{'wsrep-on'};

my ($provider) = grep { -f $_ } $ENV{WSREP_PROVIDER},
                                "/usr/lib/galera/libgalera_smm.so",
                                "/usr/lib64/galera/libgalera_smm.so";

return "No wsrep provider library" unless -f $provider;

$ENV{WSREP_PROVIDER} = $provider;

my ($path) = grep { -f "$_/wsrep_sst_rsync"; } "$::bindir/scripts", $::path_client_bindir;

return "No SST scripts" unless $path;

push @::global_suppressions,
  (
     qr(WSREP:.*down context.*),
     qr(WSREP: Failed to send state UUID:.*),
     qr(WSREP: wsrep_sst_receive_address.*),
     qr(WSREP: Could not open saved state file for reading: .*),
     qr(WSREP: last inactive check more than .* skipping check),
     qr(WSREP: Gap in state sequence. Need state transfer.),
     qr(WSREP: Failed to prepare for incremental state transfer: .*),
   );


$ENV{PATH}="$path:$ENV{PATH}";

bless { };


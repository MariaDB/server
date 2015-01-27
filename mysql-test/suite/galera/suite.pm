package My::Suite::GALERA;
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

my ($spath) = grep { -f "$_/wsrep_sst_rsync"; } "$::bindir/scripts", $::path_client_bindir;
return "No SST scripts" unless $spath;

my ($epath) = grep { -f "$_/my_print_defaults"; } "$::bindir/extra", $::path_client_bindir;
return "No my_print_defaults" unless $epath;

push @::global_suppressions,
  (
     qr(WSREP: Failed to guess base node address),
     qr(WSREP: Guessing address for incoming client connections failed),
     qr(WSREP: wsrep_sst_receive_address is set to '127.0.0.1),
     qr(WSREP: Could not open saved state file for reading: ),
     qr(WSREP: Gap in state sequence. Need state transfer.),
     qr(WSREP: Failed to prepare for incremental state transfer:),
     qr(WSREP:.*down context.*),
     qr(WSREP: Failed to send state UUID:),
     qr(WSREP: last inactive check more than .* skipping check),
     qr(WSREP: SQL statement was ineffective),
     qr(WSREP: Releasing seqno [0-9]* before [0-9]* was assigned.),
   );


$ENV{PATH}="$epath:$ENV{PATH}";
$ENV{PATH}="$spath:$ENV{PATH}" unless $epath eq $spath;

bless { };

